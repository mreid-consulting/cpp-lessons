---
title: Concepts and if constexpr
part: Part II - Modern C++23
summary: Say what a template requires, get an error message a human can read, and delete branches that only ever had one answer.
time: 30 min
level: intermediate
tags: concepts, requires, if-constexpr, templates, devirtualization
---

Templates in lesson 09 were duck typing with no documentation: a template accepted anything
until it did not, and the failure surfaced two hundred lines deep in a header you did not
write. Concepts move that check to the interface, where it belongs. `if constexpr` does the
same job for the body, letting one function have several shapes and compile only the one it
needs. Together they are how modern C++ writes generic code without paying for it.

## The problem concepts solve

Before C++20 the only way to constrain a template was to make the *substitution* fail, and
rely on the rule that a substitution failure while forming a candidate is not an error, it
just removes the candidate. That is SFINAE, and it reads like this:

```cpp The same constraint, before and after
#include <concepts>
#include <cstdint>
#include <type_traits>

template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
std::int64_t to_ticks_sfinae(T raw) { return static_cast<std::int64_t>(raw); }

std::int64_t to_ticks(std::integral auto raw) { return static_cast<std::int64_t>(raw); }
```

In the first form the intent, "T must be an integer", is buried in a defaulted template
parameter that exists only to be removed. Pass a `double` and the compiler says the call has
no matching function, without saying why. Nest three of these and the error runs to hundreds
of lines.

The second form behaves identically, and the failure now reads: *constraints not satisfied, because
`std::integral<double>` is false*. The constraint is part of the declaration, so it appears
in documentation, in IDE completion, and in the error.

## Writing a concept

A **concept** is a named compile-time predicate on types (and on values, if you want). You
define it with `concept`, and its body is any constant expression of type `bool`.

A one-line concept is often enough, and the interesting form is the **requires expression**,
which asks "does this code compile?" without ever running it. Everything inside is checked
for validity only.

```cpp quote_source.hpp
#pragma once
#include <concepts>
#include <cstdint>

template <class T>
concept TickPrice = std::signed_integral<T> && sizeof(T) == 8;

template <class T>
concept QuoteSource = requires(const T& q) {
    { q.bid_ticks() } -> std::same_as<std::int64_t>;
    { q.ask_ticks() } -> std::same_as<std::int64_t>;
    { q.symbol_id() } -> std::convertible_to<std::uint32_t>;
    typename T::clock_type;                 // must have this nested type
    requires T::kMaxDepth > 0;              // nested requirement: a value constraint
};
```

Four kinds of requirement appear there. A **simple requirement** (`q.bid_ticks();`) says the
expression must compile. A **compound requirement** (`{ expr } -> Concept`) adds a
constraint on the result type. A **type requirement** (`typename T::clock_type;`) says the
name must exist. A **nested requirement** (`requires expr;`) says a boolean constant
expression must be true.

:::pitfall
`{ q.bid_ticks() } -> std::same_as<std::int64_t>` checks the type *after* reference
stripping and decay-free evaluation, which means a function returning `const std::int64_t&`
fails `same_as` and passes `convertible_to`. Use `std::convertible_to` unless you genuinely
need the exact type; exact-type constraints break silently when someone adds a reference.
:::

### requires requires

You will see this and it is not a typo. The first `requires` introduces a requires *clause*
on a template; the second introduces a requires *expression*. Write it when a constraint is
needed once and does not deserve a name.

```cpp
template <class T>
    requires requires(T& t) { t.reset(); }
void recycle(T& t) noexcept { t.reset(); }
```

Read it as "requires that the following compiles". If the same anonymous constraint appears
twice, stop and name it: unnamed constraints do not subsume each other, so overloads built
on them go ambiguous rather than ordering themselves.

### The standard concepts worth knowing

These are the ones you will actually reach for:

| Concept | Header | Means |
|---|---|---|
| `std::integral`, `std::signed_integral` | `<concepts>` | integer types |
| `std::same_as<U>`, `std::convertible_to<U>` | `<concepts>` | type relations |
| `std::derived_from<B>` | `<concepts>` | public unambiguous base |
| `std::invocable<Args...>` | `<concepts>` | callable with those arguments |
| `std::equality_comparable`, `std::totally_ordered` | `<concepts>` | `==`, `<` behave |
| `std::movable`, `std::copyable`, `std::regular` | `<concepts>` | value semantics |
| `std::ranges::contiguous_range` | `<ranges>` | elements are adjacent in memory |

## Constraining a template, and who wins

There are four places to put a constraint, all equivalent in effect:

```cpp
#include <concepts>
#include <cstdint>

template <class T>
concept TickPrice = std::signed_integral<T> && sizeof(T) == 8;

template <TickPrice T> void a(T px);                      // constrained parameter
template <class T> requires TickPrice<T> void b(T px);    // requires clause
template <class T> void c(T px) requires TickPrice<T>;    // trailing requires clause
void d(TickPrice auto px);                                // abbreviated template
```

The last is an **abbreviated function template**: a `auto` parameter with a concept in front
of it. `void d(TickPrice auto px)` declares a template with an invented parameter. It is the
shortest form and reads best for single-parameter constraints.

Constraints participate in overload resolution, and the rule is that the **more constrained**
overload wins when both are viable. "More constrained" is decided by *subsumption*: a
constraint A subsumes B if A's normalised form logically implies B's. This is why building
concepts out of other concepts matters.

```cpp
#include <concepts>
#include <cstdio>

template <std::integral T>        void quote(T px) { std::puts("integral"); }
template <std::signed_integral T> void quote(T px) { std::puts("signed");   }

int main() {
    quote(42);          // "signed":   signed_integral subsumes integral
    quote(42u);         // "integral": signed_integral is not satisfied
}
```

`std::signed_integral` is defined as `integral<T> && is_signed_v<T>`, so it literally
contains `integral`'s atomic constraint and subsumes it. Had the second overload been
written `requires std::is_signed_v<T> && std::is_integral_v<T>`, subsumption would fail and
the call would be ambiguous, because subsumption compares atomic constraints for identity,
not for meaning. Build constraints from concepts, not from raw type traits.

## if constexpr: the branch that is never instantiated

`if constexpr` takes a condition that is a constant expression and discards the branch not
taken. The discarded branch is still parsed, so it must be syntactically valid, but inside a
template it is **not instantiated**, so it may contain code that would not compile for this
`T`. That is the entire difference from a runtime `if`, and it is the whole point.

```cpp decode.hpp
#pragma once
#include <cstdint>

template <class> inline constexpr bool always_false = false;

struct ItchTrade  { std::int64_t price_ticks; std::uint32_t qty; };
struct FixTrade   { std::int64_t mantissa; std::int32_t exponent; std::uint32_t qty; };

template <class Msg>
constexpr std::int64_t price_of(const Msg& m) {
    if constexpr (requires { m.price_ticks; }) {
        return m.price_ticks;
    } else if constexpr (requires { m.mantissa; m.exponent; }) {
        std::int64_t v = m.mantissa;
        for (std::int32_t i = 0; i < m.exponent; ++i) v *= 10;
        return v;
    } else {
        static_assert(always_false<Msg>, "message type carries no price");
    }
}
```

With a plain `if`, all three branches would be instantiated for every `Msg`, and
`m.price_ticks` would be a compile error for `FixTrade`. Note `always_false<Msg>` rather than
a bare `false`: a `static_assert` whose condition does not depend on a template parameter
fires even in a discarded branch, because the compiler is allowed to evaluate it without
instantiating anything.

This replaces **tag dispatch**, the pre-C++17 way of selecting a body. Tag dispatch needed a
tag type per case, an overload per tag, and a trait to pick the tag:

```cpp
#include <cstdint>
#include "decode.hpp"

struct fixed_tag {}; struct scaled_tag {};
std::int64_t price_impl(const ItchTrade& m, fixed_tag);
std::int64_t price_impl(const FixTrade& m, scaled_tag);
// plus a trait mapping Msg -> tag, plus a forwarding function
```

Four moving parts and a name in the overload set that clients can accidentally call. One
`if constexpr` chain replaces all of it, keeps the logic in one readable function, and
generates identical code.

## Erasing a runtime branch with a compile-time policy

Here is where `if constexpr` stops being tidiness and becomes latency work. A dry-run mode
is a boolean on every order-sending path. As a data member it costs a load, a test, and a
branch on every send, and it occupies a branch-target-buffer entry and instruction-cache
bytes even when perfectly predicted. As a template parameter it costs nothing, because the
code is not emitted.

```cpp gate.hpp
#pragma once
#include <cstdint>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

enum class Mode { Live, DryRun };

template <Mode M>
class OrderGate {
public:
    explicit OrderGate(int fd) noexcept : fd_{fd} {}

    void send(const Order& o) noexcept {
        if constexpr (M == Mode::DryRun) {
            ++simulated_;                       // no syscall, no wire format
        } else {
            write_wire(fd_, o);
        }
    }

    std::uint64_t simulated() const noexcept requires (M == Mode::DryRun) {
        return simulated_;
    }

private:
    static void write_wire(int fd, const Order& o) noexcept;
    int fd_;
    std::uint64_t simulated_ = 0;
};
```

`OrderGate<Mode::Live>::send` compiles to the wire write and nothing else: no compare, no
jump, no reference to `simulated_`. The trailing `requires (M == Mode::DryRun)` on
`simulated()` is a constraint on a member function of a class template, which means the
member simply does not exist for the live instantiation. Calling it is a clean error rather
than a silently meaningless zero.

The cost is one instantiation of the class per mode, so binary size roughly doubles for that
class. That is the trade: instruction bytes for branches removed. Make the choice per class,
not as a project-wide policy.

## A Strategy concept, and the virtual alternative

Put both tools together and you get the standard shape of a fast feed handler.

```cpp feed.hpp
#pragma once
#include <concepts>
#include <cstdint>
#include <string_view>
#include <utility>

struct Tick { std::uint32_t symbol_id; std::int64_t price_ticks; std::uint32_t qty; };

template <class S>
concept Strategy = requires(S& s, const Tick& t) {
    { s.on_tick(t) } -> std::same_as<void>;
    { s.name() }    -> std::convertible_to<std::string_view>;
};

template <Strategy S>
class FeedHandler {
public:
    explicit FeedHandler(S s) noexcept : strat_{std::move(s)} {}

    void on_tick(const Tick& t) noexcept {
        ++seen_;
        strat_.on_tick(t);          // a direct call the compiler can inline away
    }

    std::uint64_t seen() const noexcept { return seen_; }

private:
    S strat_;
    std::uint64_t seen_ = 0;
};

struct Momentum {
    void on_tick(const Tick& t) noexcept { last_ = t.price_ticks; }
    std::string_view name() const noexcept { return "momentum"; }
    std::int64_t last_ = 0;
};

static_assert(Strategy<Momentum>);
```

`FeedHandler<Momentum>::on_tick` inlines `Momentum::on_tick` entirely, and at `-O2` the two
functions collapse into a store. The abstract-base-class version instead stores an
`IStrategy*`, loads a vtable pointer, loads a slot, and does an indirect call. That call
cannot be inlined, so the compiler cannot propagate constants across it or keep values in
registers over it, and if the indirect target is not in the branch-target buffer you pay a
full mispredict, on the order of 15 to 20 cycles on a current x86 core. Measure it with the
method in lesson 27 before quoting a number for your machine; the loss of inlining is
usually larger than the call itself.

The `static_assert(Strategy<Momentum>)` line is the other half of the deal. With an abstract
base, forgetting to override a method is caught by the compiler. With a concept, put the
assertion next to the type and you get the same guarantee, checked at the definition rather
than at the first use.

:::hft
The rule of thumb on a desk: if the set of strategies is known at link time and each feed
handler runs one of them, template on the strategy. If you genuinely need a heterogeneous
container of strategies chosen from config at start-up, you need type erasure, and the
honest answer is a `switch` over a small enum in the dispatch loop rather than a virtual
call, because the switch's indirect jump is at least confined to a jump table the predictor
can learn. Lesson 23 measures all three.
:::

:::exercise
Write a `PriceBook` concept requiring `best_bid()`, `best_ask()` returning `std::int64_t`,
and a nested `static constexpr std::size_t kDepth`. Implement two conforming books, one
array-backed with `kDepth == 10` and one with `kDepth == 1`. Then write a
`template <PriceBook B> std::int64_t spread(const B&)` that uses `if constexpr (B::kDepth == 1)`
to skip a bounds check the deep book needs. Compile at `-O2 -S` and confirm the shallow
instantiation has no compare instruction.
:::

## Takeaways

- A concept is a named compile-time predicate; a `requires` expression asks whether code
  compiles, without running it.
- Build concepts from other concepts. Subsumption compares atomic constraints for identity,
  so raw type traits break the "more constrained wins" rule.
- The discarded branch of `if constexpr` is not instantiated inside a template. That is what
  separates it from a runtime `if`, and it retires tag dispatch entirely.
- Turning a runtime policy flag into a template parameter removes the branch from the
  generated code at the cost of one instantiation per policy.
- Templating a feed handler on a `Strategy` concept gives a direct, inlinable call; an
  abstract base gives an indirect call that blocks inlining across it.
- Use `static_assert(Concept<Type>)` next to each implementation so conformance is checked at
  the definition, not at the first call site.
