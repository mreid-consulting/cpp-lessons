---
title: The STL Containers, Honestly
part: Part I - Foundations
summary: What each standard container is actually made of, what it costs in cache misses rather than in big-O, and which of them belong anywhere near a hot path.
time: 35 min
level: intermediate
tags: vector, map, unordered_map, cache, flat_map
---

Complexity guarantees are the wrong lens for this material. `std::map::find` and
`std::unordered_map::find` are O(log n) and O(1), and on a large table the O(log n) one is
sometimes the faster. Asymptotics count *operations*; your latency is determined by *cache
misses*, and the standard says nothing about those. So each container below is described
by its memory layout first.

The numbers to keep in your head, developed properly in lesson 19: an L1 hit is about
1 ns, L2 about 4 ns, L3 about 15 ns, and main memory 80-100 ns. A cache miss costs roughly
what a hundred arithmetic instructions cost.

## Contiguous storage: vector, array and string

### std::vector

One heap allocation holding elements contiguously, plus three pointers: begin, end, and
end-of-capacity. `sizeof(std::vector<T>)` is 24 on x86-64 regardless of `T`.

```cpp
#include <cstdint>
#include <print>
#include <vector>

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

int main() {
    std::vector<Order> orders;
    orders.reserve(1024);                        // one allocation, then never again
    for (std::uint64_t i = 0; i < 1024; ++i) {
        orders.push_back(Order{i, 10'000 + static_cast<std::int64_t>(i), 100});
    }
    std::print("size {} capacity {}\n", orders.size(), orders.capacity());
}
```

When `size() == capacity()`, `push_back` allocates a new buffer, relocates every element
(moving them if the move is `noexcept`, per lesson 7), and frees the old one. The **growth
factor** is 2 in libstdc++ and libc++, 1.5 in MSVC. Growth is amortised O(1), which is true
and irrelevant: the one `push_back` that triggers it costs an allocation plus n
relocations, and that is the one that lands on your tick. `reserve` is the entire
mitigation.

Contiguity matters more than any of that. Iterating a vector is a linear scan the hardware
prefetcher recognises immediately, so after the first miss per 64-byte cache line the rest
are free.

### std::array

`std::array<T, N>` is a struct wrapping a C array: no allocation, no pointer, `sizeof` is
`N * sizeof(T)` exactly. The size is a compile-time constant, so loops over it unroll and
vectorise.

```cpp
#include <array>
#include <cstddef>
#include <cstdint>

// A book with a fixed number of levels: no allocation, no indirection, one cache-friendly blob.
struct BookSide {
    static constexpr std::size_t kLevels = 32;
    std::array<std::int64_t, kLevels>  price_ticks{};
    std::array<std::uint32_t, kLevels> qty{};
    std::uint32_t depth = 0;
};
```

This is the default hot-path container. If you know a bound, use it.

## Segmented and linked: deque, list, forward_list

### std::deque

A **segmented** structure: an array of pointers to fixed-size blocks. `push_front` and
`push_back` are both O(1) and neither invalidates references to existing elements, which is
its one genuine selling point.

It is **not contiguous**, so `&d[0] + 1` is not `&d[1]` and `std::span` will not take it.
Every `operator[]` is two indirections: block table lookup, then block. The block size is a
surprise: libstdc++ uses 512 bytes or one element, whichever is larger, so a
`std::deque<BookSide>` of 512-byte elements allocates one block per element, and an empty
`deque` already allocates its block table. Use it off the hot path when you need stable
references and growth at both ends. Otherwise use a `std::vector` or a ring buffer.

### std::list and std::forward_list

A doubly and singly linked list of individually allocated nodes. Each node is the element
plus one or two pointers, from a separate `operator new` call.

The cost model is brutal and simple: **one allocation per insert, one cache miss per
element traversed**. Nothing prefetches, because the address of the next node is only known
once the current node has arrived. The measured gap against a `std::vector` scan is commonly
**20x to 50x**.

Its O(1) splice and O(1) insert-at-iterator are real, but you had to traverse to that
iterator and traversal is the expensive part. `std::list` is essentially never the right
answer; an intrusive list over nodes you allocated contiguously yourself occasionally is.

## Ordered lookup: map, set and flat_map

### std::map and std::set

A red-black tree. Every element is a separately allocated node containing the key, the
value, three pointers and a colour bit. Lookup descends the tree, and **every level is a
pointer chase to an unrelated address**.

A tree of one million elements is about 20 levels deep. The top few levels stay resident,
the bottom ten or so are cold, so a lookup is roughly ten memory accesses. That puts
`std::map::find` on a large table at **100-200 ns**, dominated by where the allocator put
each node rather than by the comparison count.

```cpp
#include <cstdint>
#include <functional>
#include <map>

// Ordered iteration is the reason to reach for this, not lookup speed.
using LadderByPrice = std::map<std::int64_t, std::uint32_t, std::greater<>>;
```

The real reason to use it is sorted keys *plus* references and iterators that survive
insertion and erasure elsewhere. That combination is genuine, and it is why order books
handing out stable level pointers sometimes still use a `std::map`. Without the stability
requirement, `std::flat_map` below is the same interface with a tenth of the misses.

## Hashed lookup: unordered_map and open addressing

The standard mandates a specific design: a **bucket array** of pointers, each bucket
heading a singly linked list of separately allocated nodes. The mandate follows from
requiring references to elements to survive a rehash, and from the bucket interface being
public API.

That design costs at least two dependent memory accesses per lookup, even with a perfect
hash and no collisions:

1. Hash the key, mask to a bucket index, load the bucket pointer — one miss.
2. Follow the pointer to the node, compare the key — a second miss, to a completely
   unrelated address chosen by the allocator.

And because libstdc++ uses a prime bucket count, mapping the hash to a bucket is a real
integer `div`, adding 20-40 cycles.

:::warn
A `std::unordered_map` lookup on a large table typically measures **60-120 ns**, which is
not obviously better than the tree it was supposed to beat. The O(1) is honest. The
constant is two cache misses and a division.
:::

**Open addressing** fixes this. Store entries inline in one flat array and resolve
collisions by probing nearby slots, so a lookup is one cache line fetch plus, usually, a
single SIMD comparison against several stored hash fragments. Abseil's `flat_hash_map`,
Boost's `unordered_flat_map` and hand-rolled robin-hood tables all work this way and all
measure roughly **15-30 ns** for the same lookup, a 3-5x improvement from layout alone. The
price is that insertion invalidates every reference, because elements move.

For a bounded universe of instruments the right answer is usually neither: a dense integer
instrument id indexing straight into a `std::vector`, which is one load.

### std::string and the small string optimisation

A `std::string` in libstdc++ is 32 bytes: pointer, size, and a 16-byte union that is either
a capacity field or an inline buffer. The **small string optimisation** (SSO) stores up to
15 characters inside the object itself, with no allocation. libc++ uses a 24-byte layout
with a 22-character inline capacity.

```cpp
#include <print>
#include <string>

int main() {
    std::string short_sym = "ESZ5";              // no allocation: SSO
    std::string long_key  = "CME:ES:20251219:FUT:CONTINUOUS";  // heap allocation
    std::print("{} {}\n", sizeof(std::string), short_sym.capacity());
}
```

Ticker symbols fit in SSO, so a `std::string` symbol is not automatically a disaster. It is
still 32 bytes where a `std::array<char, 8>` would be 8, and still not trivially copyable,
which disqualifies it from anything you memcpy onto the wire. On the hot path use a
fixed-size character array, or `std::string_view` from lesson 16 to view one.

### std::flat_map and std::flat_set (C++23)

These are **adapters**, not new data structures: a `std::flat_map` holds two sorted
vectors, one of keys and one of values, and implements the `std::map` interface over them
by binary search.

```cpp
#include <cstdint>
#include <flat_map>
#include <print>

int main() {
    std::flat_map<std::int64_t, std::uint32_t> ladder{
        {10'050, 200}, {10'051, 150}, {10'052, 400}
    };
    if (auto it = ladder.find(10'051); it != ladder.end()) {
        std::print("qty at 10051 is {}\n", it->second);
    }
}
```

You gain contiguous keys: the binary search touches a handful of cache lines rather than
one per tree level, and a small book's whole key array fits in a few of them. You lose O(1)
insertion, because the vectors shift, and every iterator and reference with it.

For a price ladder of tens to low hundreds of levels, read far more often than written, the
flat version wins on every access and the O(n) insert is a `memmove` of a few hundred bytes.
That is the shape of most trading data. `<flat_map>` is one of the later C++23 library
additions, so check your standard library version; until it lands, a sorted
`std::vector<std::pair<K, V>>` with `std::lower_bound` is the same thing by hand.

## Complexity versus what you measure

Figures are order-of-magnitude, for a table or container of roughly one million elements on
a modern x86-64 server, and are there to be re-measured on your hardware.

| Container | Lookup big-O | Memory accesses per lookup | Typical measured lookup | Iteration per element |
|---|---|---|---|---|
| `std::vector` (index) | O(1) | 1 | ~1-5 ns | ~0.5-2 ns, prefetched |
| `std::vector` + `lower_bound` | O(log n) | ~log2(n) lines, mostly cached at the top | ~40-80 ns | as above |
| `std::array` | O(1) | 1, often already resident | ~1 ns | ~0.5 ns |
| `std::deque` | O(1) | 2 | ~5-15 ns | ~2-5 ns |
| `std::list` | O(n) | n | do not | ~80-100 ns |
| `std::map` | O(log n) | ~10 cold levels | ~100-200 ns | ~80-100 ns |
| `std::unordered_map` | O(1) | 2 plus a `div` | ~60-120 ns | ~80-100 ns |
| open-addressed flat hash | O(1) | 1, usually | ~15-30 ns | ~2-5 ns |
| `std::flat_map` | O(log n) | ~log2(n) but contiguous | ~30-60 ns | ~0.5-2 ns |

:::key
Big-O tells you how cost grows, not what it is. At the sizes a trading system uses the
constant factor is a memory access, so the container touching fewer cache lines wins even
when it does asymptotically more work.
:::

### Iterator invalidation

Getting this wrong is a use-after-free that usually survives testing.

| Container | Insert invalidates | Erase invalidates |
|---|---|---|
| `std::vector` | all iterators and references if reallocating; otherwise those at or after the point | those at or after the erased element |
| `std::array` | n/a | n/a |
| `std::deque` | all *iterators*; references stay valid | iterators and references to erased elements; erasing at either end invalidates only those |
| `std::list`, `std::forward_list` | nothing | only the erased element |
| `std::map`, `std::set` | nothing | only the erased element |
| `std::unordered_map`, `std::unordered_set` | iterators on rehash; references never | only the erased element |
| `std::flat_map`, `std::flat_set` | everything | everything at or after the point |
| `std::string` | as `std::vector` | as `std::vector` |

The classic bug, and its fix:

```cpp
#include <cstdint>
#include <vector>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

void wrong(std::vector<Order>& v) {
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->qty == 0) v.erase(it);              // it is now dangling; ++it is UB
    }
}

void right(std::vector<Order>& v) {
    std::erase_if(v, [](const Order& o) { return o.qty == 0; });   // C++20, one pass
}
```

## Choosing for the hot path

| You need | Use | Not |
|---|---|---|
| A bounded sequence | `std::array<T, N>` | `std::vector` |
| A grown-once sequence | `std::vector<T>` with `reserve` | `std::deque` |
| A queue between two threads | a power-of-two ring buffer (lesson 33) | `std::queue<std::deque>` |
| Lookup by dense integer id | `std::vector<T>` indexed directly | `std::unordered_map` |
| Lookup by sparse key, hot | open-addressed flat hash | `std::unordered_map` |
| Ordered price levels, small | `std::flat_map` or sorted `std::vector` | `std::map` |
| Ordered levels needing stable references | `std::map`, or an intrusive structure | `std::flat_map` |
| A short symbol | `std::array<char, 8>` | `std::string` |
| Anything at all | something contiguous | `std::list` |

:::hft The container question in a code review
When a reviewer on a trading desk asks "why is there a map here", they are asking three
things at once. Does this allocate, and on whose thread? How many cache lines does one
operation touch? Are the elements laid out so the next access is predictable?

`std::map` and `std::unordered_map` fail all three: a node allocation per insert, a pointer
chase per level or bucket, and addresses chosen by the allocator rather than by you. They
are not bad containers. They solve a problem — arbitrary growth with stable references and
no capacity planning — that a hot path has already solved differently, by knowing its
bounds in advance and preallocating.

So the rule most desks converge on: on the hot path every container is a `std::array`, a
`std::vector` reserved at startup, or a ring buffer. Node-based containers live in the
config loader, the reference data cache and the reconciliation tooling, where a hundred
nanoseconds is not a number anybody measures.
:::

:::exercise
Build a table of one million `std::int64_t` keys mapped to `std::uint32_t` quantities in
four containers: `std::map`, `std::unordered_map`, a sorted `std::vector<std::pair<...>>`
searched with `std::lower_bound`, and a plain `std::vector<std::uint32_t>` indexed by a
dense id. Time one million random lookups in each at `-O2`, accumulating into a volatile sum
so nothing is elided. Then repeat with a thousand keys. The ranking changes between the two
sizes; explain why in terms of which level of cache each structure fits into.
:::

## Takeaways

- Judge a container by its memory layout first. Contiguous beats node-based at every size
  that fits in a cache hierarchy, which is every size you will use.
- `std::vector` with `reserve` is the default. Its growth is amortised O(1), and the
  amortisation is exactly the spike you cannot afford.
- `std::list` costs an allocation per element and a cache miss per traversal step.
- `std::map` is a pointer chase per level, roughly 100-200 ns on a large tree.
  `std::unordered_map`'s mandated bucket-plus-node design costs two misses and a division;
  open-addressed alternatives measure 3-5x faster.
- C++23's `std::flat_map` gives the `std::map` interface over sorted vectors: far fewer
  misses, O(n) insertion, total iterator invalidation.
- SSO gets you 15 or 22 characters free, but `std::string` is not trivially copyable and
  does not belong in a wire struct.
