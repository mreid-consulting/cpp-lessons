---
title: Algorithms, Iterators and Ranges
part: Part I - Foundations
summary: The standard algorithms worth knowing on a trading system, why lower_bound on a vector beats map::find, and where C++20 ranges and lazy views help you and where they quietly cost you.
time: 30 min
level: intermediate
tags: algorithms, iterators, ranges, views, projections
---

The standard algorithms are the part of the library that survives contact with a latency
budget. They contain no allocation, they are written by people who read the generated
assembly, and they are the same code the compiler's vectoriser has been tuned against. The
ranges layer added in C++20 makes them pleasant to call; it also introduces a lazy
evaluation model with cost characteristics that are worth understanding before you put one
in a feed handler.

## Iterators and what algorithms demand of them

An **iterator** is anything that behaves like a pointer: dereference it, advance it,
compare it. Algorithms are written against **categories**, which say what operations are
available and therefore what the algorithm may do.

| Category | Can do | Example | Algorithms it unlocks |
|---|---|---|---|
| input | read once, advance | `std::istream_iterator` | `find`, `for_each`, `accumulate` |
| forward | read repeatedly, advance, multi-pass | `std::forward_list` | `search`, `unique` |
| bidirectional | also step backwards | `std::list`, `std::map` | `reverse`, `next_permutation` |
| random access | `it + n` and `b - a` in O(1) | `std::deque` | `sort`, `nth_element`, `binary_search` |
| contiguous | also: elements adjacent in memory | `std::vector`, `std::array`, `std::span` | anything that can `memcpy` or vectorise |

**Contiguous** was split out of random access in C++20 precisely because it is the property
that permits vectorisation and bulk memory moves. It is why `std::copy` on a
`std::vector<int>` compiles to a `memmove` and on a `std::deque<int>` does not.

A `std::map` iterator is bidirectional, so `std::sort` on a map does not merely perform
badly, it does not compile.

## Prefer the algorithm to the loop

```cpp
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

// Hand-written: correct today, and a place for an off-by-one to appear at the next edit.
const Order* find_by_id_loop(const std::vector<Order>& v, std::uint64_t id) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].id == id) return &v[i];
    }
    return nullptr;
}

// Standard: states the intent, and the vectoriser has seen this shape before.
const Order* find_by_id(const std::vector<Order>& v, std::uint64_t id) {
    auto it = std::ranges::find(v, id, &Order::id);
    return it == v.end() ? nullptr : &*it;
}
```

The argument is not primarily about speed, though the standard versions are at least as
fast and often better vectorised. It is that `std::ranges::find` cannot be off by one,
cannot iterate the wrong container, and tells the next reader in one token what the loop
does. You will occasionally beat the library by hand; be ready to show the measurement.

### The algorithms that earn their place

```cpp
#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <vector>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

std::int64_t total_notional(const std::vector<Order>& v) {
    return std::transform_reduce(v.begin(), v.end(), std::int64_t{0}, std::plus<>{},
                                 [](const Order& o) {
                                     return o.price_ticks * static_cast<std::int64_t>(o.qty);
                                 });
}

// Partition working orders to the front; returns the boundary. One pass, no allocation.
auto split_working(std::vector<Order>& v) {
    return std::partition(v.begin(), v.end(),
                          [](const Order& o) { return o.qty > 0; });
}

// The ten largest orders, in no particular order among themselves. O(n), not O(n log n).
void top_ten_by_qty(std::vector<Order>& v) {
    if (v.size() > 10) {
        std::nth_element(v.begin(), v.begin() + 10, v.end(),
                         [](const Order& a, const Order& b) { return a.qty > b.qty; });
    }
}
```

- `find` / `find_if` — a linear scan the compiler vectorises on contiguous data.
- `lower_bound` / `upper_bound` — binary search on sorted data. `lower_bound` returns the
  first element not less than the key, `upper_bound` the first strictly greater; together
  they bracket a run of equal keys, which is what a price ladder needs.
- `partition` / `stable_partition` — reorder in one pass by a predicate, without allocating
  a second vector.
- `nth_element` — put the nth element where sorting would put it, everything smaller before
  it. Linear on average, and the right tool for a top-k whose internal order does not matter.
- `sort` — introsort: quicksort falling back to heapsort, insertion sort for small runs.
  `std::stable_sort` is mergesort and allocates a temporary buffer, so it stays off the hot
  path.
- `accumulate` / `reduce` / `transform_reduce` — `accumulate` is strictly left-to-right and
  so not vectorisable for floating point; `reduce` is unordered and can be. For integer
  ticks the distinction changes the codegen, not the result.
- `transform`, `copy_n`, `rotate` — bulk operations that lower to `memcpy`/`memmove` on
  contiguous trivially copyable data.

## Why lower_bound on a vector beats map::find

Both are O(log n). One is a binary search over a contiguous array; the other is a descent
through separately allocated nodes.

```cpp ladder_lookup.cpp
#include <algorithm>
#include <cstdint>
#include <print>
#include <vector>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

// Precondition: levels is sorted ascending by price_ticks.
const Level* find_level(const std::vector<Level>& levels, std::int64_t px) noexcept {
    auto it = std::ranges::lower_bound(levels, px, {}, &Level::price_ticks);
    return (it != levels.end() && it->price_ticks == px) ? &*it : nullptr;
}

int main() {
    std::vector<Level> levels;
    levels.reserve(256);
    for (std::int64_t p = 10'000; p < 10'256; ++p) levels.push_back({p, 100});
    const Level* l = find_level(levels, 10'128);
    std::print("{}\n", l ? l->qty : 0);
}
```

For 256 levels the whole array is 4 KiB, 64 cache lines, comfortably in L1. The eight
probes of the binary search touch at most eight lines, and the last few land inside a line
already loaded. The equivalent `std::map` does eight pointer chases to eight addresses the
allocator scattered across the heap.

The measured gap for a few hundred to a few thousand elements is typically **3x to 5x** in
favour of the sorted vector, and it widens as the tree's nodes age and spread. The tree wins
only when insertion and erasure are frequent enough for the vector's O(n) shifting to
dominate, and a `memmove` of 4 KiB is on the order of 100 ns.

`std::ranges::lower_bound(levels, px, {}, &Level::price_ticks)` uses two ranges features at
once. The third argument is the comparator, and `{}` means the default `std::ranges::less`.
The fourth is a **projection**.

## Ranges, projections and views

The `std::ranges::` algorithms take a range directly rather than an iterator pair, and
accept a projection applied to each element before the comparator or predicate sees it.

```cpp
#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

void sorting(std::vector<Order>& v) {
    std::sort(v.begin(), v.end(),                                  // classic
              [](const Order& a, const Order& b) { return a.price_ticks < b.price_ticks; });

    std::ranges::sort(v, {}, &Order::price_ticks);                 // ranges + projection
    std::ranges::sort(v, std::ranges::greater{}, &Order::qty);     // descending by qty
}
```

A projection is a pointer-to-member, a pointer-to-function, or any callable. It removes the
comparator body for the common "order by this field" case, and because a pointer-to-member
is a compile-time constant it inlines exactly as the lambda would.

A **view** is a lazy, non-owning range. Composing views builds a pipeline that does no work
until something iterates it, and allocates nothing.

```cpp views_pipeline.cpp
#include <algorithm>
#include <cstdint>
#include <print>
#include <ranges>
#include <vector>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

int main() {
    std::vector<Order> book;
    for (std::uint64_t i = 0; i < 1'000; ++i) {
        book.push_back({i, 10'000 + static_cast<std::int64_t>(i % 50), (i % 7) * 100});
    }

    auto top_five_notionals = book
        | std::views::filter([](const Order& o) { return o.qty > 0; })
        | std::views::transform([](const Order& o) {
              return o.price_ticks * static_cast<std::int64_t>(o.qty);
          })
        | std::views::take(5);

    for (std::int64_t n : top_five_notionals) std::print("{}\n", n);
}
```

Nothing is computed until the loop runs, and then only five orders are transformed however
large the book is. That short-circuiting is the real win over building intermediate vectors.

C++23 adds the pieces that were most conspicuously missing:

```cpp
#include <cstdint>
#include <print>
#include <ranges>
#include <vector>

int main() {
    std::vector<std::int64_t> bids{10'050, 10'049, 10'048, 10'047};
    std::vector<std::uint32_t> qtys{200, 150, 400, 50};

    for (auto [px, q] : std::views::zip(bids, qtys)) {            // parallel iteration
        std::print("{} x {}\n", px, q);
    }

    for (auto [i, px] : std::views::enumerate(bids)) {            // index without a counter
        std::print("level {} at {}\n", i, px);
    }

    for (auto window : std::views::slide(bids, 2)) {              // adjacent pairs
        std::print("spread {}\n", window.front() - window.back());
    }

    auto top = bids | std::views::take(2) | std::ranges::to<std::vector>();  // materialise
    std::print("{}\n", top.size());
}
```

`std::views::chunk(r, n)` splits a range into fixed-size groups, which is how you batch
orders into wire messages. `std::ranges::to<Container>()` is the long-missing way to turn a
pipeline back into a real container in one expression.

Library support for these lags the language: `zip` and `ranges::to` are widely available,
while `enumerate`, `slide` and `chunk` are still missing from some standard libraries that
otherwise claim C++23. Check `__cpp_lib_ranges_enumerate` and its siblings rather than
assuming.

## Where views cost you

:::pitfall
`std::views::filter` must find the first matching element to know where the view begins, so
`begin()` is O(n) in the worst case. The standard therefore requires it to **cache** the
result of the first `begin()` call. Two consequences: `begin()` is not `const`, so a
`filter_view` is not a `const` range and cannot be passed as `const auto&`; and if the
underlying range changes after you have called `begin()`, the cached iterator is stale and
using the view again is undefined behaviour. Build filter pipelines where you consume them,
and do not store them across mutations.
:::

:::perf
Three real costs, all of which you should confirm with your own compiler and flags:

- **Inlining depth.** A pipeline of four views is four layers of iterator wrappers, each
  with its own `operator++` and `operator*`. At `-O2` GCC and Clang normally flatten this
  to the same loop you would have written, but "normally" is not "always": long pipelines,
  or views over a `std::deque`, sometimes leave a layer un-inlined and the loop is then
  noticeably slower than the hand-written form. Read the assembly for anything in a hot
  loop.
- **The debug cliff.** At `-O0` every one of those wrapper calls is a real call. A views
  pipeline can be **10x to 50x** slower in an unoptimised build than in an optimised one,
  far worse than the usual 3x debug penalty. If your team debugs at `-O0`, a views-heavy
  hot path becomes untestable at realistic message rates.
- **Vectorisation.** A plain loop over a `std::vector<std::int64_t>` vectorises. The same
  loop behind a `filter_view` cannot, because the trip count is not known and the accesses
  are conditional. `transform_view` over a contiguous range usually still vectorises;
  `filter_view` essentially never does.
:::

## Execution policies

Passing `std::execution::par` or `par_unseq` as an extra first argument to `std::sort`,
`std::for_each` and friends asks the library to parallelise. That is useful offline:
end-of-day analytics, backtests, reconciliation over millions of fills. It does not belong
on a hot path, because it hands work to a thread pool you do not control, on cores you have
pinned other things to, with a scheduling latency in microseconds and a tail in
milliseconds. On a tick-to-trade path you want one thread on one core doing one thing, and
lesson 35 explains why.

:::hft Where this actually lands on a desk
Two habits from this lesson survive into production code on a trading system, and the rest
is mostly for the tooling around it.

The first is `lower_bound` over a sorted, reserved `std::vector` as the default lookup for
anything ordered. A price ladder, a symbol table, a set of active order ids: sorted vector,
binary search, no allocation, everything in L1. It is the single most common structural
difference between a fast book and a slow one.

The second is projections. `std::ranges::sort(orders, {}, &Order::price_ticks)` is not a
performance change over the lambda, it is a correctness one: there is no comparator body to
get backwards, and a comparator that is not a strict weak ordering is undefined behaviour
that `std::sort` expresses as a segfault deep inside the library at 3am.

Views are the part to be selective about. Use them freely in analytics, replay tooling and
test harnesses, where composability is worth more than nanoseconds. In the tick handler,
write the loop, and let the reviewer see the loop.
:::

:::exercise
Compute the total notional of orders with `qty > 0` over a million-entry
`std::vector<Order>`, three ways: a hand-written indexed loop, `std::transform_reduce` with
the predicate baked into the lambda, and a `views::filter | views::transform` pipeline
consumed by `std::ranges::fold_left`. Time all three at `-O2`, then at `-O0`. Then compile
the `-O2` versions with `-S -masm=intel` and check which contain SIMD instructions. Explain
the `-O0` result to someone who has not read this lesson.
:::

## Takeaways

- Iterator categories are a contract. Contiguous, new in C++20, is the one that permits
  `memcpy` and vectorisation, which is why hot-path containers should provide it.
- Reach for a standard algorithm before a loop. It states the intent, it cannot be
  off-by-one, and it is the shape the vectoriser was tuned on.
- `lower_bound` over a sorted `std::vector` typically beats `std::map::find` by 3-5x at a
  few hundred to a few thousand elements, because eight probes into 4 KiB are eight L1 hits
  and eight pointer chases are not.
- Projections remove the comparator body for field-based ordering, which is a correctness
  win as much as an ergonomic one.
- Views are lazy, allocation-free and composable. `filter` caches its `begin`, so a
  `filter_view` is not `const` and must not outlive a mutation of its source.
- Views are 10-50x slower in an unoptimised build and block vectorisation behind a filter.
  Use them in tooling; write the loop in the tick handler. Execution policies belong
  offline.
