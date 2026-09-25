---
title: Interactive C++: A REPL for Asking Questions
part: Part I - Foundations
summary: The edit-compile-run loop is right for building systems and wrong for asking questions. An incremental C++ session answers layout, decoding and arithmetic questions in seconds, using your production headers.
time: 30 min
level: beginner
tags: clang-repl, incremental-compilation, exploration, workflow
---

Lesson 01 showed how a program is built: whole translation units, compiled, linked, run
from `main`. That loop is correct for building a trading system and slow for asking
questions about one. Most questions on a desk are small. How big is this message struct,
and where is the padding? What does this captured packet decode to? What does the book look
like after these five messages? Does this rounding function do the right thing on a negative
price? Each deserves an answer in seconds, not a new file, a build target and a `main`.

An incremental C++ session gives you that. It is a C++ REPL (read, evaluate, print loop):
you type a declaration or a statement, it compiles just that input, runs it, and keeps
everything alive for the next one. The implementation that ships with LLVM is `clang-repl`,
and it uses the same Clang front end as your production compiler, so the types, layouts and
arithmetic it shows you are the real ones.

:::hft Why not just prototype in Python?
Because the prototype and the production code then disagree, and they disagree exactly
where it matters. A quant explores a pricing rule in Python with floats; an engineer
reimplements it in C++ with `std::int64_t` ticks; the two differ by one tick at a rounding
boundary and someone spends a week reconciling them. Exploring in C++ with the production
headers removes the translation step. The code you poked at is the code you ship.
:::

## Two execution models

The session behaves differently from a compiled program in ways that are not obvious, and
every surprise below follows from one fact: each input is compiled as its own small
translation unit and linked into a process that never restarts.

| | Compiled binary | Incremental session |
|---|---|---|
| Unit of compilation | a whole `.cpp` file | each input you type |
| State | fresh on every run | persists until you quit |
| Entry point | `main` required | none; top-level statements just run |
| Redefining a name | build fails | that one input is rejected, the session continues |
| `#include` | per translation unit | once per session; guarded headers are no-ops after that |
| Order of execution | source order | the order you typed it, including what you forgot |
| Optimisation | whatever you asked for | near `-O0` unless you pass `-O2` |

The row that bites is the order one. A compiled program's behaviour is a function of its
source. A session's behaviour is a function of its history.

## A first session

Install LLVM from your package manager, then start the REPL with the standard you use in
production.

```sh
$ brew install llvm                  # macOS: /opt/homebrew/opt/llvm/bin/clang-repl
$ sudo apt install clang-tools-18    # Ubuntu 24.04: installed as clang-repl-18
$ clang-repl --Xcc=-std=c++23
```

Code blocks labelled `repl` in this lesson are session input: top-level statements and
`%` commands that the REPL accepts and an ordinary `.cpp` file would not.

Flags for the underlying compiler go through `--Xcc`, one flag per `--Xcc`. Add include
paths the same way: `--Xcc=-I/path/to/your/headers`.

:::pitfall
On macOS, let the LLVM REPL use its own standard library and only point it at the platform
SDK for the C headers, with `--Xcc=-isysroot --Xcc=$(xcrun --show-sdk-path)`. Adding the
SDK's copy of libc++ as well puts two versions of the standard library on the include path,
and the first `#include <array>` produces several pages of errors from inside the C
library headers that have nothing to do with your code.
:::

The first question worth asking is a layout audit, because the compiler answers it exactly:

```repl
#include <cstddef>
#include <cstdint>
#include <cstdio>
struct Quote { std::int64_t px; std::uint32_t qty; std::uint8_t side; };
std::printf("size %zu, align %zu, qty@%zu, side@%zu\n", sizeof(Quote), alignof(Quote), offsetof(Quote, qty), offsetof(Quote, side));
```

```text
size 16, align 8, qty@8, side@12
```

Sixteen bytes for thirteen bytes of data: three bytes of tail padding, so the struct is a
multiple of its eight-byte alignment. That is lesson 21's material, answered in one line
without a build.

## State persists, and so do mistakes

Every input stays alive, which is what makes the session useful:

```repl
std::int64_t position = 0;
void on_fill(std::int64_t signed_qty) { position += signed_qty; std::printf("position %lld\n", (long long)position); }
on_fill(100);
on_fill(-40);
```

```text
position 100
position 60
```

Now try to start over by redeclaring:

```repl
std::int64_t position = 0;
```

```text
error: redefinition of 'position'
error: Parsing failed.
```

The input is rejected and the session carries on with `position` still at 60. That is the
first habit: once a name exists, **assign to it, do not redeclare it**. `position = 0;`
works.

The deeper consequence is that the session's state is the sum of every input you typed,
including the ones you have scrolled past and forgotten. A result you got at 14:00 may
depend on a variable you changed at 13:40. The rule that follows is the most important one
in this lesson: before you believe or share a result, **replay the session from nothing**.

:::warn
A failed `#include` can leave the session half-initialised. Declarations that parsed before
the error are in scope and the rest are not, so the next input fails with a cascade of
unrelated-looking errors. Do not try to repair it by re-including. Quit, fix the cause, and
replay.
:::

## Five habits for a stateful session

**Assign, do not redeclare.** Covered above. Redeclaring is an error or, worse, a shadowing
declaration inside a scope that makes you think you changed something you did not.

**Version in namespaces to compare implementations.** When you want to try a second version
of a function, do not fight the redefinition rule. Give each version its own namespace and
run both on the same inputs:

```repl
namespace v1 { \
std::int64_t round_to_tick(std::int64_t px, std::int64_t tick) { return px / tick * tick; } \
}
namespace v2 { \
std::int64_t round_to_tick(std::int64_t px, std::int64_t tick) { \
    const std::int64_t q = px / tick, r = px % tick; \
    return (r < 0 ? q - 1 : q) * tick; \
} \
}
std::printf("px -7, tick 5:  v1 %lld   v2 %lld\n", (long long)v1::round_to_tick(-7, 5), (long long)v2::round_to_tick(-7, 5));
```

```text
px -7, tick 5:  v1 -5   v2 -10
```

The trailing backslashes matter. The terminal REPL treats every line as a separate input,
so a function body spread over several lines has to be joined into one input, and a
backslash at the end of a line does that.

That is a real bug found in one line. Integer division in C++ truncates toward zero, so
`v1` rounds a negative price *up* to -5 while claiming to round down. Positive prices hide
it completely. Negative values are ordinary on a desk: calendar spreads, basis, P&L, and
occasionally the outright price of a future. Lesson 40 covers rounding rules properly; the
point here is that side-by-side namespaces made the disagreement visible immediately.

**Put types in headers and include them.** Redefining a struct is an error, so iterating on
a type by retyping it does not work. Put the type in a header with `#pragma once` and
include it; including it again is then a no-op. This is also simply the right structure,
because the header is what production includes.

:::pitfall
In the terminal REPL every line is a separate input, so an `#ifndef ... #endif` guard typed
across several lines fails with "unterminated conditional directive". Use a header, or end
each line of a multi-line input with a backslash so the REPL joins them into one input.
Front ends that submit a whole block of text as a single input do not have this problem,
which is why the guard idiom works in some interactive tools and not in the terminal.
:::

:::warn
Forgetting the backslashes is worse than an error when input is piped from a file. The REPL
receives `void run() {` as a complete input, rejects it, then rejects every following line
of the body, and on some versions never recovers: it prints "expected expression" in a
loop at full CPU and appears to be running a slow computation. If a replayed session sits
at 100% CPU and prints nothing useful, look for a multi-line construct without
continuations before you suspect your code.
:::

**Bound every loop.** A busy-poll loop is the normal shape of trading code, and in a session
it never returns, taking every piece of state you have built with it. Give exploratory loops
an iteration cap. If you need to see a polling loop run, run a fixed number of iterations.

**Retract with `%undo`.** The REPL keeps a history of inputs and can roll back the most
recent one, including its declarations:

```repl
int levels = 10;
%undo
int levels = 32;
```

The second declaration succeeds, because the first no longer exists. `%undo` is the precise
tool; quitting and replaying is the reliable one.

## Driving your real code

The session earns its place when it runs production code, not toy examples. Point it at your
headers and use them directly. With the capstone from lesson 41:

```sh
$ clang-repl --Xcc=-std=c++23 --Xcc=-Iexamples/capstone
```

```repl
#include "book.hpp"
#include <cstdio>
#include <memory>
auto book = std::make_unique<hft::Book>(hft::Price{100'000});
book->on_add({1, hft::Price{99'998}, 300, hft::Side::Buy});
book->on_add({2, hft::Price{99'999}, 100, hft::Side::Buy});
book->on_add({3, hft::Price{100'002}, 250, hft::Side::Sell});
std::printf("bid %lld x %u   ask %lld x %u   spread %lld\n", (long long)book->best_bid().v, book->bid_qty(), (long long)book->best_ask().v, book->ask_qty(), (long long)book->spread());
book->on_reduce(2, 0);
std::printf("after cancel: bid %lld x %u   spread %lld\n", (long long)book->best_bid().v, book->bid_qty(), (long long)book->spread());
std::printf("sizeof(Book) %zu\n", sizeof(hft::Book));
```

```text
bid 99999 x 100   ask 100002 x 250   spread 3
after cancel: bid 99998 x 300   spread 4
sizeof(Book) 3702816
```

Three findings in eleven lines. The book builds and cancels correctly, the best bid falls
back to the next level when the top empties, and the whole structure is 3.7 MB. That last
number is why the capstone allocates it once at startup with `make_unique` rather than
declaring it as a local. As a local it would take nearly half of a typical 8 MB main-thread
stack, and it would overflow outright on a platform whose secondary threads default to a
512 KB stack, which is the case on macOS.

You can also load a compiled library and call into it:

```sh
$ clang++ -std=c++23 -O2 -shared -fPIC risk.cpp -o librisk.so
```

```repl
%lib ./librisk.so
extern "C" bool within_band(std::int64_t px, std::int64_t ref, std::int64_t band);
std::printf("%d %d\n", within_band(100500, 100000, 500), within_band(100501, 100000, 500));
```

```text
1 0
```

The library runs at the optimisation level it was built with. Only the lines you type are
compiled by the session. That distinction matters for the next section.

## What it is not: a benchmark harness

The session is for correctness and layout questions. It is useless for performance ones, and
the reason is not subtle. The same loop, summing notional over a million prices, timed four
ways on one laptop:

| How the loop was compiled | ns per element |
|---|---|
| Compiled binary, `-O2` | 0.47 |
| Session with `--Xcc=-O2`, whole loop in one included header | 0.46 |
| Session with `--Xcc=-O2`, per-price helper typed as its own input | 1.73 |
| Session, default flags | 4.9 to 5.2 |
| Compiled binary, `-O0` | 5.7 to 7.1 |

Figures are the range over three runs on an Apple M-series laptop with LLVM 21. The
absolute values will differ on your machine; the ratios are the point. The same two
split-versus-single-input sessions on an x86-64 Linux server with LLVM 18 measured 0.31 and
0.96 ns per element, a 3.1x penalty against 3.7x here.

Three things follow. The session's default is an unoptimised build, roughly ten times
slower than production, so an untuned session tells you nothing about speed. With `-O2` and
everything in one input it matches the compiled binary, so the JIT itself is not the
problem. But the moment a helper is typed as a separate input, the same code at the same
optimisation level is 3.7 times slower. Each input is its own translation unit, and as
lesson 01 explained, the optimiser cannot inline across a translation unit boundary.

So a timing taken in a session depends on how you happened to type the code, not on the
code. That is the precise reason not to benchmark here, and it holds even when every flag is
right. There is also no link-time or profile-guided optimisation and the process is an
unpinned interactive tool. `sizeof`, `offsetof`, `alignof` and arithmetic results are exact.
Timings are not. Take performance questions to lessons 26 and 27.

## Structuring a session you can replay

A useful session is one somebody else can run. Save the inputs to a file, in the order that
works from nothing, and replay it:

```sh
$ clang-repl --Xcc=-std=c++23 --Xcc=-Iexamples/capstone < book_session.repl
```

Lay the file out in the order a reader needs it:

1. **Setup.** Every `#include` and every compiler flag assumption, first.
2. **Types.** Structs, enums and constants, ideally by including production headers.
3. **Definitions.** Functions under test, versioned in namespaces if you are comparing.
4. **Scenario.** Build the state: a book, a position, a decoded packet.
5. **Checks.** The outputs you care about, printed, with the expected value alongside.

A replayable session file is small, readable in review, and belongs in the repository next
to the code it explores. It turns "I checked this in a REPL last week" into something a
colleague can rerun in two seconds.

Every session in this lesson ships as a file in `examples/repl/`, with a script that finds
your `clang-repl`, applies the right flags for your platform, builds the shared library, and
replays each one with a time limit:

```sh
$ examples/repl/run.sh
```

The outputs quoted above are what it prints. Read the session files as well as the output:
they are the structure described here, applied.

:::exercise
Start a session with the capstone headers on the include path. Build a book, add three
orders on each side, cancel the best bid, and print the new top of book. Then write two
versions of a function that converts a decimal price string such as `"99.9975"` to integer
ticks at a tick size of 0.0025, in namespaces `v1` and `v2`, and find an input where they
disagree. Finally, save everything that worked into `book_session.repl`, quit, and replay the
file from nothing. If the replay fails, you have just found a dependency on state you did not
know you had.
:::

## Takeaways

- An incremental session compiles each input on its own and keeps the process alive, so
  state accumulates in the order you typed it, not the order of any source file.
- Use it for questions with exact answers: layout, decoding, book state, arithmetic edge
  cases. Never use it to measure speed.
- Assign rather than redeclare, compare versions in namespaces, keep types in headers, bound
  every loop, and use `%undo` to retract.
- Drive production headers and libraries, not copies, so the exploration and the shipped
  code cannot drift apart.
- Before trusting a result, replay the session from nothing. Keep sessions worth keeping as
  replayable files in the repository.
