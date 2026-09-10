---
title: CRTP and Static Polymorphism
part: Part III - The Machine
summary: The curiously recurring template pattern, what it buys over virtual, the mixin trick it enables, the three ways it bites, and where C++23 deducing this retires it.
time: 35 min
level: advanced
tags: crtp, mixins, static-polymorphism, deducing-this
---

Lesson 23 listed CRTP as one of four ways to dispatch without a vtable and moved on. It
deserves more than a bullet, because the pattern does something the other three cannot:
it lets a base class call into a derived class it has never heard of, with no indirection,
and lets that base inject behaviour back down. That is the whole toolkit for building
strategy frameworks that cost nothing at run time.

## The shape

The base class is a template, and the template argument is the class deriving from it.

```cpp
template <class Derived>
struct Base {
    void handle() {
        static_cast<Derived*>(this)->handle_impl();
    }
};

struct Concrete : Base<Concrete> {
    void handle_impl() { /* ... */ }
};
```

`Concrete` inherits from a base parameterised on `Concrete` itself. The name recurs, and
in 1995 Jim Coplien called that curious, so we are stuck with the acronym. The mechanism
is the `static_cast` in `handle`: because `Base` knows the exact derived type as a
template argument, the downcast is a compile-time adjustment of a pointer, and the call
that follows is an ordinary direct call the compiler can inline.

:::key
A virtual call asks at run time "what type am I?". CRTP answers that question at compile
time by putting the answer in the type. The vptr, the vtable load and the indirect branch
all disappear, and so does the optimisation barrier that a virtual call puts in front of
the inliner.
:::

## The same interface, three ways

Here is one strategy interface written with `virtual`, with CRTP, and with a concept, so
the differences are side by side.

```cpp dispatch_three_ways.cpp
#include <cstdint>
#include <memory>

using Ticks = std::int64_t;

// 1. Virtual: one type for all strategies, resolved at run time.
struct StrategyV {
    virtual ~StrategyV() = default;
    virtual Ticks quote(Ticks mid) noexcept = 0;
};

struct PassiveV final : StrategyV {
    Ticks offset = 2;
    Ticks quote(Ticks mid) noexcept override { return mid - offset; }
};

Ticks run_v(StrategyV& s, Ticks mid) noexcept { return s.quote(mid); }

// 2. CRTP: the base provides shared machinery, the derived provides the decision.
template <class Derived>
class StrategyC {
public:
    Ticks quote(Ticks mid) noexcept {
        ++calls_;
        return self().quote_impl(mid);
    }
    [[nodiscard]] std::uint64_t calls() const noexcept { return calls_; }

private:
    // Both spellings, so const and non-const members can use them.
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
    const Derived& self() const noexcept { return static_cast<const Derived&>(*this); }
    std::uint64_t calls_ = 0;
};

class PassiveC : public StrategyC<PassiveC> {
public:
    Ticks quote_impl(Ticks mid) noexcept { return mid - offset_; }
private:
    Ticks offset_ = 2;
};

template <class S>
Ticks run_c(StrategyC<S>& s, Ticks mid) noexcept { return s.quote(mid); }

// 3. Concept: no inheritance at all, just a requirement on the type.
template <class S>
concept Strategy = requires(S s, Ticks mid) {
    { s.quote(mid) } noexcept -> std::same_as<Ticks>;
};

Ticks run_k(Strategy auto& s, Ticks mid) noexcept { return s.quote(mid); }
```

`run_v` compiles to a vtable load and an indirect call, and the body of `quote` stays
opaque. `run_c` and `run_k` both compile to the subtraction, inlined, with the object's
offset folded in if it is known.

:::asm
Compile the three at `-O2` and look for `call` instructions. `run_v` keeps one, plus a
load of the vptr. `run_c` and `run_k` keep none. The difference is not the handful of
cycles the call itself costs; it is that everything downstream of an opaque call cannot be
constant-folded, reordered, or vectorised.
:::

## What CRTP does that concepts do not

If dispatch were all you needed, concepts would win: they are simpler, they produce better
errors, and they impose no inheritance. CRTP earns its complexity when the base has
something to **give** the derived class rather than merely something to call.

That is the mixin: a base template that implements behaviour in terms of a small hook the
derived type supplies.

```cpp mixins.cpp
#include <compare>
#include <cstdint>

// Generates the full comparison set from one hook.
template <class Derived>
struct Ordered {
    friend bool operator==(const Derived& a, const Derived& b) noexcept {
        return a.key() == b.key();
    }
    friend std::strong_ordering operator<=>(const Derived& a, const Derived& b) noexcept {
        return a.key() <=> b.key();
    }
};

// Counts live instances without the derived type writing any bookkeeping.
template <class Derived>
class Counted {
public:
    Counted() noexcept { ++live_; }
    Counted(const Counted&) noexcept { ++live_; }
    ~Counted() noexcept { --live_; }
    static std::uint64_t live() noexcept { return live_; }

protected:
    Counted& operator=(const Counted&) = default;

private:
    static inline std::uint64_t live_ = 0;   // one counter per Derived
};

struct PriceLevel : Ordered<PriceLevel>, Counted<PriceLevel> {
    // Counted has a user-provided constructor, so PriceLevel is no longer an
    // aggregate and needs one of its own.
    PriceLevel(std::int64_t price, std::uint32_t quantity) noexcept
        : price_ticks(price), qty(quantity) {}

    std::int64_t price_ticks;
    std::uint32_t qty;
    [[nodiscard]] std::int64_t key() const noexcept { return price_ticks; }
};

bool inside(const PriceLevel& a, const PriceLevel& b) noexcept {
    return a < b;                       // from Ordered, via argument-dependent lookup
}
```

That constructor is the first tax the pattern charges: a mixin with a non-trivial
constructor takes aggregate initialisation away from every class that uses it.

Two other things are worth naming. `Counted<Derived>` gets a **separate** `live_` for every
derived type, because each instantiation is a distinct class. That per-derived static is
the reason the pattern is parameterised at all. And `Ordered` injects hidden friend
operators that are found by argument-dependent lookup, so `PriceLevel` gets a complete
comparison interface without declaring anything.

## The instrumentation mixin

This is the version that earns its place on a trading system. You want per-strategy latency
measurement in staging and nothing at all in production, decided at compile time.

```cpp instrumented.cpp
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <type_traits>

using Ticks = std::int64_t;

struct NoStats {};                       // empty: occupies no storage

class Stats {
public:
    void record(std::uint64_t ns) noexcept {
        worst_ = std::max(worst_, ns);
        ++buckets_[std::min<std::size_t>(ns / 8, buckets_.size() - 1)];
    }
    [[nodiscard]] std::uint64_t worst() const noexcept { return worst_; }

private:
    std::array<std::uint32_t, 1024> buckets_{};
    std::uint64_t worst_ = 0;
};

template <class Derived, bool Measure>
class Instrumented {
public:
    Ticks quote(Ticks mid) noexcept {
        if constexpr (!Measure) {
            // Not merely skipped: the timing code is never instantiated.
            return self().quote_impl(mid);
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            const Ticks out = self().quote_impl(mid);
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            stats_.record(static_cast<std::uint64_t>(ns));
            return out;
        }
    }

    // Only exists when the layer is switched on.
    [[nodiscard]] std::uint64_t worst_ns() const noexcept requires Measure {
        return stats_.worst();
    }

private:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }

    // The storage itself is conditional, and an empty member marked
    // [[no_unique_address]] contributes nothing to the object's size.
    [[no_unique_address]] std::conditional_t<Measure, Stats, NoStats> stats_{};
};

class Passive : public Instrumented<Passive, false> {
public:
    Ticks quote_impl(Ticks mid) noexcept { return mid - offset_; }
private:
    Ticks offset_ = 2;
};

class PassiveMeasured : public Instrumented<PassiveMeasured, true> {
public:
    Ticks quote_impl(Ticks mid) noexcept { return mid - offset_; }
private:
    Ticks offset_ = 2;
};

static_assert(sizeof(Passive) == sizeof(Ticks));        // the layer vanished
static_assert(sizeof(PassiveMeasured) > sizeof(Ticks)); // and reappears on demand
```

Note what `if constexpr` alone does **not** do. It removes the timing code from the
function body, but a plain data member would still occupy space in every build, because
member declarations are not affected by a branch inside a function. Making the feature
truly free takes two more pieces: `std::conditional_t` to pick an empty type for the
storage, and `[[no_unique_address]]` to let that empty member take no room. With all three,
`Passive` is the size of its one field, reads no clock, and inlines to a subtraction. Flip
the flag in a staging build and the same source produces a measured strategy. A virtual
base cannot do this: the storage and the branch would exist in every build.

:::hft
The pattern generalises to every cross-cutting concern on the hot path: risk checks,
sequence validation, dry-run mode, and message counting. Each becomes a mixin with a
compile-time flag. The production binary contains only the layers you turned on, so
"disabled" features cost zero bytes of instruction cache rather than a predictable branch.
Predictable branches are cheap, but zero is cheaper, and the code you did not emit cannot
be mispredicted or evicted.
:::

## Three ways it bites

**The base cannot see into the derived class at class scope.** When `Base<Derived>` is
instantiated, `Derived` is still incomplete, so anything that needs its layout must live
inside a member function body, which is only instantiated on use.

```cpp
template <class Derived>
struct Base {
    // Error: Derived is incomplete here.
    // static_assert(sizeof(Derived) <= 64);

    void check() {
        static_assert(sizeof(Derived) <= 64);   // fine: instantiated later
    }
    using Value = typename Derived::Value;      // also too early
};
```

**There is no common base type.** `StrategyC<PassiveC>` and `StrategyC<AggressiveC>` are
unrelated classes. You cannot put them in one container, hold them behind one pointer, or
choose between them from a configuration file at run time. Every caller must be a template,
and the concrete type propagates outward until something erases it. This is the real cost
of CRTP, and it is a design constraint, not an inconvenience.

:::pitfall
Copy-pasting a derived class and forgetting to change the template argument compiles
cleanly and is undefined behaviour:

```cpp
struct Passive : StrategyBase<Passive> { /* ... */ };
struct Aggressive : StrategyBase<Passive> { /* ... */ };   // wrong, and it builds
```

`static_cast<Passive*>(this)` on an `Aggressive` object is a downcast to an unrelated type.
The fix is to make the base uncontructible except by its true derived class:

```cpp
template <class Derived>
class StrategyBase {
private:
    StrategyBase() = default;          // only a friend can construct
    friend Derived;                    // and the friend is the real Derived
};
```

Now `Aggressive : StrategyBase<Passive>` fails to compile, because `Aggressive` is not a
friend of `StrategyBase<Passive>` and so cannot call its constructor.
:::

**It multiplies code.** Every derived type instantiates its own copy of every base member
function it uses. With four strategies and a five-layer mixin stack, the compiler emits
twenty function bodies where a virtual design emits five. That is usually the right trade
on a hot path, and it is the wrong trade for anything cold, because instruction cache is
also a scarce resource. Lesson 29 covers how to measure when you have crossed the line.

## C++23: deducing this retires most of it

The explicit object parameter lets a member function name its own object, with its real
type, without the base being a template at all.

```cpp deducing_this.cpp
#include <cstdint>

using Ticks = std::int64_t;

struct StrategyBase {
    // Self is deduced as the most derived type at the call site.
    template <class Self>
    Ticks quote(this Self&& self, Ticks mid) noexcept {
        return self.quote_impl(mid);
    }
};

struct Passive : StrategyBase {
    Ticks offset = 2;
    Ticks quote_impl(Ticks mid) const noexcept { return mid - offset; }
};

static_assert(sizeof(Passive) == sizeof(Ticks));   // no vptr, no template base
```

`StrategyBase` is now a plain class. There is no template argument to get wrong, no
incomplete-type restriction, and no separate instantiation of the base per derived type.
For the dispatch half of CRTP, this is a straight replacement and you should prefer it.

CRTP still wins where the base needs the derived type as a **type** rather than as a value:
a per-derived static member like the counter above, a member whose type depends on
`Derived`, a return type of `Derived&`, or a base that must be specialised on the derived
type. Those cases are real but rarer than the dispatch case.

:::perf
Neither form costs anything at run time. Deducing this is a readability and
maintainability improvement, not a speed one. If you are migrating working CRTP code,
migrate it because the next person will understand it, not because you expect a profile
to change.
:::

## Choosing

| You need | Use |
|---|---|
| One decision, no shared machinery | a concept-constrained template |
| Shared machinery calling a derived hook | deducing this |
| Per-derived static state, or `Derived` as a type | CRTP |
| A closed set chosen at run time | `std::variant` plus a visit |
| An open set chosen at run time, off the hot path | `virtual` |

:::exercise
Take the `Instrumented` mixin above and add a second layer, `RiskChecked<Derived, bool>`,
that rejects a quote outside a price band before forwarding to the next layer. Stack them
as `class Passive : public Instrumented<Passive, false>, public RiskChecked<Passive, true>`
and confirm two things at `-O2`: that `sizeof(Passive)` grows only by the risk layer's
state, and that the disabled instrumentation contributes no instructions. Then rewrite the
same stack with deducing this and compare which version you would rather debug.
:::

## Takeaways

- CRTP puts the derived type in the base's template argument, turning a virtual call into a
  direct, inlinable one.
- Its real advantage over concepts is injection: a base that supplies behaviour and
  per-derived static state, not just a base that calls down.
- Compile-time flags on a mixin let a feature vanish from the binary entirely rather than
  become a predictable branch.
- The pattern has no common base type, so the concrete type propagates through every
  caller until something erases it. Accept that or pick a different tool.
- Guard against the wrong template argument with a private constructor and `friend Derived`.
- In C++23, prefer deducing this for the dispatch case, and keep CRTP for the cases that
  genuinely need `Derived` as a type.
