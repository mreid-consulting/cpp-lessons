---
title: Templates and Generic Code
part: Part I - Foundations
summary: How the compiler generates code from templates, why a compile-time capacity turns a modulo into an AND, what the zero-cost abstraction claim actually means, and the compile-time and icache bills that come with it.
time: 35 min
level: intermediate
tags: templates, ctad, nttp, instantiation, zero-cost
---

Templates are not a generics system bolted onto C++. They are a code generator that runs
inside the compiler, and the code it generates is compiled as if you had written it by
hand. That is the source of both the celebrated zero-cost abstraction and of the
twenty-minute build times, the six-thousand-character error messages, and the instruction
cache pressure that shows up in a profile long after the abstraction looked free.

## Function and class templates

### Deduction

A function template is a pattern. The compiler stamps out one real function per distinct
set of template arguments it sees used.

```cpp
#include <cstdint>

template <typename T>
constexpr T clamp_to(T value, T lo, T hi) noexcept {
    return value < lo ? lo : (value > hi ? hi : value);
}

std::int64_t bounded_price(std::int64_t px) {
    return clamp_to(px, std::int64_t{1}, std::int64_t{1'000'000});   // T deduced as int64_t
}
```

**Template argument deduction** infers `T` from the argument types. It matches types
structurally and does not perform conversions, so `clamp_to(px, 1, 1'000'000)` fails to
compile: `T` would have to be both `std::int64_t` and `int` at once. Either make the
literals match, as above, or write `clamp_to<std::int64_t>(px, 1, 1'000'000)` to supply
`T` explicitly and let the arguments convert.

For a parameter declared `T&&` in a *deduced* context, deduction has a special rule that
makes it bind to anything — the so-called forwarding reference. That, and
`std::forward`, are the subject of lesson 14; here we stay with by-value and `const&`.

### Class templates and CTAD

```cpp price_level.hpp
#pragma once
#include <cstdint>
#include <vector>

template <typename Qty>
class PriceLevel {
public:
    explicit PriceLevel(std::int64_t price_ticks) noexcept : price_(price_ticks) {}
    PriceLevel(std::int64_t price_ticks, Qty initial) noexcept
        : price_(price_ticks), total_(initial) {}

    void add(Qty q) noexcept { total_ += q; }
    Qty  total() const noexcept { return total_; }
    std::int64_t price() const noexcept { return price_; }

private:
    std::int64_t price_;
    Qty          total_{};
};

// A deduction guide states the mapping explicitly when the compiler cannot, or gets it wrong.
template <typename Qty>
PriceLevel(std::int64_t, Qty) -> PriceLevel<Qty>;
```

Before C++17 you had to spell the arguments at every construction. **Class template
argument deduction** (CTAD) lets the constructor deduce them:

```cpp
#include <cstdint>
#include <vector>
#include "price_level.hpp"

void ctad() {
    std::vector prices{100'50, 100'51, 100'52};   // std::vector<int>, deduced from the list
    PriceLevel empty_book{100'50, std::uint32_t{0}};  // PriceLevel<std::uint32_t>, deduced
    PriceLevel<std::uint32_t> explicit_form{100'50}; // one-arg ctor mentions no Qty: spell it
}
```

Deduction works from constructor parameters only. The two-argument constructor mentions
`Qty`, so CTAD can see it; the one-argument constructor does not, so you must write the
argument yourself.

## Non-type template parameters

A template parameter can be a *value* rather than a type. This is where templates stop
being about code reuse and start being about performance.

```cpp ring_buffer.hpp
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");

public:
    bool push(const T& value) noexcept {
        if (size() == Capacity) return false;
        slots_[write_ & kMask] = value;          // & not %, because Capacity is known now
        ++write_;
        return true;
    }

    bool pop(T& out) noexcept {
        if (write_ == read_) return false;
        out = slots_[read_ & kMask];
        ++read_;
        return true;
    }

    std::size_t size() const noexcept { return write_ - read_; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> slots_{};
    std::size_t write_ = 0;
    std::size_t read_  = 0;
};
```

Because `Capacity` is a compile-time constant and a power of two, `write_ & kMask` is a
single `and` instruction. The alternative, a runtime capacity stored in a member, forces
`write_ % capacity_` — an integer division, which on x86-64 is a `div` with a latency
somewhere in the range of 20 to 40 cycles depending on operand size and
microarchitecture, versus 1 cycle for `and`. The capacity being a template argument is
worth roughly an order of magnitude on that one operation, and it also lets the storage be
a `std::array` member rather than a heap allocation.

:::hft Why the capacity is in the type
Every wait-free SPSC queue you will meet in production has its capacity as a non-type
template parameter, for three compounding reasons. The modulo becomes a mask. The storage
becomes inline, so the queue and its slots share cache lines you control rather than
whatever the allocator handed out. And the compiler knows the bound, so it can unroll,
vectorise and drop the runtime capacity load from the loop entirely.

The cost you accept is that `RingBuffer<Order, 4096>` and `RingBuffer<Order, 8192>` are
unrelated types that cannot be assigned to one another or passed to the same non-template
function. On a trading system that is usually fine: capacities are chosen once at design
time and never configured. Lesson 33 builds this queue properly, with the atomics.
:::

## The instantiation model

The compiler cannot generate code for `RingBuffer<Order, 4096>` unless it can see the full
definition of the template at the point of use. That is why template definitions live in
headers, and it is the single largest structural consequence of using them.

A template is instantiated **implicitly** the first time it is used with a given set of
arguments, and only the member functions actually called are instantiated. If two
translation units instantiate the same specialisation, the linker sees duplicate symbols
marked as `inline`-like (COMDAT) and discards all but one.

You can also force instantiation, which is how you move the code generation out of every
TU and into one:

```cpp order_book.hpp, then order_book.cpp
// ---- order_book.hpp ----
#pragma once
#include <cstdint>

template <typename Qty>
class OrderBook {
public:
    void apply(std::int64_t price_ticks, Qty q);
};

extern template class OrderBook<std::uint32_t>;   // do NOT instantiate in every includer

// ---- order_book.cpp ----
template <typename Qty>
void OrderBook<Qty>::apply(std::int64_t, Qty) { /* ... */ }

template class OrderBook<std::uint32_t>;          // instantiate exactly once, here
```

`extern template` in the header suppresses implicit instantiation in every including TU;
the explicit instantiation in the `.cpp` produces the one copy. Compile time drops and the
binary shrinks, at the cost of losing cross-TU inlining unless you use `-flto`.

## Specialisation, packs and folds

**Explicit specialisation** replaces the whole template for one specific set of arguments.

Leaving the primary undefined, as `byte_swap` does below, means an unsupported type fails
at link time with a missing symbol: a crude but effective constraint. Lesson 13 shows the
modern version with concepts, which fails at compile time with a readable message instead.

**Partial specialisation** applies to class templates only, and matches a *shape* rather
than exact arguments, as `WireTraits` does for any pointer and any byte array. Function
templates cannot be partially specialised; use overloading, or the `if constexpr` and
concepts of lesson 13.

A **parameter pack** holds zero or more template arguments, and a **fold expression**
applies a binary operator across the pack without recursion.

```cpp
#include <cstddef>
#include <cstdint>
#include <print>

template <typename T>
T byte_swap(T v) noexcept;                        // primary: declared, deliberately not defined

template <>
inline std::uint16_t byte_swap(std::uint16_t v) noexcept { return __builtin_bswap16(v); }

template <>
inline std::uint32_t byte_swap(std::uint32_t v) noexcept { return __builtin_bswap32(v); }

template <>
inline std::uint64_t byte_swap(std::uint64_t v) noexcept { return __builtin_bswap64(v); }

template <typename T>
struct WireTraits { static constexpr bool fixed_size = false; };

template <typename T>
struct WireTraits<T*> { static constexpr bool fixed_size = true; };   // any pointer

template <std::size_t N>
struct WireTraits<std::uint8_t[N]> { static constexpr bool fixed_size = true; };

template <typename... Fields>
constexpr std::size_t wire_size(const Fields&... f) noexcept {
    return (sizeof(f) + ... + 0);                 // right fold over +
}

template <typename... Checks>
constexpr bool all_pass(Checks... c) noexcept {
    return (... && c);                            // left fold over &&, short-circuits
}

int main() {
    std::print("{} {} {:#x}\n",
               wire_size(std::uint64_t{}, std::int64_t{}, std::uint32_t{}),   // 20
               all_pass(true, true, false),                                   // false
               byte_swap(std::uint32_t{1}));                                  // 0x1000000
}
```

`sizeof...(Fields)` gives the pack size. Folds over `&&` and `||` short-circuit exactly as
the hand-written chain would, which matters when the operands are function calls doing risk
checks. In real code you would now write `std::byteswap` from `<bit>`, added in C++23; the
point here is the specialisation mechanism, which is how the standard library is built.

## auto and decltype

`auto` deduction uses the same rules as template argument deduction from a by-value
parameter: it strips references and top-level `const`.

```cpp
#include <cstdint>
#include <vector>

void deduction(std::vector<std::int64_t>& prices) {
    auto  a = prices[0];        // std::int64_t: a copy
    auto& b = prices[0];        // std::int64_t&: an alias into the vector
    const auto& c = prices[0];  // const std::int64_t&
    auto&& d = prices[0];       // std::int64_t&: forwarding reference collapses to lvalue ref

    decltype(prices[0]) e = prices[0];       // std::int64_t&: decltype preserves the category
    decltype(auto) f = prices[0];            // std::int64_t&: the deduction that does not strip
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
```

The rule worth memorising: `auto` gives you a value, `decltype(expr)` gives you the
expression's exact type including reference-ness, and `decltype(auto)` gives you `auto`
deduction using `decltype` rules. Use the last one when forwarding a return value whose
category you must preserve.

## The zero-cost claim, and its real bill

Here is the demonstration people quote. Sort a vector of orders by price, once through a
function pointer and once through a templated comparator.

```cpp comparator_cost.cpp
#include <algorithm>
#include <cstdint>
#include <vector>

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

bool by_price_fn(const Order& a, const Order& b) noexcept {
    return a.price_ticks < b.price_ticks;
}

struct ByPrice {                                   // stateless functor: a distinct type
    bool operator()(const Order& a, const Order& b) const noexcept {
        return a.price_ticks < b.price_ticks;
    }
};

void sort_via_pointer(std::vector<Order>& v) {
    std::sort(v.begin(), v.end(), &by_price_fn);   // Compare = bool(*)(const Order&, const Order&)
}

void sort_via_functor(std::vector<Order>& v) {
    std::sort(v.begin(), v.end(), ByPrice{});      // Compare = ByPrice
}
```

`std::sort`'s comparator is a template parameter. In the functor case, `Compare` is
`ByPrice`, a distinct empty type, so the comparison inlines into the sort's inner loop as a
single `cmp`. In the pointer case, `Compare` is a *function pointer type* — the same type
for every function with that signature — so the value is only known at run time and the
inner loop contains an indirect `call`. That is a call, a return, no inlining across it,
and a branch the predictor must learn.

The measured gap on a million-element sort is typically **1.5x to 2x** in favour of the
functor; verify it on your own machine, because it depends on how well the indirect branch
predicts. A capture-less lambda is a stateless functor with nicer syntax and gives the same
result as `ByPrice`.

:::perf The bills that do arrive
"Zero cost" means *no runtime cost relative to the equivalent hand-written code*. It does
not mean free. The real costs:

- **Compile time.** Every TU that includes the header re-parses and re-instantiates the
  template. A heavily templated header included in 200 TUs is 200 instantiations that the
  linker then throws away. This is the dominant term in most large C++ build times.
- **Template bloat.** `RingBuffer<Order, 1024>`, `RingBuffer<Order, 4096>` and
  `RingBuffer<Tick, 4096>` are three complete copies of every member function.
- **Instruction cache pressure.** The L1 instruction cache is typically 32 KiB. Bloat that
  is harmless in a batch program is fatal on a hot path, where you want the whole
  tick-to-trade path resident. This is the cost that surfaces as "we optimised each
  function and the system got slower", and lesson 29 covers measuring and fixing it.

The controls are: `extern template` plus explicit instantiation to collapse duplicates;
keeping the templated part small and delegating the bulk to a non-template function taking
`std::span` or a type-erased callback; and moving cold branches out of the template
entirely so only the hot arithmetic is duplicated.
:::

```cpp The thin-template idiom
#include <cstddef>
#include <cstdint>
#include <span>

// One instantiation of the real work, shared by every element type of this size.
std::size_t count_above_impl(std::span<const std::int64_t> prices, std::int64_t threshold) noexcept;

template <typename Container>
std::size_t count_above(const Container& c, std::int64_t threshold) noexcept {
    return count_above_impl(std::span<const std::int64_t>{c}, threshold);   // thin wrapper
}
```

The template exists only to convert the caller's container into a `std::span`. All the
generated code lives in one non-template function. This is how you get generic interfaces
without generic binaries.

:::exercise
Compile the comparator example with `-std=c++23 -O2 -S -masm=intel` and locate the inner
loop of each `std::sort` instantiation. Confirm that `sort_via_functor` contains a `cmp`
between two loaded `price_ticks` values and `sort_via_pointer` contains a `call` through a
register. Then time both on a shuffled vector of one million orders and report the ratio.
Finally, add a third variant taking `std::function<bool(const Order&, const Order&)>` and
see where it lands; the answer explains why lesson 14 spends time on the cost of a callable.
:::

## Takeaways

- A template is a code generator. One specialisation is generated per distinct set of
  arguments, and it compiles exactly as if hand-written.
- Deduction matches types structurally and does not convert. Supply arguments explicitly
  when the types differ.
- Non-type template parameters are the HFT lever: a compile-time power-of-two capacity
  turns a 20-40 cycle `div` into a 1 cycle `and` and puts the storage inline.
- Definitions must be visible at the point of instantiation, which is why templates live in
  headers. `extern template` plus explicit instantiation buys that back.
- Zero cost means no runtime overhead versus hand-written code. Compile time, binary size
  and instruction cache pressure are all real and all get worse with instantiation count.
- Keep the templated shell thin and put the work in a non-template function over
  `std::span`. Constrain the shell with concepts, covered in lesson 13.
