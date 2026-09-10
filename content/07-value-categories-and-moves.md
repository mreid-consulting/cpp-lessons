---
title: lvalues, rvalues and Move Semantics
part: Part I - Foundations
summary: What the compiler means by an expression having identity, how references bind, why std::move moves nothing, and why a missing noexcept makes std::vector copy your orders instead of moving them.
time: 40 min
level: intermediate
tags: move, rvalue, noexcept, rvo, rule-of-five
---

Move semantics is the feature people most often learn as a ritual: put `std::move` here,
add `&&` there, performance improves. That story is wrong in both directions. `std::move`
generates no instructions at all, and for the plain integer structs that make up a hot
path, a move is byte-for-byte identical to a copy. Understanding *when* moving buys you
something requires understanding what the compiler thinks an expression is.

## Value categories and reference binding

### The three you need

Every expression in C++ has a type and a **value category**. There are three you need.

- An **lvalue** has *identity*: it names a storage location you could take the address of.
  A variable, a dereferenced pointer, a struct member of an lvalue.
- A **prvalue** ("pure rvalue") is a *pure value* with no identity yet: the result of
  `42`, `a + b`, or a function returning by value. It is a recipe for initialising
  something, not a thing sitting in memory.
- An **xvalue** ("expiring value") has identity *and* is safe to cannibalise: the result of
  `std::move(x)`, or of a function returning `T&&`.

The umbrella term **rvalue** means "prvalue or xvalue" — anything you are permitted to
steal from.

```cpp
#include <cstdint>
#include <string>
#include <utility>

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

Order make_order();

void categories() {
    Order o{1, 100'50, 10};
    o;                    // lvalue: has a name, has an address
    o.price_ticks;        // lvalue: member of an lvalue
    make_order();         // prvalue: a value being produced
    std::move(o);         // xvalue: o, marked as expiring
    Order{2, 100'51, 5};  // prvalue: a temporary
}
```

A useful two-question test. *Can I take its address?* If yes it is a glvalue (lvalue or
xvalue). *Is it safe to steal from?* If yes it is an rvalue (prvalue or xvalue). An lvalue
answers yes/no; a prvalue answers no/yes; an xvalue answers yes/yes.

### What binds to what

References are the mechanism by which a function says which categories it will accept.

| Parameter | Binds lvalue | Binds prvalue | Binds xvalue | Meaning |
|---|---|---|---|---|
| `Order&` | yes | no | no | I will modify your object |
| `const Order&` | yes | yes | yes | I will only read it |
| `Order&&` | no | yes | yes | I will gut it |
| `Order` (by value) | yes (copy) | yes (move) | yes (move) | I want my own |

The second row is the important one: **`const&` binds to everything**, including
temporaries, and it extends the temporary's lifetime to the lifetime of the reference.
That is why `const&` was the universal parameter type for twenty years, and why it remains
the correct default for any type you do not intend to keep.

```cpp
#include <cstdint>

std::int64_t notional(const Order& o) {          // accepts named orders and temporaries
    return o.price_ticks * static_cast<std::int64_t>(o.qty);
}

void call_sites() {
    Order named{1, 10050, 10};
    notional(named);                              // lvalue -> const&
    notional(Order{2, 10051, 5});                 // prvalue -> const&, lifetime extended
}
```

An **rvalue reference**, spelled `T&&`, binds only to rvalues. It is not "a reference that
is fast". It is a reference that carries a promise from the caller: *nobody else is
looking at this object, do what you like to it*.

:::pitfall
An rvalue reference variable is itself an **lvalue**. Inside
`void f(Order&& o)`, the parameter `o` has a name, so it is an lvalue, and passing it
onward will copy unless you write `std::move(o)` again. "Has a name" beats "is declared
`&&`" every time.
:::

## std::move is a cast

This is the whole implementation, modulo the exact reference collapsing:

```cpp
template <typename T>
constexpr std::remove_reference_t<T>&& my_move(T&& t) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(t);
}
```

It moves nothing. It allocates nothing. It emits zero instructions. All it does is change
the value category of an expression from lvalue to xvalue, so that overload resolution
picks the `&&` overload instead of the `const&` one. The actual work happens inside
whatever constructor or assignment operator that choice selects.

:::key
`std::move(x)` is a request, not an action. If the receiving type has no move constructor,
or its move constructor is not viable, you silently get a copy. The compiler will not warn
you. `std::move` on a `const` object is a particularly common way to write a copy that
looks like a move — `const Order&&` cannot bind to `Order&&`, so `const Order&` wins.
:::

## Moving a type that owns something

Moves only mean anything for a type that owns a resource. Here is a minimal buffer that
owns heap memory, of the kind you might use to hold a decoded message body off the hot
path.

```cpp message_buffer.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

class MessageBuffer {
public:
    explicit MessageBuffer(std::size_t n)
        : data_(new std::uint8_t[n]), size_(n) {}

    ~MessageBuffer() { delete[] data_; }

    MessageBuffer(const MessageBuffer& other)                 // copy: duplicate the bytes
        : data_(new std::uint8_t[other.size_]), size_(other.size_) {
        for (std::size_t i = 0; i < size_; ++i) data_[i] = other.data_[i];
    }

    MessageBuffer(MessageBuffer&& other) noexcept              // move: steal the pointer
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}

    MessageBuffer& operator=(MessageBuffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    MessageBuffer& operator=(const MessageBuffer& other) {
        if (this != &other) {
            MessageBuffer tmp(other);                          // copy-and-swap
            *this = std::move(tmp);
        }
        return *this;
    }

    std::uint8_t* data() noexcept { return data_; }
    std::size_t   size() const noexcept { return size_; }

private:
    std::uint8_t* data_;
    std::size_t   size_;
};
```

`std::exchange(other.data_, nullptr)` from `<utility>` returns the old value and assigns
the new one in a single expression. It is the canonical move-constructor idiom because it
makes the two obligations of a move — *take the resource* and *leave the source safe to
destroy* — a single line that cannot be half-written.

### Moved-from means valid but unspecified

After `MessageBuffer b = std::move(a);`, the object `a` still exists and its destructor
will still run. The standard requires moved-from library types to be in a **valid but
unspecified** state: you may destroy them, and you may assign a new value to them. You may
not assume what they contain.

```cpp
std::string s = "IBM";
std::string t = std::move(s);
// s.size() is not guaranteed to be 0. It is legal to call s.clear() or s = "MSFT".
// It is a bug to print s and expect anything in particular.
```

Your own types should uphold the same contract. The `nullptr` and `0` above make
`MessageBuffer`'s destructor safe, which is exactly what is required and no more.

### The rules of zero, three and five

If you write any of destructor, copy constructor, copy assignment, move constructor, or
move assignment, you probably need to think about all five. That is the **rule of five**.
Its predecessor, the **rule of three**, is the same statement before moves existed.

The rule that actually matters is the **rule of zero**: write none of them. Let members
that manage their own resources — `std::vector`, `std::string`, `std::unique_ptr` — do the
work, and the compiler generates all five correctly for you.

```cpp
#include <string>
#include <vector>

struct Instrument {              // rule of zero: five special members, all correct, none written
    std::string symbol;
    std::vector<std::int64_t> tick_ladder;
    std::int64_t lot_size;
};
```

Declaring a destructor suppresses the implicit move constructor and move assignment. A
class with a hand-written destructor and no other special members silently copies
everywhere it looks like it moves. If you must write a destructor, write or `= default`
the other four.

## noexcept, or std::vector copies behind your back

`std::vector` offers the **strong exception guarantee** on `push_back`: if it throws, the
vector is unchanged. When it reallocates, it must relocate every existing element into the
new buffer. If a *move* threw halfway through, the old buffer would already be gutted and
the vector could not roll back. If a *copy* throws, the old buffer is untouched and the
new one is simply discarded.

So `std::vector` uses `std::move_if_noexcept`: it moves only when the move constructor is
`noexcept` (or the type is not copyable at all). Otherwise it copies. Every element. Every
growth.

```cpp noexcept_check.cpp
#include <cstdint>
#include <print>
#include <string>
#include <type_traits>
#include <vector>

struct Slow {
    std::string tag;
    Slow(Slow&& o) : tag(std::move(o.tag)) {}     // NOT noexcept
    Slow(const Slow&) = default;
    Slow() = default;
};

struct Fast {
    std::string tag;
    Fast(Fast&& o) noexcept : tag(std::move(o.tag)) {}
    Fast(const Fast&) = default;
    Fast() = default;
};

int main() {
    std::print("Slow: {}\n", std::is_nothrow_move_constructible_v<Slow>);  // false
    std::print("Fast: {}\n", std::is_nothrow_move_constructible_v<Fast>);  // true
}
```

:::warn
This is invisible in every way that matters. The code compiles, the tests pass, and the
only symptom is that a `std::vector<Order>` of ten thousand entries costs ten thousand
string allocations each time it grows. Put
`static_assert(std::is_nothrow_move_constructible_v<T>);` next to any type you store in a
growing container.
:::

## Copy elision, RVO and NRVO

Returning by value looks expensive and usually is not. The compiler is permitted, and
since C++17 in some cases *required*, to construct the returned object directly in the
caller's storage.

```cpp
#include <cstdint>

struct Quote { std::int64_t bid_ticks; std::int64_t ask_ticks; std::uint32_t qty; };

Quote top_of_book_prvalue() {
    return Quote{100'50, 100'51, 200};   // guaranteed elision since C++17: no copy, no move
}

Quote top_of_book_named() {
    Quote q{100'50, 100'51, 200};
    q.qty *= 2;
    return q;                            // NRVO: permitted, not guaranteed, and universal in practice
}
```

Returning a prvalue is **guaranteed copy elision** in C++17 and later: there is no
temporary to elide, because the prvalue initialises the destination directly. The type
does not even need to be movable. Returning a named local is **NRVO** (named return value
optimisation), which the standard permits but does not require; GCC and Clang both do it
at `-O1` and above when there is a single obvious candidate.

:::pitfall
Do not write `return std::move(q);`. It turns a prvalue or an NRVO candidate into an
xvalue, defeats elision, and forces an actual move construction. GCC and Clang both warn
about it under `-Wpessimizing-move`. Return the local by name.
:::

## What this means on a trading system

:::hft The move that is not a move
An `Order` of three integers is **trivially copyable**: no user-provided constructors, no
virtual functions, no members that own anything. For such a type, moving *is* copying —
the same 24 bytes go to the same place with the same `mov` instructions. `std::move` on it
buys precisely zero.

Move semantics only pay when the object owns something off-heap that would otherwise be
duplicated: a `std::string` symbol, a `std::vector` of price levels, a `unique_ptr` to a
session object. Those are exactly the things a hot path should not contain. So the honest
summary for a feed handler or an order gateway is: your hot structs are trivially
copyable, you pass them by value in registers or by `const&`, and move semantics are
something you care about in configuration loading, in test harnesses, and at startup.

Where it *does* bite is the noexcept rule above. A `std::vector<Session>` grown during
market open, whose `Session` has a non-noexcept move, will do a burst of allocation at
exactly the wrong moment. Check the trait, do not assume.
:::

:::exercise
Take `MessageBuffer` above, remove `noexcept` from its move constructor, and add a
`std::vector<MessageBuffer>` that you fill with 1024 buffers of 64 bytes each without
calling `reserve`. Instrument the copy constructor and move constructor with a static
counter and print both at the end. Then restore `noexcept` and run again. You should see
the copy count collapse from several thousand to zero. Finally add `reserve(1024)` and
watch both counts go to zero regardless.
:::

## Takeaways

- **lvalue** means it has identity; **rvalue** means it is safe to steal from. `const&`
  binds to both, which is why it is the default parameter type for anything you only read.
- `std::move` is a `static_cast` to `T&&`. It emits no instructions and moves nothing; it
  only changes which overload is selected.
- A moved-from object is valid but unspecified. Destroy it or assign to it; assume nothing
  else.
- Prefer the **rule of zero**. Writing a destructor silently disables moves, so write or
  `= default` all five. Mark move constructors `noexcept` or `std::vector` copies during
  reallocation; verify with `std::is_nothrow_move_constructible_v<T>`.
- Return locals by name and rely on guaranteed elision and NRVO. Never `return std::move(x)`.
- Moving a trivially copyable struct is a copy. On the hot path, move semantics buy nothing
  because the hot path should not own heap resources.
