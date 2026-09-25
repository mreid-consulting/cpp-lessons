---
title: How a C++ Program Is Built
part: Part I - Foundations
summary: Preprocessor, compiler, linker. Where your code actually goes, why headers are not modules, and which build flags decide whether you are fast.
time: 25 min
level: beginner
tags: toolchain, odr, linkage, flags
---

Every latency problem you will ever chase starts here. Not because the build system is
slow, but because C++ gives the compiler a fixed window of information, and everything
it can see inside that window it can optimise. Everything outside it, it cannot. Learning
where that boundary sits is the first performance skill.

## The four stages

A C++ build is four programs pretending to be one.

```text
main.cpp ──preprocess──> main.i ──compile──> main.s ──assemble──> main.o ──┐
book.cpp ──preprocess──> book.i ──compile──> book.s ──assemble──> book.o ──┴─link──> ./trader
```

1. **Preprocessor** — textual. `#include` splices a file in, `#define` substitutes tokens,
   `#if` deletes lines. It has no idea what a class is.
2. **Compiler** — turns one preprocessed file into assembly. This is where optimisation
   happens, and it can only see this one file.
3. **Assembler** — assembly to machine code in an object file.
4. **Linker** — glues object files together, resolves every symbol to an address.

The unit the compiler sees, one `.cpp` plus everything it included, is a **translation
unit** (TU). Say "TU" and people will assume you have written C++ before.

:::key
The compiler optimises within a translation unit. A function defined in another `.cpp`
is, by default, a black box it must call through. Inlining, constant propagation and
dead-code elimination all stop at the TU boundary unless you enable link-time
optimisation.
:::

## Your first program, and what it costs

```cpp hello.cpp
#include <print>   // C++23

int main() {
    std::print("tick-to-trade starts here\n");
    return 0;
}
```

```sh
$ g++ -std=c++23 -O2 hello.cpp -o hello
$ ./hello
tick-to-trade starts here
```

`main` is where execution begins. Returning `0` means success; the value ends up as the
process exit status. If you omit the `return`, `main` alone among all functions implicitly
returns `0`.

If your compiler is older than GCC 14 or Clang 18, `<print>` may be missing. Use
`#include <cstdio>` and `std::printf` for now; we abandon both on the hot path anyway.

## Headers, declarations, definitions

A **declaration** tells the compiler a name exists and what its type is. A **definition**
provides the body or storage. You may declare a thing many times. You may define it once.

```cpp order.hpp
#pragma once            // include this file at most once per TU
#include <cstdint>

struct Order {          // definition of a type
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

std::int64_t notional(const Order& o);   // declaration only: no body
```

```cpp order.cpp
#include "order.hpp"

std::int64_t notional(const Order& o) {  // the one definition
    return o.price_ticks * static_cast<std::int64_t>(o.qty);
}
```

Any TU that includes `order.hpp` can now call `notional`. The linker matches the call to
the definition in `order.cpp`.

:::pitfall
`#pragma once` is not standard, but every compiler you will meet supports it and it is
faster than include guards. The standard-blessed form is:

```cpp
#ifndef ORDER_HPP
#define ORDER_HPP
/* ... */
#endif
```
:::

## The One Definition Rule

The ODR says: exactly one definition of each function or variable across the whole
program; and if a type or inline function is defined in several TUs, every definition must
be **token-for-token identical**.

Violate the second half, usually by compiling two TUs with different `-D` flags or
different struct packing, and you get no diagnostic at all. The linker picks one
definition arbitrarily and you spend a week debugging a struct whose fields are at
different offsets in different files.

```cpp
// tick.hpp
struct Tick {
#ifdef WITH_TIMESTAMPS
    std::uint64_t recv_ns;      // present in some TUs, absent in others
#endif
    std::int64_t price_ticks;
};
```

:::warn
Compile one file with `-DWITH_TIMESTAMPS` and another without, and both compile, both
link, and `price_ticks` lands at offset 0 in one TU and offset 8 in the other. This class
of bug is silent, non-deterministic, and always found in production.
:::

The escape hatch for "I need this definition in many TUs" is `inline`, which relaxes the
ODR to allow one definition **per TU** as long as they are identical:

```cpp
inline constexpr int kMaxLevels = 32;      // fine in a header
inline std::int64_t half(std::int64_t x) { return x / 2; }
```

## A word on modules

C++20 added modules, which replace textual inclusion with a compiled artefact the compiler
imports directly.

```cpp
export module book;

import <cstdint>;

export struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};
```

A module is parsed once and imported as a binary interface, so the same header is not
re-tokenised in every translation unit, and macros do not leak across the boundary. Build
times improve substantially on large codebases.

They are also, as of GCC 15 and Clang 20, still awkward: build system support is uneven,
the standard library module `import std;` landed late, and mixing modules with a large
existing header tree takes real effort. Most trading codebases are still headers plus
`-flto`, and that is what this series assumes. Know the feature exists, expect it to matter
within a few years, and do not migrate a working low-latency codebase to it this quarter.

:::note
Modules change compile time, not run time. They do not make your code faster, and they do
not remove the need for link-time optimisation to inline across translation units.
:::

## Linkage: who can see your symbol

| Linkage | How | Visible to |
|---|---|---|
| External | default for functions and non-`const` globals | the whole program |
| Internal | `static` at namespace scope, or an anonymous namespace | one TU |
| None | locals | one block |

Prefer internal linkage for anything not part of your public interface. It is not
politeness, it is optimisation: a function only visible in one TU can be inlined,
cloned, or deleted entirely, and it never collides with someone else's symbol.

```cpp
namespace {                       // anonymous namespace: internal linkage
    constexpr int kRingCapacity = 4096;
    bool is_crossed(std::int64_t bid, std::int64_t ask) { return bid >= ask; }
}
```

## Flags that change your latency

```sh
$ g++ -std=c++23 -O3 -march=native -flto -DNDEBUG feed.cpp book.cpp -o trader
```

| Flag | Effect |
|---|---|
| `-O0` | no optimisation, debug default. Never benchmark this. |
| `-O2` | the sensible production baseline |
| `-O3` | more aggressive inlining and vectorisation; measure, do not assume |
| `-march=native` | emit instructions for **this** CPU: AVX-512, BMI2, `lzcnt` |
| `-flto` | link-time optimisation: restores cross-TU inlining |
| `-DNDEBUG` | compiles out `assert` |
| `-fno-exceptions` | no unwinding; a policy choice, see lesson 18 |
| `-g` | debug info. Costs nothing at run time. Always keep it on. |

:::hft
`-march=native` is the single largest free win, and the single easiest way to crash in
production. The binary you build on a Sapphire Rapids dev box will die with an illegal
instruction on an older Skylake server. Build on the target microarchitecture, or pin it
explicitly with something like `-march=icelake-server`.
:::

:::perf
`-flto` matters more than it sounds. Without it, a two-line accessor in `book.cpp` called
from `strategy.cpp` costs you a real call: register spills, a branch, and a return. With
it, the compiler sees through the boundary and the call disappears.
:::

## Seeing the assembly

Get in the habit early. This is the only way to know what the compiler actually did.

```sh
$ g++ -std=c++23 -O2 -S -masm=intel -o - order.cpp | c++filt
```

`-S` stops after compilation and prints assembly. `c++filt` demangles the names. The
online equivalent is Compiler Explorer, and in this series "look at the assembly" always
means one of those two.

:::exercise
Build `order.cpp` twice, once at `-O0` and once at `-O2`, and diff the assembly of
`notional`. At `-O0` you will find a stack frame, two loads and a store. At `-O2` you
should find a single `imul` and a `ret`.
:::

## The other way to run C++

Everything above assumes the whole-program model: compile translation units, link, run from
`main`. That model is right for building systems. For asking a quick question about a type,
a packet or a formula, lesson 01a introduces the alternative: an incremental session that
compiles each input as you type it and keeps the process alive between them. It also turns
out to be the clearest possible demonstration of the translation unit boundary from this
lesson, because in a session every input is its own translation unit.

## Takeaways

- The **translation unit** is the compiler's field of view. Optimisation stops at its edge.
- Declare in headers, define in one `.cpp`, or mark it `inline` and define it in the header.
- ODR violations are silent. Keep flags uniform across every TU in a build.
- Give everything internal linkage unless it is genuinely part of an interface.
- `-O2 -march=<target> -flto -g` is the default posture. Everything else you justify by measurement.
