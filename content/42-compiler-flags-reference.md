---
title: Compiler Flag Reference
part: Appendix
summary: The flags that change your latency, your correctness, or your ability to debug, with the GCC and Clang spellings and the three command lines you actually need.
time: 10 min
level: intermediate
tags: flags, gcc, clang, lto, pgo, sanitizers
---

Flags are the cheapest performance work available and the easiest place to quietly break a
production binary. This page is the lookup table: what each flag does, where GCC and Clang
disagree, and which tempting-looking options are traps. Everything here assumes x86-64
Linux and `-std=c++23`; the concepts appear first in lesson 1 and are used throughout
lessons 19 to 30.

## Optimisation level and target architecture

### Levels

| Flag | What you get | When |
|---|---|---|
| `-O0` | Nothing. Every variable lives on the stack. | Only when stepping in a debugger. Never benchmark it. |
| `-Og` | Optimise without destroying debug info. | The default debug build. |
| `-O1` | Cheap passes, little inlining. | Sanitizer builds, where speed still matters. |
| `-O2` | The production baseline. Inlining, and since GCC 12 a conservative auto-vectoriser. | Default for everything. |
| `-O3` | More inlining, more unrolling, an aggressive vectoriser. | Only after measuring against `-O2`. |
| `-Os` / `-Oz` | Optimise for size; `-Oz` gives up more speed than `-Os`. | Code-size-bound paths, rarely trading. |
| `-Ofast` | `-O3` plus `-ffast-math` plus other non-conforming behaviour. | Never on a trading binary. See the arithmetic section. |

`-O3` is not automatically faster than `-O2`. The extra inlining and unrolling grow the
instruction footprint, and a hot loop that no longer fits in the L1 instruction cache is
slower no matter how good its scheduling is. Lesson 29 covers that trade in detail. The
honest procedure is to build both and compare the p99.9, not the p50.

Clang's `-O2` and `-O3` differ less than GCC's, because Clang's `-O2` already unrolls and
vectorises. Clang additionally has `-O1` as a genuine middle ground and treats `-Og` as an
alias for `-O1`.

### Architecture and tuning

`-march` decides which instructions the compiler is *allowed* to emit. `-mtune` decides
which microarchitecture it schedules for and emits no new instructions. `-march=X` implies
`-mtune=X` unless you override it.

| Flag | Meaning |
|---|---|
| `-march=native` | Everything this build machine supports. Dangerous across a fleet. |
| `-march=icelake-server` | An explicit target. This is what you ship. |
| `-march=x86-64-v2` | SSE4.2, POPCNT. Roughly Nehalem and later. |
| `-march=x86-64-v3` | AVX2, FMA, BMI1/2, LZCNT. Roughly Haswell and later. |
| `-march=x86-64-v4` | AVX-512F/BW/CD/DQ/VL. |
| `-mtune=generic` | Schedule for no machine in particular while allowing `-march` instructions. |
| `-mavx2 -mfma -mbmi2` | Enable individual extensions without moving the whole baseline. |
| `-mprefer-vector-width=256` | Cap the vectoriser at 256 bits on AVX-512 parts. See lesson 25. |

The x86-64-v2/v3/v4 levels are the useful compromise for a mixed fleet: they name a
capability set rather than a chip, so the binary runs everywhere at or above that level.

```sh Finding out what -march=native actually enabled
$ gcc -march=native -Q --help=target | grep -E 'march=|mtune='
$ gcc -march=native -Q --help=target | grep -v '\[disabled\]'
$ gcc -march=native -dM -E - < /dev/null | grep -E '__AVX|__BMI|__FMA'

$ clang -march=native -### -E - 2>&1 | tr ' ' '\n' | grep -- '-target-feature'
```

The first two lines are the ones to remember. GCC's `-Q --help=target` prints every target
option with its resolved value, which is the only reliable way to turn `native` into a
name you can put in a build file. Clang has no equivalent, so you read the driver's own
expanded command line with `-###`.

:::hft
Build machines outlive production hardware. A binary compiled with `-march=native` on a
Sapphire Rapids developer box dies with SIGILL on the Skylake server that is actually
sitting in the colocation cage, and it dies on the first packet that reaches the AVX-512
path, not at startup. Pin the architecture explicitly in the build system, and have CI
assert the resolved `-march` string, so upgrading a build host cannot silently change the
instruction set of tomorrow's release.
:::

## Link-time and profile-guided optimisation

Link-time optimisation restores cross-translation-unit inlining. Profile-guided
optimisation tells the compiler which branches and functions are hot so it can lay the
code out accordingly. Both matter more for tail latency than any single `-O` bump.

| Purpose | GCC | Clang |
|---|---|---|
| LTO | `-flto=auto` | `-flto=thin` (scalable) or `-flto=full` |
| Archive support | use `gcc-ar`, `gcc-nm`, `gcc-ranlib` | use `llvm-ar`, `llvm-nm` |
| Keep normal objects too | `-ffat-lto-objects` | n/a |
| LTO cache | n/a | `-Wl,--thinlto-cache-dir=...` |
| PGO: instrument | `-fprofile-generate` | `-fprofile-generate` |
| PGO: merge | automatic (`.gcda`) | `llvm-profdata merge -output=x.profdata *.profraw` |
| PGO: use | `-fprofile-use -fprofile-correction` | `-fprofile-use=x.profdata` |
| Don't pessimise uncovered code | `-fprofile-partial-training` | n/a |
| Sampling PGO from `perf` | `-fauto-profile` (via `create_gcov`) | `-fprofile-sample-use=` (via `create_llvm_prof`) |

Two rules. LTO must be passed at link time as well as compile time, with the same
optimisation level, or you get either a link error or a quietly worse binary. And a PGO
profile must come from a workload that resembles production; a profile gathered from a
unit-test run will happily mark your error handling as hot. Sampling PGO exists precisely
so you can collect the profile from a real production process with `perf` instead of
running an instrumented binary. BOLT, which reorders the already-linked binary, is covered
in lesson 29.

## Catching bugs: warnings and sanitizers

### Warnings

| Flag | Catches |
|---|---|
| `-Wall -Wextra -Wpedantic` | The baseline. Not "all" warnings despite the name. |
| `-Wshadow` | A local that hides an outer variable or a member. |
| `-Wconversion` | Implicit narrowing that loses value. |
| `-Wsign-conversion` | Signed/unsigned conversions. In C++ this is **not** implied by `-Wconversion`. |
| `-Wold-style-cast` | C casts, which can silently become `const_cast` or `reinterpret_cast`. |
| `-Wnon-virtual-dtor` | Deleting through a base pointer with a non-virtual destructor. |
| `-Wcast-align` | A cast that increases required alignment. |
| `-Wdouble-promotion` | An accidental `float` to `double` promotion. |
| `-Wnull-dereference -Wformat=2 -Wimplicit-fallthrough` | Cheap, high signal. |
| `-Wuseless-cast -Wduplicated-cond -Wduplicated-branches -Wlogical-op` | GCC only, all worth it. |
| `-Wthread-safety` | Clang only; enforces lock annotations. Pairs with lesson 31. |
| `-Wframe-larger-than=N -Wstack-usage=N -Walloca -Wvla` | Unbounded stack use on the hot path. |
| `-Wpadded` | Very noisy. Turn it on once per struct redesign, then off. Lesson 21. |

`-Wsign-conversion` deserves its own row because prices are `std::int64_t` ticks and
quantities are `std::uint32_t` (lesson 2), so every notional calculation is a
signed/unsigned boundary. Without that flag, the compiler will convert silently and the
bug appears only at a size that overflows.

`-Werror` policy: on in CI, off in developer builds. A compiler upgrade introduces new
warnings, and if `-Werror` is baked into the default build then that upgrade becomes an
outage for everyone at once. Promote individual warnings instead, so the ones that are
always bugs are always fatal:

```sh
-Werror=return-type -Werror=switch -Werror=uninitialized -Werror=implicit-fallthrough
```

### Sanitizers

| Sanitizer | Flag | CPU cost | Memory cost | Finds |
|---|---|---|---|---|
| Address | `-fsanitize=address` | ~2x | ~3x | Overflow, use-after-free, use-after-return, leaks |
| Undefined behaviour | `-fsanitize=undefined` | 1.2x to 2x | small | Signed overflow, bad shifts, misalignment, bad casts |
| Thread | `-fsanitize=thread` | 5x to 15x | 5x to 10x | Data races, lock-order inversions |
| Memory | `-fsanitize=memory` | ~3x | ~2x | Reads of uninitialised memory. Clang only. |
| Hardware address | `-fsanitize=hwaddress` | ~1.1x to 2x | ~15% | Same as ASan, AArch64 only |

Those are the ballparks the upstream sanitizer documentation quotes; your numbers will
differ. What matters is the ordering: UBSan is nearly free, ASan is affordable in CI, TSan
is a nightly job.

Address, Thread and Memory each replace the allocator and the shadow-memory scheme, so
**no two of them can be combined**. UBSan composes with any of the three. LeakSanitizer is
already inside ASan on Linux; `-fsanitize=leak` on its own can be combined with TSan.
MemorySanitizer only gives useful results if every library in the process, including the
standard library, was also built with it, which in practice means building libc++ yourself.

Useful companions: `-fno-sanitize-recover=all` turns findings into aborts so CI actually
fails, `-fno-omit-frame-pointer` makes the reports readable, and
`-fsanitize-address-use-after-scope` extends ASan to stack objects whose lifetime ended.
For a build that must stay fast, `-fsanitize-trap=undefined` emits a bare `ud2` instead of
a diagnostic call, which costs almost nothing in size or speed.

## Code generation, exceptions and arithmetic

| Flag | Effect |
|---|---|
| `-fno-plt` | Call through the GOT instead of a PLT stub, removing one indirect jump per cross-library call. |
| `-fno-semantic-interposition` | Let the compiler inline your own functions inside a shared library. GCC needs this explicitly; Clang assumes it already. |
| `-fvisibility=hidden -fvisibility-inlines-hidden` | Fewer dynamic symbols: smaller GOT, faster load, more devirtualization. Export deliberately with `__attribute__((visibility("default")))`. |
| `-falign-functions=32 -falign-loops=32` | Align to cache-line and decoder boundaries. Removes some build-to-build noise. Lesson 29. |
| `-fno-omit-frame-pointer` | Keeps the frame-pointer chain so `perf record -g` gets correct stacks for about 1% of throughput. |
| `-ffunction-sections -fdata-sections -Wl,--gc-sections` | Drop unreferenced code, shrinking the hot text. |
| `-Wl,-z,now -Wl,-z,relro` | Resolve every symbol at load, not on first call. |
| `-ftls-model=initial-exec` | Cheap `thread_local` access instead of a call to `__tls_get_addr`. Not valid for `dlopen`ed libraries. |
| `-fno-stack-protector` | Removes a load and compare per protected frame. Keep the protector everywhere except the measured hot path. |

`-fno-exceptions` and `-fno-rtti` are policy flags, discussed in lesson 18. `-fno-exceptions`
makes `throw` a compile error and shrinks `.eh_frame`; it does not make the happy path
faster, because table-driven unwinding already costs nothing when nothing throws. The win
is code size and layout. `-fno-rtti` removes `dynamic_cast` and `typeid` on polymorphic
types. Both must be set identically for every translation unit and every library in the
program, or you have an ODR violation (lesson 1).

:::warn
`-Ofast` and `-ffast-math` do not make your arithmetic faster. They change what your
arithmetic means. `-ffinite-math-only` lets the compiler fold `std::isnan(x)` to `false`,
so your NaN guard disappears. `-fassociative-math` reorders summations, so two builds
disagree on the last bits. Worst of all, on GNU/Linux a `-ffast-math` build links
`crtfastmath.o`, which sets flush-to-zero and denormals-are-zero in the MXCSR register at
startup, changing floating-point behaviour for the **entire process**, including libraries
that never asked for it. If you ship a shared library built this way, you have altered
your customer's arithmetic.
:::

The safe subsets are worth knowing. `-fno-math-errno` lets `sqrt` compile to a single
instruction instead of a call guarded by an `errno` check, and is safe unless you actually
read `errno` after a maths function. `-ffp-contract` controls fused multiply-add
formation: GCC defaults to `fast` (contract anywhere), Clang defaults to `on` (contract
within one statement), and `off` gives bit-reproducible results across compilers. Clang
also offers `-ffp-model=precise|strict|fast` as a single dial. On a trading hot path most
of this is moot, because prices are integer ticks and not doubles at all (lesson 40).

## Instrumentation for profiling and tracing

These change what the binary can tell you about itself. Lesson 28a covers how to use the
output; this is the flag lookup.

| Flag | Compiler | Effect |
|---|---|---|
| `-g` | both | Debug info. No run-time cost. There is no good reason to ship without it. |
| `-fno-omit-frame-pointer` | both | Keeps a walkable stack. Costs a register and a little speed, and it is what makes production profiling possible. |
| `-fxray-instrument` | Clang | Patchable nop sleds at function entry and exit, inert until turned on at run time. |
| `-fxray-instruction-threshold=N` | Clang | Only instrument functions of at least N instructions. Without it you instrument every accessor. |
| `-finstrument-functions` | both | Calls `__cyg_profile_func_enter`/`_exit` around every function. Always on and far heavier than XRay; what `uftrace` builds on. |
| `-finstrument-functions-exclude-file-list=` | both | Narrows the above, which it badly needs. |
| `-pg` | both | gprof instrumentation. Largely superseded by sampling profilers. |
| `-fprofile-generate` / `-fprofile-use` | both | PGO instrumentation and consumption. Lessons 29 and 29a. |
| `-fprofile-partial-training` | GCC | Do not pessimise code the profile never exercised. |
| `-fsanitize-coverage=trace-pc-guard` | both | Coverage callbacks, the basis of fuzzing instrumentation. |

```sh Clang XRay: build once, decide later whether to record
$ clang++ -std=c++23 -O2 -g -fxray-instrument -fxray-instruction-threshold=64       main.cpp -o trader
$ XRAY_OPTIONS="patch_premain=true xray_mode=xray-basic" ./trader
$ llvm-xray account xray-log.trader.* --sort=sum --top=20 --format=text
```

:::warn
`-finstrument-functions` and `-pg` are unconditional: every call pays, in every build,
including the one you benchmark. XRay's sleds are `nop`s until patched, which is the whole
reason it is deployable and they are not. Do not reach for the old flags out of habit.
:::

## Asking the compiler what it did

Never assume a loop vectorised or a function inlined. Ask.

```sh GCC: what got vectorised, what did not get inlined
$ g++ -std=c++23 -O3 -march=native -fopt-info-vec book.cpp
$ g++ -std=c++23 -O3 -fopt-info-vec-missed book.cpp 2>&1 | grep book.cpp
$ g++ -std=c++23 -O2 -fopt-info-inline-missed book.cpp
$ g++ -std=c++23 -O2 -Q --help=optimizers | grep enabled | head
```

```sh Clang: the same questions
$ clang++ -std=c++23 -O3 -march=native -Rpass=loop-vectorize book.cpp
$ clang++ -std=c++23 -O3 -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize book.cpp
$ clang++ -std=c++23 -O2 -Rpass=inline book.cpp
$ clang++ -std=c++23 -O2 -fsave-optimization-record book.cpp   # book.opt.yaml
```

```sh Reading the generated code, either compiler
$ g++ -std=c++23 -O2 -S -masm=intel -fverbose-asm -o - book.cpp | c++filt
$ objdump -d --no-show-raw-insn -M intel ./trader | c++filt
$ g++ -### -std=c++23 -O2 book.cpp        # the real driver invocation
```

`-fverbose-asm` annotates each instruction with the variable it came from, which turns an
unreadable listing into something you can follow next to the source. Compiler Explorer
does the same thing with colours and is usually faster to reach for (lesson 1).

## Three command lines and the traps

```sh Production
$ g++ -std=c++23 -O3 -march=icelake-server -mtune=icelake-server \
      -flto=auto -fno-plt -fno-semantic-interposition \
      -fvisibility=hidden -fvisibility-inlines-hidden \
      -falign-functions=32 -fno-omit-frame-pointer \
      -ffunction-sections -fdata-sections \
      -DNDEBUG -g -ggdb3 \
      -Wl,--gc-sections -Wl,-z,now -Wl,-z,relro \
      feed.cpp book.cpp strategy.cpp -o trader
```

```sh Debug
$ g++ -std=c++23 -Og -g3 -march=native \
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
      -Wold-style-cast -Wnon-virtual-dtor -Wcast-align -Wdouble-promotion \
      -Wnull-dereference -Wimplicit-fallthrough -Wformat=2 \
      -D_GLIBCXX_ASSERTIONS -fno-omit-frame-pointer \
      feed.cpp book.cpp tests.cpp -o trader_dbg
```

```sh CI, two passes because ASan and TSan cannot coexist
$ g++ -std=c++23 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Werror \
      -fsanitize=address,undefined -fno-sanitize-recover=all \
      -fsanitize-address-use-after-scope \
      feed.cpp book.cpp tests.cpp -o trader_asan

$ g++ -std=c++23 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Werror \
      -fsanitize=thread -fno-sanitize-recover=all \
      feed.cpp book.cpp tests.cpp -o trader_tsan

$ ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1 \
  UBSAN_OPTIONS=print_stacktrace=1 ./trader_asan
$ TSAN_OPTIONS=second_deadlock_stack=1 ./trader_tsan
```

`-D_GLIBCXX_ASSERTIONS` adds cheap bounds and precondition checks to libstdc++ containers
and does not change the ABI. Its heavier sibling `-D_GLIBCXX_DEBUG` does change container
layout, so every translation unit and every library must agree or you get memory
corruption that looks like a compiler bug.

Flags that look tempting and are traps:

- `-Ofast`, for the reasons above. It is not an optimisation level, it is a semantics change.
- `-march=native` in a build that ships. Correct on your desk, SIGILL in the cage.
- `-O3` adopted without a measurement. Frequently a tail-latency regression via icache pressure.
- `-funroll-loops` applied globally. GCC deliberately leaves it out of `-O3` and turns it on only under PGO, where it knows which loops are hot.
- `-fomit-frame-pointer` in production. It buys one register and costs you every future profile.
- `-fno-asynchronous-unwind-tables`. A small size win that breaks backtraces and `perf`'s DWARF unwinding.
- `-DNDEBUG` in the test build. Your assertions are still in the source and no longer in the binary.
- `-flto` at compile time only, or with plain `ar` instead of `gcc-ar`. Silent loss of the optimisation, or a link failure hours later.
- `-mavx512f` without `-mprefer-vector-width=256` on older server parts, where wide vectors drop the core's clock (lesson 25).

:::exercise
Run `gcc -march=native -Q --help=target | grep -v '\[disabled\]'` on your development
machine and on a production host, and diff the two outputs. Every line that differs is an
instruction the compiler would have emitted on one and not the other. Then pick the
narrowest `-march=x86-64-vN` or explicit target that covers both and put it in the build
file.
:::

## Takeaways

- `-O2 -march=<explicit target> -flto -g -fno-omit-frame-pointer` is the default posture; every deviation needs a measurement.
- `-march=native` belongs in experiments, never in a shipped binary; resolve it with `-Q --help=target` and pin the answer.
- Warnings are free bug detection: enable the full list, promote the always-fatal ones with `-Werror=`, and keep blanket `-Werror` in CI only.
- ASan, TSan and MSan are mutually exclusive; UBSan composes with all of them and is cheap enough to leave on.
- `-Ofast` and `-ffast-math` change the meaning of your arithmetic and can alter denormal handling for the whole process.
- Ask the compiler what it did with `-fopt-info` or `-Rpass` instead of assuming; the answer is often not what the source suggests.
