---
title: constexpr, consteval, constinit
part: Part II - Modern C++23
summary: Work the compiler does is work the hot path does not. Tables in .rodata, functions that cannot survive to run time, and a start-up with no dynamic initialisation at all.
time: 30 min
level: intermediate
tags: constexpr, consteval, constinit, compile-time, static-init
---

The cheapest instruction is the one that was never emitted. C++ lets you push real
computation into the compiler, so that what the hot path sees is a load from a read-only
table rather than a divide, a loop, or a branch. The three keywords in the title are the
controls for that: `constexpr` says *may*, `consteval` says *must*, and `constinit` says
*before `main`, with no code*.

## What counts as a constant expression

A **constant expression** is one the compiler can evaluate itself, during translation,
following the abstract machine's rules exactly. The evaluator is a small interpreter
living inside the compiler. It can do arithmetic, call functions, construct objects, run
loops, and touch memory it allocated during the same evaluation. It cannot read a global
that has not been constant-initialised, call a function whose body it cannot see, perform
I/O, or execute anything that would be undefined behaviour.

That last point is worth pausing on. Signed overflow, reading an uninitialised value, or
indexing past the end of an array are all undefined behaviour, and the constant evaluator
is required to reject them. So a `constexpr` computation is a *proof* that the code is
free of those specific bugs on those specific inputs.

:::key
Undefined behaviour in a constant expression is a compile error, not a silent
mis-optimisation. Evaluating a computation at compile time is the strongest static check
C++ has, and you get it for free.
:::

## constexpr variables and functions

`constexpr` on a variable means "initialise this at compile time, and it is `const`".
`constexpr` on a function means "this function *may* be called during constant evaluation,
if its arguments allow it". It is a permission, not an obligation: the same function
compiles to ordinary machine code and runs at run time when the arguments are not
constants.

```cpp fixed_point.hpp
#pragma once
#include <cstdint>

// Prices are integer ticks. dp is decimal places, 0..18.
constexpr std::int64_t pow10(int dp) {
    std::int64_t v = 1;
    for (int i = 0; i < dp; ++i) v *= 10;
    return v;
}

constexpr std::int64_t to_ticks(std::int64_t whole, std::int64_t frac, int dp) {
    return whole * pow10(dp) + frac;
}

constexpr std::int64_t kOpeningPrice = to_ticks(101, 2500, 4);   // computed by the compiler
```

The rules a `constexpr` function must obey are now short: no `goto` before C++23, no
`static` variables of non-literal type, and the body must be capable of constant evaluation
*for at least one set of arguments*. If it is capable for none, the program is ill-formed,
but compilers are permitted not to diagnose it, so do not lean on that.

`kOpeningPrice` is a compile-time integer. It appears in the instruction stream as the
literal `1012500` or in `.rodata`, never as a multiply and an add.

## Tables the hot path only loads

This is where `constexpr` earns its place on a trading system. Any pure function of a small
domain can become an array, built once during compilation, living in the read-only data
segment, costing one L1 hit on the hot path.

```cpp crc32.hpp The table is built by the compiler, not by a start-up routine
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

consteval std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB8'8320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}

inline constexpr auto kCrc32 = make_crc32_table();

constexpr std::uint32_t crc32(const std::byte* p, std::size_t n) {
    std::uint32_t c = 0xFFFF'FFFFu;
    for (std::size_t i = 0; i < n; ++i)
        c = kCrc32[(c ^ std::to_integer<std::uint32_t>(p[i])) & 0xFFu] ^ (c >> 8);
    return ~c;
}
```

The same shape covers the two other tables every desk ends up with: powers of ten for
fixed-point scaling, and the exchange's tick-size ladder.

```cpp ladder.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

inline constexpr std::array<std::int64_t, 19> kPow10 = [] {
    std::array<std::int64_t, 19> a{};
    a[0] = 1;
    for (std::size_t i = 1; i < a.size(); ++i) a[i] = a[i - 1] * 10;
    return a;                       // a[18] == 10^18, still inside int64_t
}();

struct Band { std::int64_t upper_ticks; std::int64_t tick_size; };

// Exchange rule: finer ticks below 1.00, coarser above 50.00.
inline constexpr std::array<Band, 4> kTickLadder{{
    {  10'000,   1 },
    { 500'000,  10 },
    { 5'000'000, 100 },
    { INT64_MAX, 500 },
}};

constexpr std::int64_t tick_size_for(std::int64_t price_ticks) {
    for (const Band& b : kTickLadder)
        if (price_ticks < b.upper_ticks) return b.tick_size;
    return kTickLadder.back().tick_size;
}
```

Note the immediately-invoked lambda initialising `kPow10`. Any `constexpr` variable can be
built by a lambda that runs at compile time, which saves inventing a named function you
call once. The lambda's `operator()` is implicitly `constexpr` when its body qualifies.

:::perf
`tick_size_for` on a run-time price is a four-iteration loop with a compare and a branch:
call it a handful of cycles, and a possible mispredict. If the price band changes rarely,
hoist the lookup out of the inner loop rather than making the table bigger. The win from
`constexpr` here is that the *ladder* costs nothing to build, not that the *lookup* is free.
:::

## consteval: functions that must not survive to run time

`consteval` makes a function **immediate**: every call must be a constant expression, and
the call is replaced by its result. There is no run-time version of the function; it has no
address you can take. Use it when producing the value at run time would be a bug rather
than merely slow.

The canonical case is a mapping validated at compile time. A mistyped FIX tag name or an
unknown symbol should stop the build, not return a sentinel that a strategy then trades on.

```cpp fix_tags.hpp
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

struct TagEntry { std::string_view name; std::uint16_t tag; };

inline constexpr std::array<TagEntry, 5> kFixTags{{
    {"ClOrdID",  11}, {"OrderQty", 38}, {"Price", 44},
    {"Side",     54}, {"Symbol",   55},
}};

consteval std::uint16_t fix_tag(std::string_view name) {
    for (const TagEntry& e : kFixTags)
        if (e.name == name) return e.tag;
    throw "unknown FIX tag name";   // reaching here is not a constant expression
}

static_assert(fix_tag("Price") == 44);
// fix_tag("Pirce")  --> hard compile error, pointing at this line
```

The `throw` never runs. A throw-expression cannot be evaluated during constant evaluation,
so reaching it makes the call ill-formed and the compiler reports the failure at the call
site. If you build with `-fno-exceptions`, replace it with a call to an undeclared-in-
`constexpr` helper, or with `std::unreachable()`, which has the same effect.

:::hft
Symbol-to-instrument-id mapping is the version of this that matters. A desk with 4,000
instruments wants `id_of("ESZ5")` resolved during compilation for anything named in
strategy source, so that a typo is a build failure at 09:00 rather than a rejected order at
09:31. Keep a `consteval` lookup for compile-time-known symbols and a separate, ordinary
run-time lookup for symbols arriving from a config file. Do not let one function serve both.
:::

## constinit and the static initialisation order fiasco

A namespace-scope object is initialised in one of two ways. **Constant initialisation**
happens at compile time; the value is baked into the executable image and costs nothing.
**Dynamic initialisation** runs a constructor before `main`, and the order across
translation units is unspecified.

```cpp Two separate .cpp files, shown together. The order between them is unspecified
#include <cstddef>
#include <string>
#include <vector>

// --- venues.cpp ---
std::vector<std::string> g_venues = {"XNAS", "XNYS", "BATS"};   // dynamic initialisation

// --- risk.cpp ---
extern std::vector<std::string> g_venues;
std::size_t g_venue_count = g_venues.size();   // undefined if this TU initialises first
```

That is the **static initialisation order fiasco**: `g_venue_count` may read a `vector`
whose constructor has not run, and the result is a plausible-looking wrong number rather
than a crash. `constinit` asserts that a variable is constant-initialised, so the compiler
rejects anything that would have been dynamic.

```cpp venues.hpp
#pragma once
#include <array>
#include <string_view>

inline constinit std::array<std::string_view, 3> g_venues{"XNAS", "XNYS", "BATS"};
```

`constinit` is not `const`. The object still lives in writable `.data` and you may modify it
at run time; you have only constrained *how it starts*. That is exactly what a mutable
global needs: a sequence number, a kill-switch flag, a preallocated buffer. Dynamic
initialisation also costs real time at start-up, and on a process with thousands of globals
it is measured in milliseconds, all of it before your first packet.

## Containers, if consteval, and the limits

Since C++20, `std::vector` and `std::string` work during constant evaluation, subject to
one rule: **the allocation must not escape**. Anything allocated inside a constant
evaluation must be freed before that evaluation finishes, so a `constexpr` variable can
never *be* a `vector`. You use one as scratch space and return a fixed-size result.

```cpp
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

consteval std::array<std::int64_t, 5> sorted_strikes() {
    std::vector<std::int64_t> v{10'500, 9'500, 10'000, 11'000, 9'000};
    std::ranges::sort(v);
    std::array<std::int64_t, 5> out{};
    std::ranges::copy(v, out.begin());
    return out;                     // the vector dies here; only the array escapes
}

inline constexpr auto kStrikes = sorted_strikes();
static_assert(kStrikes.front() == 9'000);
```

`if consteval` (C++23) lets one function take a different path in each world. Use it when
the run-time path needs something the evaluator cannot do, such as a hardware instruction.

```cpp
#include <cstddef>
#include <cstdint>
#include "crc32.hpp"

std::uint32_t crc32_hw(const std::byte* p, std::size_t n);   // intrinsics, defined elsewhere

constexpr std::uint32_t checksum(const std::byte* p, std::size_t n) {
    if consteval {
        return crc32(p, n);         // the table-driven version above
    } else {
        return crc32_hw(p, n);
    }
}
```

Note that `if consteval` takes no condition and its branches are ordinary statements. It is
not `if constexpr`, which is a different tool covered in lesson 13.

:::warn
What still cannot happen at compile time: `reinterpret_cast`, `std::memcpy` (use
`std::bit_cast` or `std::copy`), reading a non-`constexpr` global, `new` whose result
escapes, inline assembly, and anything touching a file, socket or clock. A `constexpr`
function containing any of these is legal only if that path is never taken during constant
evaluation.
:::

`static_assert` is the third member of the family and belongs in every header you write. It
turns an assumption into a build failure.

```cpp
#include <cstdint>
#include <type_traits>
#include "ladder.hpp"

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t venue;
};

static_assert(sizeof(Order) == 24, "Order grew; check the ring buffer stride");
static_assert(std::is_trivially_copyable_v<Order>, "Order is memcpy'd into the ring");
static_assert(kPow10[9] == 1'000'000'000);
```

The cost is compile time. The constant evaluator is a tree-walking interpreter, roughly two
to three orders of magnitude slower than the generated code, and it allocates a node per
operation. A 256-entry CRC table is free; a million-entry table generated by a nested loop
will add seconds to every build of every TU that includes it. GCC caps the work with
`-fconstexpr-ops-limit`, Clang with `-fconstexpr-steps`. If you find yourself raising
either, generate the table with a script and check in the resulting header instead.

:::exercise
Take `tick_size_for` and turn it into a direct lookup: build a `constexpr` array of 4,096
entries indexed by `price_ticks >> 12`, so the run-time cost is one shift and one load with
no branch. Compile both versions at `-O2 -S` and compare the assembly of a function that
calls it in a loop over 1,000 prices. Then measure the build-time difference with
`time g++ -std=c++23 -O2 -c`, and decide whether removing the branch was worth it.
:::

## Takeaways

- `constexpr` is permission to run at compile time; `consteval` is a requirement; `constinit`
  constrains only how a mutable global starts.
- Undefined behaviour is a hard error inside a constant expression, which makes compile-time
  evaluation the strongest static check the language offers.
- Turn pure functions of small domains into `constexpr` tables so the hot path does a
  `.rodata` load instead of a computation.
- `constinit` removes dynamic initialisation, killing both the static initialisation order
  fiasco and the pre-`main` cost of constructing globals.
- `vector` and `string` work during constant evaluation, but the allocation must not escape;
  return a `std::array`.
- Compile time is the price. Measure it, and generate very large tables with a script rather
  than the constant evaluator.
