---
title: Lambdas and the Cost of a Callable
part: Part II - Modern C++23
summary: Every lambda is a struct you did not have to write. Passing one as a template parameter costs nothing; wrapping it in std::function costs an indirect call, a lost inline, and sometimes a malloc.
time: 30 min
level: intermediate
tags: lambdas, closures, std-function, type-erasure, inlining
---

A lambda is not a function. It is a compiler-generated class with an `operator()`, and the
whole performance story follows from that one fact. If the compiler knows the exact class, it
inlines the call and the abstraction disappears. If you hide the class behind `std::function`,
the compiler knows nothing, and every call becomes an indirect jump it cannot see through.
The gap between those two is a few nanoseconds per call plus every optimisation that was
blocked, and on a callback in a feed handler that is the difference that matters.

## Syntax, and what a lambda really is

```cpp
#include <cstdint>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

int main() {
    std::int64_t limit = 100'000;

    auto is_cheap = [limit](const Order& o) noexcept -> bool {
        return o.price_ticks < limit;
    };

    Order o{1, 99'000, 10};
    return is_cheap(o) ? 0 : 1;
}
```

The parts are the **capture list** `[limit]`, the parameter list, optional specifiers
(`noexcept`, `mutable`, `constexpr`, `static`), an optional trailing return type, and the
body. Everything except the capture list and the body is optional.

`auto` is required for the variable's type because you cannot name the type of a lambda. Each
lambda expression creates a distinct, unnamed class type, called the **closure type**. Two
lambdas with identical text have different types. Here is what the compiler generated above,
written out by hand:

```cpp The closure type, spelled explicitly
#include <cstdint>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

struct IsCheap_closure {
    std::int64_t limit;                                    // one member per captured entity

    bool operator()(const Order& o) const noexcept {       // const unless the lambda is mutable
        return o.price_ticks < limit;
    }
};

int main() {
    std::int64_t limit = 100'000;
    IsCheap_closure is_cheap{limit};
    Order o{1, 99'000, 10};
    return is_cheap(o) ? 0 : 1;
}
```

Nothing else is going on. The closure has no vtable, no allocation, and `sizeof` is the sum of
its captures with padding. A capture-less lambda is an empty class of size 1, and it also
converts implicitly to a plain function pointer.

## Captures

| Form | Meaning |
|---|---|
| `[x]` | copy `x` into a member at the point the lambda is created |
| `[&x]` | store a reference to `x`; `x` must outlive the closure |
| `[=]` | copy every entity the body uses. Does not capture `this` in C++20 and later |
| `[&]` | reference every entity the body uses |
| `[this]` | capture the *pointer*, so members are read through it |
| `[*this]` | copy the whole enclosing object into the closure |
| `[p = std::move(q)]` | init-capture: a new member initialised from any expression |
| `mutable` | make `operator()` non-`const`, so by-value captures can be modified |

Init-capture is how a closure takes ownership, and it is the only way to move something in.

```cpp
#include <cstdint>
#include <memory>
#include <vector>

int main() {
    auto buf = std::make_unique<std::int64_t[]>(1024);

    auto writer = [buf = std::move(buf), n = 0u](std::int64_t px) mutable {
        buf[n++] = px;                 // needs mutable: n is a by-value member
    };
    writer(101'250);
}
```

The dangerous capture is `this`. It stores a raw pointer, and the members you touch in the
body are read through it every call.

:::warn
```cpp
#include <cstdint>
#include <functional>

class Strategy {
public:
    std::function<bool(std::int64_t)> make_filter() {
        return [this](std::int64_t px) { return px > threshold_; };   // captures this
    }
private:
    std::int64_t threshold_ = 100'000;
};
```

If the returned closure outlives the `Strategy`, every call reads freed memory and returns a
plausible answer. This is the single most common lifetime bug in modern C++ callbacks.
`[*this]` copies the object into the closure and removes the hazard, at the cost of copying.
`[threshold_ = threshold_]` copies just the field, which is usually what you meant.
:::

A **generic lambda** has `auto` parameters and turns `operator()` into a template. C++20 lets
you name the parameter, which you need whenever you want the type itself.

```cpp
#include <cstdint>
#include <span>

auto total_qty = []<class Msg>(std::span<const Msg> msgs) noexcept -> std::uint64_t {
    std::uint64_t n = 0;
    for (const Msg& m : msgs) n += m.qty;
    return n;
};
```

## Passing a lambda so the compiler can delete it

Take a callback by template parameter and the closure type is known at the call site. The
compiler inlines `operator()` into the loop, then optimises across the boundary as if you had
written the body there.

```cpp levels.hpp
#pragma once
#include <cstdint>
#include <span>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

template <class F>
constexpr void for_each_level(std::span<const Level> levels, F&& f) {
    for (const Level& l : levels) f(l);
}

inline std::uint64_t depth_above(std::span<const Level> levels, std::int64_t px) {
    std::uint64_t n = 0;
    for_each_level(levels, [&](const Level& l) noexcept {
        if (l.price_ticks > px) n += l.qty;
    });
    return n;
}
```

At `-O2` this compiles to the same loop you would have written by hand: a compare, a
conditional add, and an increment. There is no call, no closure object in memory, and `n`
stays in a register. Check it with `-S -masm=intel`.

`F&&` here is a forwarding reference, so the template accepts lvalue and rvalue callables
alike and copies nothing. Taking `F f` by value is also fine and often simpler; closures are
usually a few bytes.

## What std::function costs

`std::function<R(Args...)>` erases the closure type. It can hold any callable with a
compatible signature, which is exactly why the compiler can no longer see which one.

```cpp
#include <cstdint>
#include <functional>
#include <span>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

void for_each_level(std::span<const Level> levels,
                    const std::function<void(const Level&)>& f) {
    for (const Level& l : levels) f(l);      // indirect call, once per level
}
```

Three costs, in increasing order of importance:

1. **An indirect call.** The object stores a pointer to an invoker function; calling it is a
   load and an indirect branch. Predictable when the same target repeats, but it still
   consumes a branch-target-buffer entry.
2. **Possible heap allocation.** `std::function` has a small-buffer optimisation, and a
   closure that does not fit is allocated with `new` on construction. Both libstdc++ and
   libc++ make `std::function` 32 bytes; the usable inline buffer is 16 bytes in libstdc++
   and 24 in libc++, and libstdc++ additionally requires the closure to be trivially
   copyable to store it inline. Do not trust that sentence, assert it:
   ```cpp
   static_assert(sizeof(std::function<void(const Level&)>) == 32);
   ```
3. **No inlining.** This is the real loss. The compiler cannot see the body, so it cannot
   keep your accumulator in a register across the call, cannot vectorise the loop, and must
   assume the callback may modify anything reachable through a pointer.

On a modern x86 core, expect the call itself to be in the low single-digit nanoseconds and
the lost optimisation to be worth several times that on a tight loop. Both figures depend
entirely on the loop, so measure them with the technique in lesson 27 rather than quoting
mine.

:::hft
The `std::function` in a market-data callback is usually installed once at start-up and
called ten million times a day. That is the worst possible ratio. Type-erase at
configuration boundaries, where the cost is amortised over the life of the process, and use
templates or a non-owning view inside the loop. A single `std::function` on the tick path is
one of the few code-review findings that is worth blocking a merge over.
:::

## The alternatives

**Plain function pointer.** `void (*)(const Level&)` is 8 bytes and holds no state. A
capture-less lambda converts to one implicitly. Still an indirect call, but no allocation and
no wrapper. Pair it with a `void*` context argument, which is the C callback pattern and is
what `function_ref` below formalises.

**A non-owning view.** Most callbacks are called during the call that installs them and never
stored. For those you want a two-pointer view: the callable and a trampoline. C++26 adds
`std::function_ref`; until then, sixteen lines:

```cpp function_ref.hpp Non-owning, 16 bytes, never allocates
#pragma once
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

template <class Sig> class function_ref;

template <class R, class... Args>
class function_ref<R(Args...)> {
public:
    template <class F>
        requires (!std::is_same_v<std::remove_cvref_t<F>, function_ref>)
              && std::is_invocable_r_v<R, F&, Args...>
    function_ref(F&& f) noexcept
        : obj_{const_cast<void*>(static_cast<const void*>(std::addressof(f)))},
          call_{[](void* o, Args... a) -> R {
              using T = std::remove_reference_t<F>;
              return std::invoke(*static_cast<T*>(o), std::forward<Args>(a)...);
          }} {}

    R operator()(Args... args) const {
        return call_(obj_, std::forward<Args>(args)...);
    }

private:
    void* obj_;
    R (*call_)(void*, Args...);
};

static_assert(sizeof(function_ref<void(int)>) == 2 * sizeof(void*));
```

The capture-less lambda in the constructor converts to a function pointer, and that pointer
is the only type information kept. It costs one indirect call and nothing else. It borrows,
so storing one in a member is the same dangling bug as `[this]`.

**Fixed-capacity storage.** When you must own the callable and cannot allocate, use a
buffer of known size and refuse anything larger at compile time. The pattern is
`inplace_function<Sig, N>`: an `alignas` byte array of `N`, a trampoline pointer, and a
`static_assert(sizeof(F) <= N)` in the converting constructor. Boost and SG14 ship one, and
`std::move_only_function` (C++23) still allocates for large closures, so it is not a
substitute. This is the right choice for a stored handler on a hot path: the cost is one
indirect call, and the failure mode is a build error rather than a `malloc` at 09:30.

C++23 adds `static operator()`, which removes the unused `this` argument from a capture-less
callable. One fewer register set up per call, and it matters in comparators invoked millions
of times.

```cpp
#include <algorithm>
#include <cstdint>
#include <vector>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

void sort_book(std::vector<Level>& v) {
    std::ranges::sort(v, [](const Level& a, const Level& b) static noexcept {
        return a.price_ticks > b.price_ticks;
    });
}
```

Requires GCC 13 or Clang 17 and later. Only legal with no captures.

## Choosing, on the hot path

| Mechanism | Size | Allocates | Inlinable | Use when |
|---|---|---|---|---|
| Template parameter | 0 | no | yes | the callable is known at the call site |
| Function pointer | 8 B | no | rarely | C interop, no state needed |
| `function_ref` | 16 B | no | no | callback used only during the call |
| `inplace_function<Sig,N>` | N + 8 B | no | no | stored handler, hot path, no heap |
| `std::function` | 32 B | maybe | no | configuration, start-up, cold paths |
| `std::move_only_function` | 32 B | maybe | no | as above, for move-only closures |

The decision rule is short. If the loop runs per tick, the callable belongs in the type. If
the callable is chosen once from a config file, `std::function` is fine and clearer. Anything
in between: `function_ref` for borrow, `inplace_function` for own.

:::exercise
Write two versions of a function summing the quantity of every level passing a predicate: one
templated on the predicate, one taking `const std::function<bool(const Level&)>&`. Run both
over a `std::vector<Level>` of 1,000 elements, ten million times, and report nanoseconds per
element for each. Then dump both at `-O2 -S` and identify, in the templated version, which
instructions correspond to the predicate. You should not be able to point at a call
instruction.
:::

## Takeaways

- A lambda is a unique unnamed class with an `operator()` and one member per capture. There is
  no runtime machinery beyond that.
- `[this]` captures a pointer, not the object. A closure that outlives its enclosing object
  and captured `this` reads freed memory silently.
- Passing a closure as a template parameter lets the compiler inline the call away entirely.
  This is the default for anything on the tick path.
- `std::function` erases the type, which costs an indirect call, may heap-allocate above a
  16 to 24 byte closure, and blocks inlining. The blocked inlining is the larger cost.
- Reach for a 16-byte `function_ref` when borrowing and a fixed-capacity
  `inplace_function` when owning, so the failure mode is a compile error rather than an
  allocation.
- C++23 `static operator()` removes the unused `this` argument from capture-less callables.
