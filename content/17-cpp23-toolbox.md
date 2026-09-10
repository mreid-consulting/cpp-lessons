---
title: The C++23 Toolbox
part: Part II - Modern C++23
summary: The C++23 features a latency engineer actually reaches for - flat_map, deducing this, [[assume]], byteswap, and the standard-blessed way to read a network buffer.
time: 30 min
level: advanced
tags: cpp23, flat_map, assume, deducing-this, lifetime
---

C++23 is a small release, and most of what it adds is library polish you will never notice.
But four or five of its features change how a hot path is written, and one of them finally
makes legal a thing every feed handler in the world has been doing illegally since 1998.
This lesson is a tour of the parts worth learning, in rough order of how often you will
use them.

## Printing, and where it does not belong

`<print>` gives you `std::print` and `std::println`, which are `std::format` plus an
efficient write to a `FILE*`. They are type-safe, they do not need a format-string/argument
match to be checked by a compiler extension, and unlike `iostream` they do not drag in a
locale-aware virtual dispatch chain for every value.

```cpp
#include <print>
#include <cstdint>

int main() {
    std::int64_t price_ticks = 1'234'50;
    std::uint32_t qty = 500;
    std::println("bid {} x {}", price_ticks, qty);
    std::print("no newline, {:>8} right-aligned\n", qty);
}
```

Formatting an integer costs on the order of tens of nanoseconds, and the write to stdout
costs a system call unless it is buffered. Neither number is enormous, and both are wildly
variable, which is the actual problem.

:::hft
Formatted output never appears on a hot path. Not `printf`, not `std::print`, not a
"cheap" debug line behind a boolean flag. The flag itself is fine; the call it guards
brings a multi-kilobyte formatting routine into the instruction cache and, if the pipe is
full, a blocking `write`. The pattern that works is to push a fixed-size binary record into
a ring buffer and let a separate, unpinned thread format it. Lesson 18 makes this a rule.
:::

## flat_map and flat_set

`std::map` is a red-black tree: every node is a separate allocation, and a lookup is a
chain of dependent loads through pointers scattered across the heap. `std::flat_map` is not
a container at all but a **container adaptor** over two sorted sequences, keys in one
`std::vector` and values in another, with lookup by binary search.

```cpp
#include <flat_map>
#include <cstdint>
#include <print>

int main() {
    // price in ticks -> resting quantity
    std::flat_map<std::int64_t, std::uint32_t> levels{
        {100'25, 400}, {100'50, 900}, {100'75, 150}
    };

    levels[100'50] += 300;                     // O(log n) lookup, no allocation
    if (auto it = levels.find(100'75); it != levels.end())
        std::println("{} @ {}", it->second, it->first);
}
```

Storing keys and values in separate vectors is the struct-of-arrays idea from lesson 20
applied inside the standard library: a binary search touches only keys, so every cache line
it pulls in is entirely keys.

**When it wins.** Small to medium sizes, lookup-heavy access, and iteration in key order.
For a few hundred price levels a `flat_map` lookup is typically 2 to 5 times faster than
`std::map` on a modern x86 server, because the search fits in a handful of cache lines and
the values are prefetchable. Iteration is a linear scan of contiguous memory rather than a
pointer walk.

**When it loses.** Insertion or erasure anywhere but the end is `O(n)` because the vectors
have to be shifted. A book that inserts a new level in the middle a million times a second
will spend all its time in `memmove`. Also, `flat_map` invalidates all iterators on any
modification, and its insert is not exception-safe in the same way a node-based map's is.

:::note
If your key set is dense and bounded, neither map is the answer. An array indexed by
`price_ticks - base_ticks` is one load. Lesson 37 builds an order book on exactly that.
:::

## Deducing this

C++23 lets a member function name its own object as an explicit first parameter, written
`this Self&& self`. The immediate payoff is the elimination of const/non-const duplication.

```cpp
#include <cstdint>
#include <utility>
#include <vector>

class Book {
    std::vector<std::int64_t> bid_prices_;
public:
    // One definition serves const and non-const callers, and preserves value category.
    template <typename Self>
    auto&& best_bid(this Self&& self) {
        return std::forward<Self>(self).bid_prices_.front();
    }
};
```

Calling `best_bid()` on a `Book&` yields `std::int64_t&`; on a `const Book&` it yields
`const std::int64_t&`. Previously that needed two functions, one of which usually cheated
with `const_cast`.

The second payoff is CRTP without the C, the R or the T. The curiously recurring template
pattern existed only so a base class could call into its derived type at compile time.
The explicit object parameter does that directly, using `this auto&&` when the body does
not need to name the type:

```cpp
#include <cstdint>

struct FeedHandler {
    // `self` is the most-derived type; the call to on_trade is resolved statically.
    void dispatch(this auto&& self, std::int64_t px, std::uint32_t qty) {
        if (qty != 0) self.on_trade(px, qty);
    }
};

struct MidCapture : FeedHandler {
    std::int64_t last = 0;
    void on_trade(std::int64_t px, std::uint32_t) { last = px; }
};

int main() {
    MidCapture h;
    h.dispatch(100'25, 300);       // inlines to `h.last = 100'25;`
}
```

No template parameter on the base, no `static_cast<Derived&>(*this)`, and the call still
devirtualises completely at `-O2`.

## Telling the optimiser what you already know

The compiler proves what it can. Everything else it must assume might happen, which means
emitting a branch. `[[assume(expr)]]` is a promise that `expr` is true; the compiler may
propagate that fact and is permitted to do anything at all if you lie.

```cpp
#include <cstdint>
#include <span>
#include <vector>

std::int64_t price_at(const std::vector<std::int64_t>& levels, std::size_t i) {
    [[assume(i < levels.size())]];
    return levels.at(i);      // range check and the throwing path both fold away
}

std::int64_t notional_sum(std::span<const std::int64_t> px,
                          std::span<const std::uint32_t> qty) noexcept {
    [[assume(px.size() == qty.size())]];
    [[assume(px.size() % 8 == 0)]];        // the caller pads the batch
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < px.size(); ++i)
        sum += px[i] * static_cast<std::int64_t>(qty[i]);
    return sum;
}
```

`std::vector::at` normally compares the index against the size and calls a
`__throw_out_of_range` helper. With the assumption in place, GCC 13 and Clang 19 at `-O2`
emit a single load: the comparison is provably false, so the branch and the entire cold
throw path disappear.

The second function shows the other common use. Without the `% 8` assumption the compiler
must emit a scalar epilogue for the leftover 0 to 7 elements; with it, the vector body is
the whole function. Confirm both in Compiler Explorer rather than believing me.

`std::unreachable()` is the same idea for control flow. It marks a path that cannot be
taken, which lets the compiler drop the default case of a switch entirely:

```cpp
#include <utility>
#include <cstdint>

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

std::int64_t signed_qty(Side s, std::uint32_t qty) noexcept {
    switch (s) {
        case Side::Buy:  return  static_cast<std::int64_t>(qty);
        case Side::Sell: return -static_cast<std::int64_t>(qty);
    }
    std::unreachable();                    // no bounds table, no default branch
}
```

:::warn
`[[assume]]` and `std::unreachable()` are load-bearing lies if you get them wrong. An
assumption that is false at run time is undefined behaviour, and the failure mode is not a
crash at the assumption. It is a silent miscompilation somewhere downstream. Use them only
where a validation at the system boundary has already established the fact, and consider
asserting the same condition in a debug build.
:::

Two smaller conveniences in the same family. `std::to_underlying` replaces
`static_cast<std::underlying_type_t<E>>(e)`, and `std::byteswap` gives you a portable
`bswap` instruction for the big-endian fields that every exchange protocol is full of:

```cpp
#include <bit>
#include <utility>
#include <cstdint>

enum class Venue : std::uint8_t { Xnas = 3, Xnys = 7 };

std::uint32_t from_wire_be(std::uint32_t raw) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(raw);         // one `bswap` at -O2
    return raw;
}

std::uint8_t venue_index(Venue v) noexcept { return std::to_underlying(v); }
```

## Reading a network buffer legally

Every feed handler ever written contains something like
`auto* m = reinterpret_cast<const TradeMsg*>(buf);`. That has always been undefined
behaviour, and not for a pedantic reason. The abstract machine says an object exists at an
address only if something started its lifetime there. `recv()` wrote bytes into an array of
`std::byte`; it did not create a `TradeMsg`. Reading `m->price_ticks` reads an object that
does not exist. Compilers mostly tolerate this, and then one day type-based alias analysis
decides your store to the buffer and your load from the struct cannot refer to the same
memory, and reorders them.

C++23 gives the blessed form. An **implicit-lifetime type** is one that is trivially
destructible and has a trivial or deleted default constructor: aggregates of scalars, in
practice. `std::start_lifetime_as` says "the bytes here now hold one of these", costs zero
instructions, and makes the access defined.

```cpp
#include <cstdint>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

struct alignas(8) TradeMsg {       // implicit-lifetime; sizeof 16, no padding
    std::int64_t  price_ticks;     // offset 0
    std::uint32_t qty;             // offset 8
    std::uint32_t seq;             // offset 12
};
static_assert(sizeof(TradeMsg) == 16);
static_assert(std::is_trivially_destructible_v<TradeMsg>);           // implicit-lifetime:
static_assert(std::is_trivially_default_constructible_v<TradeMsg>);  // both must hold

const TradeMsg* as_trade(std::span<const std::byte> buf) noexcept {
    if (buf.size() < sizeof(TradeMsg)) return nullptr;
    return std::start_lifetime_as<TradeMsg>(buf.data());
}
```

C++23 also adds `std::is_implicit_lifetime_v<T>` to `<type_traits>`, which states the
property directly rather than spelling out the two conditions above.

:::note
Library support lags the paper. As of GCC 15 and Clang 20, neither `std::start_lifetime_as`
nor `std::is_implicit_lifetime_v` has shipped. Until they do, the portable form that is
also unambiguously defined is a copy into a local object:

```cpp
#include <cstring>

TradeMsg as_trade_copy(std::span<const std::byte> buf) noexcept {
    TradeMsg m{};
    std::memcpy(&m, buf.data(), sizeof m);   // one 16-byte load and store at -O2
    return m;
}
```

For a message that fits in one or two cache lines this compiles to the same handful of
instructions as the cast would have, so the "zero-copy" argument for
`reinterpret_cast` was mostly imaginary anyway.
:::

Two obligations remain and neither is waived by C++23. The pointer must be correctly
aligned for `TradeMsg`, so either the receive buffer is over-aligned or the message offset
is known to be a multiple of 8. And the wire layout must match the struct layout exactly,
which means auditing padding by hand as in lesson 21. Lesson 36 does this properly for a
real protocol.

:::pitfall
`std::start_lifetime_as` is not a decoder. It does not byteswap, it does not validate, and
it does not make a misaligned pointer safe. It only fixes the lifetime problem. If the wire
format is big-endian or packed differently from your struct, you still have to copy field
by field, and that copy is cheap: a 16-byte message is one cache line either way.
:::

## The rest of the drawer

Three small things you will meet in review.

Multidimensional `operator[]` finally takes more than one argument, so a flat 2D array
stops needing `operator()`.

`static operator()` lets a stateless comparator avoid passing a `this` pointer it never
reads, which occasionally saves a register in a sort inner loop. And the `uz` suffix gives
you a `std::size_t` literal, so index arithmetic stops producing signed/unsigned warnings.

```cpp
#include <algorithm>
#include <cstdint>
#include <vector>

class DepthGrid {                       // symbols x levels, row-major
    std::vector<std::int64_t> cells_;
    std::size_t levels_;
public:
    DepthGrid(std::size_t syms, std::size_t levels)
        : cells_(syms * levels), levels_(levels) {}

    std::int64_t& operator[](std::size_t sym, std::size_t lvl) noexcept {
        return cells_[sym * levels_ + lvl];      // C++23: two subscript arguments
    }
};

struct ByPriceDesc {
    static bool operator()(std::int64_t a, std::int64_t b) noexcept { return a > b; }
};

void rebuild(DepthGrid& g, std::vector<std::int64_t>& bids) {
    std::ranges::sort(bids, ByPriceDesc{});
    for (auto i = 0uz; i < bids.size(); ++i)     // uz: no conversion warning
        g[0, i] = bids[i];
}
```

Finally, `<stacktrace>` gives you `std::stacktrace::current()`. It is slow, it allocates,
and it needs debug info, so it belongs exactly one place: the terminate handler and the
error path that is about to kill the process anyway. On libstdc++ you must link with
`-lstdc++exp`.

:::exercise
Take the `price_at` function above and compile it three ways at `-O2`: as written, with the
`[[assume]]` removed, and with `at` replaced by `operator[]`. Compare the assembly. Then
write a version whose assumption is false for some input and run it. Note that the wrong
answer does not appear at the assumption, which is the whole point of the warning above.
:::

## Takeaways

- `std::flat_map` wins on lookup-heavy, iteration-heavy, small-to-medium data and loses badly on middle insertion. It is a sorted vector pair, so treat it like one.
- Deducing this collapses const/non-const member pairs into one function and retires CRTP.
- `[[assume]]` and `std::unreachable()` feed the optimiser facts it cannot prove, and are undefined behaviour the moment they are wrong. Establish the fact at the boundary first.
- `std::start_lifetime_as` is the standard-blessed replacement for `reinterpret_cast` over a received buffer, for implicit-lifetime types only. Alignment and layout remain your problem.
- `std::print` is a better `printf` and still has no place on a hot path. Log binary, format elsewhere.
