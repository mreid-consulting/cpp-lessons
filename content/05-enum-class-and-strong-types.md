---
title: enum class and Strong Types
part: Part I - Foundations
summary: Making the compiler reject send(qty, price) at zero runtime cost, using scoped enumerations for closed sets and tagged wrappers for the integers that keep getting swapped.
time: 25 min
level: beginner
tags: enum-class, strong-types, type-safety, zero-cost
---

Every hot path in a trading system moves the same three or four 64-bit integers around: a
price, a quantity, an order id, a timestamp. They are all `std::int64_t` or
`std::uint64_t`, which means the compiler will happily accept them in any order you write
them. A transposed pair of arguments is not a subtle bug at 2am; it is a market order for
ten thousand lots at a price of one hundred. This lesson makes that a compile error and
costs you nothing at run time.

## Plain enums and their two problems

An unscoped enumeration introduces named integer constants:

```cpp
enum Side { Buy, Sell };
enum OrderState { New, Acked, Filled };
```

Two things are wrong here, and both are fatal in a real codebase.

**The names leak into the enclosing scope.** `Buy` and `New` are now names at namespace
scope. `New` in particular collides with something eventually, and `enum TimeInForce { Day,
Ioc, Fok };` in another header will collide with an `enum SessionState { Day, Night };` the
first time both are included in the same translation unit.

**The values implicitly convert to `int`.** That means every one of these compiles:

```cpp
enum Side { Buy, Sell };
enum OrderState { New, Acked, Filled };

void log_it() {
    int x = Buy + Filled;          // 0 + 2. Meaningless, but legal.
    if (Buy == New) { /* true: both are 0 */ }
}
```

Comparing a `Side` to an `OrderState` succeeds because both decay to `int`. The type system
knew these were different sets and threw the information away.

## enum class, and the underlying type

A **scoped enumeration** fixes both problems in one keyword:

```cpp
#include <cstdint>

enum class Side       : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderState : std::uint8_t { New, Acked, PartiallyFilled, Filled, Cancelled };

void trade() {
    Side s = Side::Buy;             // must be qualified
    // int x = s;                   // error: no implicit conversion
    // if (s == OrderState::New) {} // error: different types
}
```

The names live inside the enumeration, and there is no implicit conversion to an integer in
either direction. When you genuinely need the number, C++23 gives you a named cast that
does not require you to repeat the underlying type:

```cpp
#include <cstdint>
#include <utility>            // std::to_underlying (C++23)

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

std::uint8_t on_wire(Side s) noexcept { return std::to_underlying(s); }
```

The `: std::uint8_t` is the **underlying type**, and specifying it is not cosmetic. Without
it the underlying type is implementation-defined but at least as large as `int`, so each
enumeration member costs 4 bytes. Pin it to `std::uint8_t` and a struct carrying `Side`,
`OrderState` and `TimeInForce` spends 3 bytes rather than 12. On a 24-byte wire message
that is the difference between fitting in the padding you already have and growing the
message. It also means the in-memory representation matches the byte the exchange sends, so
your decoder is a `static_cast` rather than a lookup.

A fixed underlying type has one further effect: it makes every value of that type
representable, so `static_cast<Side>(7)` is well defined even though no enumerator names 7.
Without a fixed underlying type, casting a value outside the enumerators' range is undefined
behaviour. Validate what comes off the wire either way.

## Switching, exhaustively

A `switch` over a scoped enumeration with **no `default` label** makes the compiler tell you
when you miss a case. GCC and Clang both implement this as `-Wswitch`, which is on under
`-Wall`.

```cpp
#include <cstdint>

enum class OrderState : std::uint8_t { New, Acked, PartiallyFilled, Filled, Cancelled };

bool is_terminal(OrderState s) noexcept {
    switch (s) {
        case OrderState::New:
        case OrderState::Acked:
        case OrderState::PartiallyFilled: return false;
        case OrderState::Filled:
        case OrderState::Cancelled:       return true;
    }
    return false;    // unreachable for valid values; silences -Wreturn-type
}
```

Add `Rejected` to the enumeration and this function fails to build until you handle it. Add
a `default: return false;` and it silently keeps compiling with the new state treated as
non-terminal, which is exactly the bug you wanted the compiler to find. The rule is: omit
`default` on every switch over a closed set, and accept the extra `return` at the bottom for
the case where a corrupt value arrives from outside.

C++20's `using enum` removes the repetition inside a scope where one enumeration dominates:

```cpp
#include <cstdint>

enum class OrderState : std::uint8_t { New, Acked, PartiallyFilled, Filled, Cancelled };

const char* name(OrderState s) noexcept {
    using enum OrderState;               // C++20
    switch (s) {
        case New:             return "new";
        case Acked:           return "acked";
        case PartiallyFilled: return "partial";
        case Filled:          return "filled";
        case Cancelled:       return "cancelled";
    }
    return "?";
}
```

Use it inside a function body. At namespace scope it reintroduces exactly the leakage
`enum class` was invented to prevent.

## Bitmask flags done properly

Flags are a set, not a closed choice, so you want `|` and `&` back. Define them, rather than
dropping to a plain `enum`:

```cpp order_flags.hpp
#include <cstdint>
#include <utility>

enum class OrderFlags : std::uint8_t {
    None       = 0,
    PostOnly   = 1u << 0,
    Ioc        = 1u << 1,
    Hidden     = 1u << 2,
    ReduceOnly = 1u << 3,
};

constexpr OrderFlags operator|(OrderFlags a, OrderFlags b) noexcept {
    return static_cast<OrderFlags>(std::to_underlying(a) | std::to_underlying(b));
}
constexpr OrderFlags operator&(OrderFlags a, OrderFlags b) noexcept {
    return static_cast<OrderFlags>(std::to_underlying(a) & std::to_underlying(b));
}
constexpr OrderFlags& operator|=(OrderFlags& a, OrderFlags b) noexcept {
    return a = a | b;
}
constexpr bool has(OrderFlags set, OrderFlags f) noexcept { return (set & f) == f; }
```

```cpp
#include "order_flags.hpp"

static_assert(has(OrderFlags::PostOnly | OrderFlags::Hidden, OrderFlags::Hidden));
static_assert(!has(OrderFlags::PostOnly, OrderFlags::Ioc));
```

Equality comes for free on a scoped enumeration, so `has` needs no extra operator. All four
functions are `constexpr` and trivial, so at `-O2` they vanish entirely: the `static_assert`s
above are evaluated at compile time and a runtime `has(flags, OrderFlags::Ioc)` is one `test`
and one `setne`.

## The strong-type problem

Now the harder case. `Side` is a closed set, so an enumeration fits. Price and quantity are
open sets of integers that happen to have the same underlying type, and no enumeration helps.

```cpp
#include <cstdint>

// The interface everybody writes first.
bool send(std::int64_t price_ticks, std::uint32_t qty);

void strategy() {
    std::int64_t  px  = 10'025;
    std::uint32_t qty = 100;
    send(qty, px);                  // compiles. qty widens, px narrows.
}
```

`send(qty, px)` compiles because `std::uint32_t` converts to `std::int64_t` and
`std::int64_t` converts to `std::uint32_t`. The second conversion is narrowing and will
produce a warning under `-Wconversion`, which most codebases do not enable because it is
noisy. The first produces nothing at all. You have sent 100 lots at a price of 10025 ticks
as an order for 10025 lots at a price of 100.

Argument order is not the only failure. An order id and a sequence number are both
`std::uint64_t`; a millisecond timestamp and a nanosecond timestamp are both
`std::int64_t`; a price in ticks and a price in cents are both `std::int64_t` and differ by
a factor nobody remembers. Every one of these is a type error the compiler is not being
allowed to see.

## A strong typedef, and how far to take it

The fix is a template that wraps a value and takes its identity from a tag type:

```cpp strong.hpp
#include <compare>
#include <utility>

template <typename T, typename Tag>
class Strong {
public:
    using value_type = T;

    Strong() = default;
    constexpr explicit Strong(T v) noexcept : v_(v) {}

    constexpr T get() const noexcept { return v_; }

    friend bool operator==(const Strong&, const Strong&) = default;
    friend auto operator<=>(const Strong&, const Strong&) = default;

private:
    T v_{};
};
```

```cpp domain.hpp
#include "strong.hpp"
#include <cstdint>

struct PriceTag; struct QtyTag; struct OrderIdTag;   // never defined, never instantiated

using Price   = Strong<std::int64_t,  PriceTag>;
using Qty     = Strong<std::uint32_t, QtyTag>;
using OrderId = Strong<std::uint64_t, OrderIdTag>;

// Only the operations that mean something.
constexpr Qty operator+(Qty a, Qty b) noexcept {
    // Braces would reject the int the promotion produces, so widen deliberately.
    return Qty{static_cast<std::uint32_t>(a.get() + b.get())};
}
constexpr std::int64_t notional(Price p, Qty q) noexcept {
    return p.get() * static_cast<std::int64_t>(q.get());
}
```

The constructor is `explicit`, so `Price p = 100;` does not compile and `Price p{100};`
does. There is no conversion operator to `T`, so a `Price` cannot silently become an
integer and drift back into the untyped world. The defaulted `operator<=>` gives you all six
comparisons, but only against another `Price`. `Qty + Qty` is a quantity, so it is defined;
`Price + Price` is meaningless, so it is not; `Price * Qty` is a notional, so it gets a
named function rather than an operator.

Now the original bug is a build failure:

```cpp
#include "domain.hpp"

bool send(Price limit, Qty qty);

void strategy() {
    Price px{10'025};
    Qty   qty{100};
    // send(qty, px);      // error: no conversion from Qty to Price
    send(px, qty);         // the only thing that compiles
}
```

:::hft
The cost is exactly zero, and you can check that rather than take it on faith. `Price` is a
class with one `std::int64_t` member, no user-provided copy or destructor, and no virtual
functions, so it is trivially copyable and 8 bytes. Under the System V ABI that means it
passes in a register, identically to a bare `std::int64_t`. `notional(Price, Qty)` compiles
to:

```asm g++ -std=c++23 -O2 -masm=intel
notional(Price, Qty):
        mov     eax, esi          ; Qty arrives in esi, zero-extended
        imul    rax, rdi          ; Price arrives in rdi
        ret
```

That is byte-for-byte what the raw `std::int64_t` version produces. Strong types are a
compile-time construct that leaves no trace in the binary; the only thing they cost is the
`.get()` calls at the boundary where you talk to the exchange API.
:::

How far to take it is a judgement call, and the answer is not "everything". Wrap a value
when it satisfies at least one of these:

- It shares an underlying type with something it sits next to in a signature. Price and
  quantity, order id and sequence number.
- It carries a unit that has been got wrong before. Ticks against cents, nanoseconds against
  microseconds.
- It has a validity condition worth enforcing once, in the constructor, rather than at every
  use.

Do not wrap loop counters, buffer sizes, or anything that only ever appears alone. A
codebase where every integer is a distinct type spends its days writing conversion functions,
which is a different way to introduce the same bugs.

:::exercise
Add a `Ticks` strong type for a *price difference*, then define exactly three operations:
`operator-(Price, Price) -> Ticks`, `operator+(Price, Ticks) -> Price`, and
`operator/(Ticks, std::int64_t) -> Ticks`. Deliberately do not define `operator+(Price,
Price)`. Write `Price mid(Price bid, Price ask)` using only those three. Then compile at
`-O2 -S` and confirm the body is a subtract, a shift and an add, with no calls and no stack
frame, and that adding two prices directly still fails to compile.
:::

## Takeaways

- Plain `enum` leaks its names into the enclosing scope and converts implicitly to `int`.
  `enum class` fixes both and costs nothing.
- Always fix the underlying type with `: std::uint8_t` or similar. It controls struct size
  and makes the in-memory value match the wire byte.
- Switch over a scoped enumeration with no `default` label so `-Wswitch` reports every case
  you forget when the enumeration grows.
- Bitmask flags belong in an `enum class` with explicit `constexpr` `|` and `&` overloads,
  not in a plain `enum`.
- A `Strong<T, Tag>` wrapper with an `explicit` constructor and only the operators that make
  sense turns transposed arguments into compile errors.
- Strong types are trivially copyable and pass in registers. The generated assembly is
  identical to the raw integer version.
