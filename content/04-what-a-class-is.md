---
title: What a Class Really Is
part: Part I - Foundations
summary: A class is a struct with rules attached. The hidden this pointer, the member initialiser list, RAII, and the layout properties that decide whether you can memcpy it into a ring buffer.
time: 35 min
level: beginner
tags: classes, raii, invariants, layout
---

Almost everything people believe about C++ classes comes from Java or Python and is wrong
here. A C++ class is not a heap object with a header. It has no runtime type by default, no
vtable unless you ask for one, and its members sit in memory in the order you declared them.
It is a struct plus two things the compiler enforces for you: an access boundary, and a
guarantee that a destructor runs at a known instant. Both are worth real money on a hot path.

## struct, class and the hidden this

`struct` and `class` differ in exactly one respect: members are `public` by default in a
`struct` and `private` in a `class`. Nothing else. Both can have constructors, member
functions, inheritance and everything else.

```cpp
#include <cstdint>

struct Level {              // public by default: a plain data carrier
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

class Position {            // private by default: has an invariant to protect
public:
    std::int64_t net() const noexcept { return long_ - short_; }
private:
    std::int64_t long_  = 0;
    std::int64_t short_ = 0;
};
```

The convention that has settled across the industry is: `struct` when the members are the
interface and there is no invariant to maintain, `class` when there is.

A member function is not stored in the object. It is an ordinary function with one extra
argument, a pointer to the object, spelled `this` inside the body. `Position::net` compiles
to something with the signature `std::int64_t Position_net(const Position* this)` and is
called with the object's address in `rdi`. That is why `sizeof(Position)` is 16 and not 16
plus room for a function pointer, and why adding a hundred member functions costs the
object nothing.

You may write `this->long_` but you rarely need to. The one case where it is required is
inside a template that inherits from a dependent base class, which lesson 9 covers.

## Constructors and the member initialiser list

A constructor runs when an object comes into existence. The part before the opening brace,
the **member initialiser list**, is where members are constructed. The part inside the
braces runs afterwards, when every member already exists.

```cpp
#include <cstdint>
#include <vector>

class Ladder {
public:
    Ladder(std::uint32_t depth, std::int64_t tick_size)
        : depth_(depth),                // constructed, in declaration order
          tick_size_(tick_size),
          prices_(depth)                // uses depth_, which is already alive
    {
        // body: everything above is constructed by the time we get here
    }

private:
    std::uint32_t             depth_;
    std::int64_t              tick_size_;
    std::vector<std::int64_t> prices_;
};
```

Three rules follow, and the last one surprises people.

**The list is not optional for some members.** A `const` member and a reference member can
only be initialised, never assigned, so they must appear in the list. There is no body-based
alternative.

**Assigning in the body is double work.** Write `prices_ = std::vector<std::int64_t>(depth);`
in the body and you default-construct an empty vector first, then destroy it and move-assign
another. For a `std::string` or `std::vector` member that is a wasted construction, and
possibly a wasted allocation, on every object you create.

**Members are initialised in declaration order, not list order.** If you declare `prices_`
before `depth_` and write `: depth_(depth), prices_(depth_)`, then `prices_` is constructed
first and reads `depth_` before it holds anything. The list order is a lie the compiler
silently reorders. `-Wreorder`, part of `-Wall`, tells you when the two disagree; treat it
as an error.

Default member initialisers, the `= 0` on `long_` above, apply whenever a constructor's list
does not mention that member, which is the cheapest way to eliminate an entire class of
uninitialised-read bugs.

## The special members

Six functions the compiler will write for you if you do not: the default constructor, the
destructor, the copy constructor, the copy assignment operator, the move constructor and
the move assignment operator. You can request one explicitly, or forbid it.

```cpp
#include <cstdint>

class SessionToken {
public:
    SessionToken() = default;
    ~SessionToken() = default;

    SessionToken(const SessionToken&)            = delete;   // not copyable
    SessionToken& operator=(const SessionToken&) = delete;
    SessionToken(SessionToken&&)                 = default;  // but movable
    SessionToken& operator=(SessionToken&&)      = default;

private:
    std::uint64_t id_ = 0;
};
```

`= default` asks for the compiler's version and, crucially, keeps the type **trivial**.
Writing an empty body `{}` instead does not: a user-provided constructor or destructor,
even an empty one, makes the type non-trivial and therefore non-trivially-copyable, which
changes how it is passed and whether you may `memcpy` it. `= default` on the declaration
line and `{}` are not synonyms; the difference is visible in `std::is_trivially_copyable_v`.

`= delete` removes a function from the overload set. A deleted copy constructor is a
compile error at the copy site, which is exactly what you want for a type that owns a
unique resource or one that must never be duplicated onto a hot path by accident.

## RAII: the destructor as a scheduling primitive

The destructor runs when the object's scope ends, deterministically, including when an
exception unwinds through. That guarantee is the foundation of **RAII**, Resource
Acquisition Is Initialisation: acquire in the constructor, release in the destructor, and
the release cannot be forgotten because it is not something anybody has to remember.

```cpp cycle_timer.hpp
#include <cstdint>
#include <x86intrin.h>          // x86-only: __rdtsc

// Records the cycle cost of the enclosing scope into `sink`.
class ScopedCycleTimer {
public:
    explicit ScopedCycleTimer(std::uint64_t& sink) noexcept
        : sink_(sink), start_(__rdtsc()) {}

    ~ScopedCycleTimer() noexcept { sink_ = __rdtsc() - start_; }

    ScopedCycleTimer(const ScopedCycleTimer&)            = delete;
    ScopedCycleTimer& operator=(const ScopedCycleTimer&) = delete;

private:
    std::uint64_t& sink_;      // reference member: must be in the initialiser list
    std::uint64_t  start_;     // declared second, so initialised second
};
```

```cpp
#include "cycle_timer.hpp"
#include <cstddef>

void decode(const std::byte* buf, std::size_t n) noexcept;

std::uint64_t g_decode_cycles = 0;

void on_packet(const std::byte* buf, std::size_t n) {
    ScopedCycleTimer t{g_decode_cycles};
    decode(buf, n);
}                              // timer fires here, on every exit path
```

`__rdtsc` does not serialise, so the reading can drift by a few cycles either side of where
you think it is; lesson 26 does this measurement properly. The structural point stands: the
timer cannot leak, cannot be forgotten on an early `return`, and costs nothing at `-O2`
beyond the two `rdtsc` instructions because the object never leaves registers.

:::hft
The same pattern is how desks guard risk state. A `ScopedPositionGuard` that adds a
working quantity to the in-flight exposure in its constructor and removes it in its
destructor is correct on every path out of the order-entry function, including the ones
added six months later by someone who did not read the whole function. Manual
increment/decrement pairs are correct on the day they are written and wrong after the third
early `return` is added. RAII moves the guarantee from code review to the type system.
:::

## Invariants, explicit, and aggregates

Encapsulation is not about hiding secrets. It is about naming the set of statements that
are true of every object of the type, the **class invariant**, and controlling every path
that could break it. `Position` above has the invariant "long_ and short_ are both
non-negative". Make them public and that statement is no longer something you can verify by
reading the class; it becomes something you verify by reading the whole program.

A single-argument constructor that is not `explicit` defines an implicit conversion, and
implicit conversions build surprising overload sets:

```cpp
#include <cstdint>

class Price {
public:
    Price(std::int64_t ticks) : ticks_(ticks) {}   // implicit: probably a mistake
    std::int64_t ticks() const noexcept { return ticks_; }
private:
    std::int64_t ticks_;
};

void send(Price limit);

void trade() {
    send(100);          // compiles. Is 100 a price, a quantity, or an order id?
}
```

Mark it `explicit` and `send(100)` stops compiling while `send(Price{100})` still works.
The rule is: every constructor callable with one argument is `explicit` unless you have a
specific reason. C++20 added `explicit(bool)` for the conditional case in templates.

At the other end, a type with no user-declared constructors, no private non-static data and
no virtual functions is an **aggregate**, and you initialise it by listing members in braces.
C++20 lets you name them:

```cpp
#include <cstdint>

struct OrderRequest {
    std::uint64_t id           = 0;
    std::int64_t  price_ticks  = 0;
    std::uint32_t qty          = 0;
    std::uint32_t venue        = 0;
};

// Designated initialisers must appear in declaration order.
constexpr OrderRequest kProbe{.id = 7, .price_ticks = 10'025, .qty = 100};
// .venue is not listed, so it takes its default member initialiser: 0.
```

Static data members belong to the class, not to any object. Since C++17 you can define one
inline in the header:

```cpp
#include <cstdint>

struct RiskLimits {
    inline static constexpr std::uint32_t kMaxQty     = 1'000'000;
    inline static constexpr std::int64_t  kMaxNotional = 50'000'000'000;
};
```

`inline` here means "one definition per translation unit, all identical", so there is no
separate `.cpp` line and no ODR violation. `constexpr` static members were implicitly inline
even before C++17.

## Layout, triviality and putting a struct on the wire

Members occupy memory in declaration order, with padding inserted so each lands on its
required alignment. Two properties of the resulting layout decide what you are allowed to do
with the bytes.

**Standard layout** means the object has no virtual functions, no virtual bases, all
non-static data members have the same access control, and only one class in the hierarchy has
data members. Such a type has a defined, predictable byte layout, and the address of the
object equals the address of its first member. That is what makes it safe to describe in an
interface definition and share with a C program or another language.

**Trivially copyable** means all copy and move constructors, all copy and move assignment
operators, and the destructor are trivial, and at least one of them is not deleted. For such
a type, copying the object's bytes with `memcpy` is guaranteed to produce an equivalent
object. That is the property a shared-memory ring buffer or a wire serialiser depends on.

```cpp wire.hpp
#include <cstdint>
#include <type_traits>

struct alignas(8) NewOrder {
    std::uint64_t client_id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint16_t venue;
    std::uint8_t  side;
    std::uint8_t  flags;
};

static_assert(sizeof(NewOrder) == 24);
static_assert(std::is_trivially_copyable_v<NewOrder>);
static_assert(std::is_standard_layout_v<NewOrder>);
```

Those three assertions belong next to any type that crosses a process, machine or language
boundary. They will fire the day somebody adds a `std::string venue_name;`, a `virtual`
function, or a hand-written destructor, each of which silently destroys the property the
serialiser was relying on. A virtual function additionally prepends an 8-byte vtable pointer,
so `sizeof` changes and every field moves.

:::warn
Adding a user-provided destructor, even `~NewOrder() {}`, makes the type non-trivially
copyable. The `memcpy` in your ring buffer becomes undefined behaviour, the ABI stops
passing it in registers, and nothing warns you. Use `= default` when you want the compiler's
version, and write a body only when you genuinely need one.
:::

:::exercise
Reorder `NewOrder` so the members interleave widths: `side`, `client_id`, `flags`, `venue`,
`qty`, `price_ticks`. Print `sizeof` and confirm it is now 32 rather than 24, a third of the
struct wasted on padding. Then restore the original order, add `std::string tag;` and watch
both triviality and standard-layout assertions fail. Finally add `virtual void touch();` and
print `sizeof` again to see the vtable pointer appear at offset 0.
:::

## Takeaways

- `struct` and `class` differ only in default access. Use `struct` for data with no
  invariant, `class` when there is one to protect.
- A member function is a free function with a hidden `this` argument. Member functions cost
  the object nothing in size.
- Initialise in the member initialiser list, not the body, and remember that initialisation
  follows declaration order regardless of what you wrote.
- `= default` preserves triviality; an empty user-written body destroys it. That difference
  decides whether the type passes in registers and whether `memcpy` on it is legal.
- RAII makes release a property of scope rather than of discipline. Scoped timers and
  position guards are correct on every exit path, including the ones added later.
- Assert `std::is_trivially_copyable_v` and `std::is_standard_layout_v` on every type that
  goes into shared memory or onto the wire.
