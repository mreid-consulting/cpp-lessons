---
title: Functions, References and const
part: Part I - Foundations
summary: How arguments actually reach a function on x86-64, why const reference is the wrong default for small types, and the const placement rules that decide what you may modify.
time: 25 min
level: beginner
tags: functions, references, const, abi
---

A function call is not free and it is not opaque. On x86-64 there is a written contract,
the System V ABI, that says exactly which register each argument arrives in and when the
compiler is forced to spill it to memory instead. Knowing that contract turns "pass by
const reference, it's cheaper" from folklore into a rule with a size threshold attached.

## Declarations, definitions and overloads

A function has a return type, a name, and a parameter list. Together the name and the
parameter types form its **signature**, which is what the compiler uses to pick between
candidates and what the linker sees after name mangling.

```cpp
#include <cstdint>

std::int64_t notional(std::int64_t price_ticks, std::uint32_t qty);  // declaration

std::int64_t notional(std::int64_t price_ticks, std::uint32_t qty) { // definition
    return price_ticks * static_cast<std::int64_t>(qty);
}
```

The return type is **not** part of the signature, so you cannot overload on it alone. Two
functions may share a name if their parameter lists differ:

```cpp
#include <cstdint>

std::int64_t mid(std::int64_t bid, std::int64_t ask) { return (bid + ask) / 2; }
std::int64_t mid(std::int64_t bid, std::int64_t ask, std::int64_t tick) {
    return ((bid + ask) / 2 / tick) * tick;      // rounded down to the tick grid
}
```

**Overload resolution** ranks the candidates for a given call. Roughly: an exact match beats
a promotion, which beats a standard conversion, which beats a user-defined conversion. If
two candidates tie, the call is ambiguous and it will not compile. That last part matters
more than it sounds. A call like `notional(100, 5)` where an overload takes `double` and
another takes `std::int64_t` is a coin toss you would rather resolve at the call site than
discover in production, which is why lesson 5 wraps these arguments in strong types.

## Passing arguments: value, reference, const reference

There are three ways to hand an object to a function.

```cpp
#include <cstdint>
#include <vector>

struct Quote {
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t venue;
};                                  // 16 bytes, trivially copyable

void by_value(Quote q);                       // callee gets its own copy
void by_reference(Quote& q);                  // alias; callee may modify caller's object
void by_const_reference(const Quote& q);      // alias; callee may not modify it
void big_by_const_ref(const std::vector<Quote>& book);   // never copy a vector
```

The System V AMD64 ABI, which every Linux and macOS x86-64 compiler follows, passes a
class type in registers when it is **trivially copyable** and **at most 16 bytes** with
members that classify as integer or SSE. Anything larger, and anything with a non-trivial
copy constructor or destructor, is passed in memory: the caller writes it to the stack and
hands over an address.

That is the whole rule of thumb. For a trivially copyable type of 16 bytes or less, pass by
value. `Quote` above arrives in `rdi` and `rsi`, already in registers, and the callee reads
its members with zero loads. Pass the same `Quote` by `const&` and the caller must
materialise an address, which usually means spilling the object to the stack, and the
callee must issue a load through that pointer for every member it touches. You have traded
two register moves for a store, a pointer pass, and two dependent loads.

:::perf
There is a second, larger cost to `const&` on a small type: it introduces a pointer the
compiler must reason about. Two `const Quote&` parameters might alias each other, or alias
a member of the enclosing object, so the compiler is obliged to reload after any store it
cannot prove independent. By-value parameters cannot alias anything, which frees the
optimiser to keep them in registers across the whole function.
:::

The threshold is not exactly 16 bytes for every type. A `std::string` is 32 bytes on
libstdc++ and non-trivially copyable, so by value costs you a potential allocation; take it
by `const&`, or by `std::string_view` (lesson 16). The practical guidance:

| Parameter | How to pass |
|---|---|
| Trivially copyable, up to 16 bytes | by value |
| Trivially copyable, larger than 16 bytes | `const&` |
| Any type with a non-trivial copy or destructor | `const&` |
| Callee needs to modify the caller's object | `T&` |
| Callee needs to take ownership | by value, then move (lesson 7) |

## References as aliases, and how they dangle

A reference is another name for an existing object. It must be initialised, it can never
be rebound to a different object, and it has no null state. At the machine level it is
almost always a pointer, but the language guarantees it refers to something.

```cpp
#include <cstdint>

void widen_spread(std::int64_t& bid, std::int64_t& ask, std::int64_t ticks) {
    bid -= ticks;
    ask += ticks;
}
```

The danger is outliving the thing you alias.

```cpp
#include <cstdint>
#include <vector>

const std::int64_t& best_bid_bad(const std::vector<Quote>& levels) {
    std::int64_t px = levels.front().price_ticks;
    return px;                      // returns a reference to a dead local
}
```

The local `px` is destroyed when the function returns, so the caller receives a reference
to reclaimed stack. GCC and Clang both diagnose this exact shape with
`-Wreturn-local-addr`, but the general problem, a reference that outlives its referent,
is not decidable and no compiler catches all of it. Returning a reference is only safe when
the referent outlives the call: a member of `*this`, an element of a container the caller
owns, or a static.

:::pitfall
Binding a `const&` to a temporary extends the temporary's lifetime to the lifetime of the
reference, which makes `const std::vector<Quote>& v = make_book();` work. That extension
does **not** survive being returned, and it does not apply through a function parameter.
`const Quote& q = make_quotes()[0];` leaves `q` dangling the moment the statement ends,
because the extension applies to the temporary vector's lifetime only when the reference
binds to the temporary itself, not to a subobject reached through `operator[]`.
:::

## const in every position

`const` means "this name may not be used to modify". Where you write it decides what is
protected. Read a declaration right to left:

```cpp
#include <cstdint>

std::int64_t v = 100;

const std::int64_t* p1 = &v;   // pointer to const int64: *p1 = 5 is an error
std::int64_t* const p2 = &v;   // const pointer to int64: p2 = nullptr is an error
const std::int64_t* const p3 = &v;   // neither may change
```

`const T*` and `T const*` are the same thing; the second reads more consistently but the
first is what you will see. The distinction is **low-level const**, which applies to the
pointed-to object, versus **top-level const**, which applies to the object itself.

Top-level `const` on a by-value parameter is ignored when forming the signature, so
`void f(int)` and `void f(const int)` are the same function and redeclaring both is legal
rather than an overload. It still has an effect inside the body: the parameter becomes
immutable there.

Member functions carry the same idea. A trailing `const` promises the function does not
modify the object, and it is what lets you call the function on a `const` object at all:

```cpp
#include <array>
#include <cstdint>

class Book {
public:
    std::int64_t best_bid() const { ++lookups_; return bids_[0]; }  // callable on a const Book
    void apply(std::int64_t px)   { bids_[0] = px; }                // not callable on one

private:
    std::array<std::int64_t, 8> bids_{};
    mutable std::uint64_t       lookups_ = 0;   // writable even from a const function
};
```

`mutable` carves out a member from the const promise. It is correct for genuinely
incidental state such as a hit counter, a memoisation cache or a lazily computed field. It
is a bug for anything that participates in the object's observable value. Lesson 4 develops
const member functions properly.

## Default arguments and [[nodiscard]]

A default argument is substituted **at the call site**, not stored in the function:

```cpp
#include <cstdint>

// tif: 0 = day, 1 = immediate-or-cancel
bool send_order(std::int64_t price_ticks, std::uint32_t qty, int tif = 0);
```

Two consequences follow. First, changing the default in a header changes behaviour in every
translation unit, but only after each is recompiled; ship a stale object file and two parts
of your binary disagree about what "default" means. Second, defaults do not participate in
overload resolution the way you expect, and a defaulted parameter plus an overload with one
fewer parameter is ambiguous by construction. Virtual functions are worse still: the default
is chosen from the static type while the body is chosen from the dynamic type, so a derived
class can never usefully change it. Prefer a named overload, or a small options struct.

`[[nodiscard]]` makes ignoring a return value a warning. On anything that reports failure
or hands back a resource, it is not optional:

```cpp
#include <cstdint>

[[nodiscard]] bool try_reserve(std::uint32_t qty) noexcept;

[[nodiscard("the risk check result must be acted on")]]
bool passes_risk(std::int64_t notional_ticks) noexcept;
```

`noexcept` on a function is a promise that it will not throw; if one escapes anyway the
program calls `std::terminate`. The promise is worth making because it lets the compiler
omit unwinding scaffolding and lets `std::vector` move rather than copy your objects when it
grows. Lesson 18 covers the policy properly.

## Returning more than one thing

C's answer was out-parameters. C++ has a better one, and it is also faster.

```cpp
#include <cstdint>

struct FillResult {
    std::int64_t  avg_price_ticks;
    std::uint32_t filled_qty;
    bool          complete;
};

[[nodiscard]] FillResult fill(std::int64_t limit_ticks, std::uint32_t qty) noexcept;

void on_ack() {
    auto [px, filled, done] = fill(10'025, 100);   // structured bindings, C++17
    if (!done) { /* work the remainder */ }
}
```

`FillResult` is 16 bytes and trivially copyable, so it comes back in `rax` and `rdx`. There
is no memory traffic at all. Contrast the out-parameter form:

```cpp
#include <cstdint>

bool fill_out(std::int64_t limit_ticks, std::uint32_t qty,
              std::int64_t* avg_price_ticks, std::uint32_t* filled_qty);
```

Now the caller must allocate two stack slots, pass two pointers, and the callee must store
through them. Worse, the compiler cannot prove those pointers do not alias each other or
anything else the function touches, so writes through them act as optimisation barriers.
Out-parameters also lose you `const`, force the caller to declare uninitialised variables,
and make it easy to use `avg_price_ticks` when the function returned `false`.

:::hft
The 16-byte return rule shapes hot-path signatures directly. A market data decoder that
returns `struct { std::int64_t price_ticks; std::uint32_t qty; std::uint32_t flags; }` hands
the whole result back in two registers. Add one more `std::uint64_t` field and the return
goes through memory: the caller allocates a hidden buffer, passes its address in `rdi`, and
the callee stores to it. That change is invisible in the source and shows up as a store and
a load per message. Check it with `-S` before you add the field, not after the latency
histogram moves.
:::

:::exercise
Write two versions of `std::int64_t notional(Quote)` and `std::int64_t notional(const Quote&)`
for the 16-byte `Quote` above, compile with `g++ -std=c++23 -O2 -S -masm=intel`, and compare.
The by-value version should be `mov`, `imul`, `ret` with no loads. Then grow `Quote` to 24
bytes by adding a `std::uint64_t seq` and repeat: the by-value version now also goes through
memory, and the two forms converge.
:::

## Takeaways

- The signature is the name plus the parameter types. The return type is not part of it.
- On x86-64 System V, trivially copyable types of 16 bytes or less pass in registers. Pass
  those by value; use `const&` for larger or non-trivially-copyable types.
- `const&` on a small type costs you a spill, a dependent load, and the optimiser's freedom,
  because a reference may alias.
- Never return a reference to a local. Lifetime is your responsibility and the compiler
  only catches the obvious cases.
- Read pointer declarations right to left. `const T*` protects the pointee, `T* const`
  protects the pointer.
- Return a small struct and destructure it with structured bindings rather than using
  out-parameters, which cost memory traffic and block aliasing analysis.
