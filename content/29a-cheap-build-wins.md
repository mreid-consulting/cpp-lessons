---
title: Cheap Wins from the Build
part: Part IV - Latency Engineering
summary: The ranked list of things you can change in the build without touching a line of source, what each is typically worth, and how to prove it was the flag and not the weather.
time: 35 min
level: advanced
tags: lto, allocator, visibility, linker, flags
---

Before anyone rewrites a data structure, there is a pile of speed sitting in the build
system. None of it changes your design, none of it adds a new bug surface beyond a
rebuild, and one engineer can work through the whole list in an afternoon. The reason to
exhaust it first is not laziness. A refactor of a hot data structure costs weeks of review
and carries a real chance of putting a correctness bug into a system that moves money, and
it is a bad week when you discover that the 4% you fought for was available from
`-fno-semantic-interposition` all along.

This lesson ranks those changes by payoff divided by effort and risk. Lesson 29 already
covers profile-guided optimisation, BOLT, text-section layout and instruction cache
pressure, so those appear here as ranked entries plus the parts lesson 29 leaves out.
Lesson 42 is the flag lookup table. This is the decision order. Everything assumes x86-64
Linux, with GCC and Clang differences called out.

## Tier 1: level, target, and link-time optimisation

### The optimisation level is a measurement, not a default

`-O2` is the baseline you justify deviations from. `-O3` adds an aggressive vectoriser,
a much higher inlining budget and loop unrolling, and it genuinely wins on code with
countable loops over contiguous arrays: a book snapshot walk, a checksum, a fixed-point
matrix update. It genuinely loses on dispatch-heavy code, because the extra inlining and
unrolling inflate the instruction footprint and push the hot path out of L1i, which is the
trade lesson 29 takes apart in detail.

The answer is per binary, and sometimes per binary per release. Build both and compare the
tail, not the mean.

```sh Both levels, same workload, judged on the tail
$ g++ -std=c++23 -O2 -march=icelake-server $SRC -o trader.o2
$ g++ -std=c++23 -O3 -march=icelake-server $SRC -o trader.o3
$ for b in o2 o3; do taskset -c 4 ./trader.$b --replay=cap.pcap --hist=$b.hdr; done

# Did the footprint move? This is usually why -O3 lost.
$ size --format=sysv trader.o2 trader.o3 | grep -E '^\.text|^trader'
$ perf stat -e L1-icache-load-misses -- ./trader.o3 --replay=cap.pcap
```

### Pin the architecture to the machine that will run it

`-march` decides which instructions may be emitted; `-mtune` decides which
microarchitecture the scheduler optimises for and adds no new instructions. Pin both to
the deployment target explicitly. `-march=native` resolves against the *build* host, so a
build box one generation ahead of the cage produces a binary that dies with SIGILL on the
first message that reaches an AVX-512 path, which is not at startup and not in staging.
Lesson 42 has the full table of targets and the discovery commands; the habit to build is
running `gcc -march=native -Q --help=target` on a production host, not a dev box, and
writing the resolved name into the build file.

### Full LTO versus ThinLTO

Lesson 1 introduced link-time optimisation as the thing that restores cross-translation-unit
inlining. What neither lesson 1 nor lesson 29 covers is that there are two of them with very
different costs.

**Full LTO** (`-flto=full` in Clang) merges the intermediate representation of every
translation unit into one enormous module and optimises it as a whole. It gives the
optimiser the best possible view and it is the slowest thing in your build: the merge is
serial, the peak memory is proportional to the whole program rather than to one file, and
on a multi-million-line C++ codebase link steps of ten to thirty minutes with tens of
gigabytes of resident memory are ordinary. It also destroys incremental builds, because
every link redoes all of it.

**ThinLTO** (`-flto=thin`) keeps one module per translation unit and builds a compact
summary index of which symbols are defined and which call sites matter. The backend then
imports only the functions each module actually needs, in parallel across cores, with a
disk cache keyed on the summary. Published comparisons on large C++ binaries put ThinLTO
within a few percent of full LTO's run-time benefit at link times close to a plain
non-LTO link. That is why ThinLTO is the right default and full LTO is the thing you try
once, measure, and usually decline.

GCC's spelling is different: `-flto=auto` or `-flto=N` already partitions the program and
runs the backend in parallel, so GCC has no separate thin mode to opt into. Both compilers
require the LTO flag at link time as well as compile time, and archives need `gcc-ar` or
`llvm-ar` so the IR survives.

```sh ThinLTO, and asking it what it did
$ clang++ -std=c++23 -O2 -flto=thin -c feed.cpp book.cpp strategy.cpp
$ clang++ -std=c++23 -O2 -flto=thin -fuse-ld=lld \
      -Wl,--thinlto-cache-dir=/var/tmp/thinlto-cache \
      -Wl,--thinlto-cache-policy=cache_size_bytes=10g \
      feed.o book.o strategy.o -o trader

# Which cross-module calls actually got inlined?
$ clang++ -std=c++23 -O2 -flto=thin -Rpass=inline -c strategy.cpp 2>&1 | head
$ clang++ -std=c++23 -O2 -flto=thin -fsave-optimization-record -c strategy.cpp

# GCC's equivalents.
$ g++ -std=c++23 -O2 -flto=auto -flto-report -fdump-ipa-inline-details \
      feed.cpp book.cpp strategy.cpp -o trader
```

One operational warning. LTO changes which symbols survive, so a function you were
watching in `perf` can vanish into a caller or reappear under a `.constprop` suffix, and a
profile taken before the switch is not comparable to one taken after.

## Tier 1: swap the allocator

This is the single most underrated item on the list, and it takes one environment variable
to test.

glibc's `malloc` is a reasonable general-purpose allocator with per-thread arenas bolted
on. tcmalloc, jemalloc and mimalloc are built around per-thread caches and size-class free
lists from the start, and they differ from glibc mostly in two places: under multithreaded
allocation churn, where glibc's arena contention shows up, and in the tail, where the
distribution of `malloc` latency matters more than its average. On a single-threaded
allocation microbenchmark the four are often within single-digit percent. On a
sixteen-thread service that allocates and frees continuously, p99 allocation latency
differences of tens of percent are routine, and the mean barely moves.

Adoption costs nothing in source. Either link the allocator, or preload it and change no
binary at all.

```sh Three steps: A/B for free, then link it, then tune it
# 1. Zero-rebuild comparison. Same binary, different malloc.
$ LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 ./trader --replay=cap.pcap
$ LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2         ./trader --replay=cap.pcap
$ LD_PRELOAD=/usr/local/lib/libmimalloc.so                      ./trader --replay=cap.pcap

# 2. Ship it. The library goes after your objects on the link line.
$ g++ -std=c++23 -O2 feed.cpp book.cpp -o trader -ltcmalloc_minimal

# 3. Tune the one you picked, then re-measure. Holding freed pages costs RSS
#    and saves the soft page faults you pay to fault them back in.
$ MALLOC_CONF=background_thread:true,dirty_decay_ms:30000,muzzy_decay_ms:30000 ./trader
$ GLIBC_TUNABLES=glibc.malloc.tcache_count=128:glibc.malloc.arena_max=16 ./trader
```

Now the honest caveat for this audience. Lesson 24 is emphatic that the hot path should
not be allocating at all, and if you have followed it, your tick-to-trade path never calls
`malloc`. So swapping the allocator does not speed up the path you care about. It speeds
up everything around it, and that still matters.

:::hft
A logging thread, a risk snapshot writer or a Python-driven config reload that stalls for
two milliseconds inside the allocator is not a background problem. While it runs, it
touches memory, and the lines it pulls in evict yours from the shared L3 that lesson 19
described. If it is not pinned away from your hot core, it also takes the core, and your
next tick arrives to find its thread descheduled. The allocator is how you make the
uninteresting threads cheap enough to stop interfering with the interesting one. Judge
the swap on your hot path's p99.9 during a busy period, not on the allocating thread's own
throughput.
:::

## Tier 1: the shared library tax

Most C++ programmers have never thought about what happens when their code is inside a
`.so`, and it is one of the largest free wins here.

When you build a shared library, every function with default visibility gets an entry in
the dynamic symbol table. A call to such a function, *even from inside the same shared
object*, goes through the Procedure Linkage Table: an indirect jump through a slot the
dynamic loader fills in. The loader resolves it by searching the global symbol scope in
load order, so the executable, or an `LD_PRELOAD`ed library, can define the same symbol and
win. That is **semantic interposition**, and it is a feature, which is exactly the problem.
Because the definition in your own file might not be the one that runs, the compiler may
not inline it, may not propagate constants through it, and may not assume calling it twice
gives the same answer.

:::key
Inside a shared library, a default-visibility function is an indirect call the compiler is
forbidden from reasoning about. Hiding it turns the same call into a direct, inlinable one.
The source does not change; only who is allowed to replace the symbol does.
:::

The fix is to export deliberately. Default everything to hidden, then mark the handful of
symbols that really are your contract.

```cpp mid.hpp - two visibilities in one header
#pragma once
#include <cstdint>

#define TRADING_API __attribute__((visibility("default")))

// Exported: callers outside the .so bind to this at load time, so it keeps a
// dynamic symbol table entry and stays interposable.
TRADING_API std::int64_t mid_price(std::int64_t bid_ticks,
                                   std::int64_t ask_ticks) noexcept;

// Not exported. Under -fvisibility=hidden this symbol never reaches the dynamic
// symbol table, so every call to it inside the library is direct and inlinable
// regardless of what any other library defines.
std::int64_t spread_ticks(std::int64_t bid_ticks,
                          std::int64_t ask_ticks) noexcept;

inline bool is_crossed(std::int64_t bid_ticks, std::int64_t ask_ticks) noexcept {
    return bid_ticks >= ask_ticks;
}
```

`-fvisibility=hidden` sets the default; `-fvisibility-inlines-hidden` additionally hides
inline member functions, which on a template-heavy C++ library is where most of the symbol
count lives. `-fno-semantic-interposition` tells GCC it may assume your own definitions win
even for exported symbols, which restores inlining without changing the export list. Clang
already behaves this way by default, so this flag is a GCC necessity and a Clang no-op.
`-fno-plt` makes cross-library calls load the target address from the GOT and call it
directly, removing the PLT stub jump; pair it with `-Wl,-z,now` so everything is resolved
at load time anyway.

```sh Watch the indirection disappear
$ g++ -std=c++23 -O2 -fPIC -shared mid.cpp book.cpp -o libtrading.so
$ objdump -d --no-show-raw-insn libtrading.so | grep -c '@plt'
$ readelf --dyn-syms -W libtrading.so | wc -l

$ g++ -std=c++23 -O2 -fPIC -shared \
      -fvisibility=hidden -fvisibility-inlines-hidden \
      -fno-semantic-interposition -fno-plt \
      mid.cpp book.cpp -o libtrading.so
$ objdump -d --no-show-raw-insn libtrading.so | grep -c '@plt'
$ readelf --dyn-syms -W libtrading.so | wc -l
```

For a call-heavy C++ library the dynamic symbol count typically drops by an order of
magnitude, load time drops with it, and run time improves by low single-digit percent. The
upper bound published for this class of change is CPython's shared-library interpreter
build, which measured roughly 1.25x to 1.3x from `-fno-semantic-interposition` alone. That
is a dispatch-heavy workload with tiny functions called constantly across the boundary, so
treat it as the ceiling rather than the expectation. Static linking, in tier 2, is the
blunt version of all of this.

## Tier 2: worth a day

**Profile-guided optimisation.** Lesson 29 has the workflow, the AutoFDO variant and the
representativeness argument. Two things it leaves out. First, GCC's
`-fprofile-partial-training`: without it, any function the profile never saw is treated as
cold and optimised for size, so a code path your replay did not exercise gets pessimised
rather than left alone. In a trading system the paths the replay misses are frequently the
exception paths that run on the worst day of the year, so turn it on. Second, a profile is
an artefact with a shelf life. When the strategy changes, the branch distribution that the
compiler baked into last quarter's layout is simply wrong, and a stale profile is worse
than none. Either regenerate the profile as a mandatory step of every release build, or
move to sampled AutoFDO from production so it refreshes itself.

**BOLT and Propeller.** Both do post-compilation layout from a profile; lesson 29 covers
BOLT. Choosing between them comes down to your toolchain. BOLT rewrites an already-linked
binary, works with GCC or Clang output, needs `-Wl,--emit-relocs`, and holds the whole
binary in memory, which becomes awkward on the very largest executables. Propeller splits
the work: Clang emits basic-block sections, the profile drives a symbol ordering, and lld
does the relayout at link time, so it is incremental and distributable but requires an
all-LLVM build. If you are on GCC or want layout applied to a binary you already ship, BOLT.
If you are already all-Clang and your link is distributed, Propeller.

**The linker.** mold and lld are dramatically faster than GNU BFD ld, commonly 2x to 5x for
lld and another 2x to 4x for mold on large links with many cores. Be precise about what that
buys you, because engineers confuse the two constantly: **this is build time, not run time.**
The linker does not change your instructions, so the binary it produces runs the same speed.
The exception is identical code folding. `--icf=all` merges functions whose machine code is
byte-identical, which on a template-heavy C++ codebase is a large number of instantiations,
shrinking `.text` and therefore instruction cache and iTLB pressure. The correctness caveat
is real: C++ guarantees distinct functions have distinct addresses, and folding breaks that,
so any code comparing function pointers or using them as map keys misbehaves. `--icf=safe`
uses the address-significance table from `-faddrsig` to fold only functions whose address is
never taken.

```sh Link time and run time are different measurements
$ /usr/bin/time -f '%e s  %M KB' g++ $OBJS -fuse-ld=bfd  -o trader
$ /usr/bin/time -f '%e s  %M KB' g++ $OBJS -fuse-ld=lld  -o trader
$ /usr/bin/time -f '%e s  %M KB' g++ $OBJS -fuse-ld=mold -o trader

# The one linker option that changes run time.
$ clang++ -std=c++23 -O2 -faddrsig -fuse-ld=lld -Wl,--icf=safe $OBJS -o trader
$ clang++ -std=c++23 -O2 -fuse-ld=lld -Wl,--icf=all \
      -Wl,--print-icf-sections $OBJS -o trader
$ size --format=sysv trader | head -3
```

**Huge pages for the heap.** Lesson 29 backed the *text* segment with 2 MB pages to collapse
iTLB coverage. The data side is a separate decision with a separate counter. A process with
a multi-gigabyte order book or a large preallocated arena covers far more memory than the
data TLB's few thousand 4 KB entries can reach, and every miss is a page walk. Huge pages
for the heap fix that, and the allocator is usually where you configure them.

```sh Huge pages for the heap; the text segment is lesson 29
$ cat /sys/kernel/mm/transparent_hugepage/enabled     # [always] madvise never

$ GLIBC_TUNABLES=glibc.malloc.hugetlb=1 ./trader --replay=cap.pcap
$ MALLOC_CONF=thp:always ./trader --replay=cap.pcap

$ perf stat -e dTLB-load-misses,dTLB-store-misses,page-faults \
      -- ./trader --replay=cap.pcap
```

Set the system policy to `madvise` rather than `always`, so you opt in where you want huge
pages instead of inviting the kernel to compact memory synchronously under you. Lesson 30
covers that machinery.

**Static linking.** Linking statically removes the dynamic loader's relocation processing,
the symbol resolution at startup and the PLT indirection entirely, and it is the crude way
to get everything the visibility section described. What it costs is binary size, the
ability to patch a vulnerable library by updating the library, and, with glibc, correct
behaviour from anything that uses `dlopen` or NSS. The middle ground most desks settle on
is `-static-libstdc++ -static-libgcc` plus dynamic glibc, which pins the ABI that actually
breaks across hosts without inheriting the rest.

## Tier 3: measure or regret it

### The floating point flags, taken apart

"Just add `-ffast-math`" is bad advice because it is four or five separate decisions wearing
one name. Take them individually.

- `-fno-math-errno` lets `sqrt` compile to a single `sqrtsd` instruction instead of a call
  sequence that sets `errno` on a domain error. Nearly always safe, because essentially
  nobody reads `errno` after a maths function. Neither compiler enables it by default on
  GNU targets.
- `-fno-trapping-math` says you have not installed floating-point trap handlers and the
  compiler may move arithmetic across branches. Usually fine.
- `-fassociative-math` lets the compiler reassociate sums. This is what unlocks reduction
  vectorisation, which is the real speed on an array sum, and it changes your results in
  the last bits. It is a decision to be taken with whoever owns the numbers, not a default.
- `-ffinite-math-only` tells the compiler no operand is ever NaN or infinity, so
  `std::isnan(x)` folds to `false` and your guard disappears. In a pricing system that is a
  correctness hazard, not a performance flag.

```sh One decision per flag
$ g++ -std=c++23 -O2 -fno-math-errno pricing.cpp -c
$ g++ -std=c++23 -O2 -fno-math-errno -fno-trapping-math pricing.cpp -c
$ g++ -std=c++23 -O2 -fno-math-errno -fno-trapping-math \
      -fno-signed-zeros -fassociative-math pricing.cpp -c     # results now differ

$ g++ -std=c++23 -Ofast -Q --help=optimizers \
      | grep -E 'finite-math|associative|reciprocal|signed-zeros'
```

:::warn
`-Ofast` turns several of these on at once and additionally links `crtfastmath.o`, which
sets flush-to-zero and denormals-are-zero for the entire process at startup, including
every library that never asked for it. It has no place in a system that prices instruments.
Run the last command above and read the list before you ever consider it.
:::

### `-fno-exceptions` and `-fno-rtti`

Lesson 18 makes the policy argument. The build-level effect is code size and layout.
Removing exceptions deletes `.eh_frame` and `.gcc_except_table`, which are frequently 5% to
15% of a C++ binary but are never fetched into the instruction cache, so the file shrinks
and the hot path does not speed up. What does help layout is the disappearance of landing
pads and cleanup code, which *are* real instructions and do sit inside your functions.
`-fno-rtti` removes `typeinfo` objects and the RTTI slot in vtables. Both flags must be
identical across every translation unit and every library, or you have an ODR violation of
the kind lesson 1 describes.

### Unity builds

Concatenating many `.cpp` files into one translation unit gives the compiler the same
cross-file view LTO does, without LTO. Build time can move either way: you re-parse far
fewer headers, but you lose parallelism and a one-line edit rebuilds the whole chunk. The
maintenance cost is the real objection. Internal-linkage names collide, anonymous namespaces
merge, `using namespace` in one file leaks into the next, and static initialisation order
changes silently. If you already have LTO, you already have most of the benefit; treat
unity builds as a build-time tool, not a latency tool.

### `-fno-omit-frame-pointer`

This is a genuine trade rather than a free win. Keeping the frame pointer costs you one
general-purpose register and a push and pop per non-leaf frame, typically under 1% to 2% on
server code and occasionally more in a register-starved inner loop. What it buys is the
ability to profile production at all: `perf record -g` gets correct stacks by walking the
frame-pointer chain, while the alternatives are DWARF unwinding, which copies stack per
sample and is far too expensive to leave on, or LBR, which is limited in depth. The
recommendation is to keep it on everywhere including production. The major distributions
re-enabled it by default for exactly this reason, and a profile you cannot take costs more
than 2%.

## The payoff table, and the traps

The bands below are an order-of-magnitude guide for one workload class: a large
multithreaded C++ trading server replaying recorded market data on x86-64 Linux. They are
not promises, and several rows can be zero on your binary.

| Change | Typical gain | Effort | Risk |
|---|---|---|---|
| `-O2` to `-O3` | -3% to +5% | minutes | Can regress the tail via icache |
| Explicit `-march`/`-mtune` for the target | 0% to 10% | an hour | SIGILL if the target is wrong |
| ThinLTO | 3% to 10% | hours | Flags must match at link; symbols move |
| Allocator swap | ~0% mean, 5% to 30% on allocating-thread p99 | an hour | Low |
| `-fvisibility=hidden -fno-semantic-interposition` | 1% to 5% | hours | May hide a symbol a plugin needed |
| PGO | 5% to 15% | a day, plus profile upkeep forever | Stale profiles pessimise |
| BOLT or Propeller on top of PGO and LTO | 5% to 8% | a day | Post-link rewrite needs validation |
| mold or lld | 0% run time, 2x to 10x link time | an hour | Low |
| `--icf=all` | 0% to 2%, via smaller text | an hour | Breaks function pointer identity |
| Huge pages for the heap | 0% to 5% on dTLB-bound workloads | hours | THP compaction stalls |
| Static linking | Startup only, ~0% steady state | a day | Patching, `dlopen`, NSS |
| `-fno-math-errno` | 0% unless `sqrt` is on a hot path | minutes | Very low |
| `-fno-exceptions -fno-rtti` | 0% to 2%, via code size | days | High if any dependency throws |
| `-fno-omit-frame-pointer` | -1% to -2% | minutes | Negative gain; buys production profiling |

The traps, all of which are common:

- **`-Ofast`.** Not an optimisation level. A process-wide change to what arithmetic means.
- **Assuming `-O3` beats `-O2`.** Frequently a tail regression. It is a measurement.
- **`-march=native` in CI.** Correct on the runner, SIGILL in the cage, and the failure
  appears on the first packet that reaches the wide path rather than at startup.
- **Benchmarking a build that differs from production in any flag.** A `-O2 -g` benchmark
  binary and a `-O3 -flto` production binary are different programs, and the conclusion you
  draw from one does not transfer.
- **A PGO profile from unrepresentative data.** A synthetic loop teaches the compiler that
  your instrument-lookup branch is always taken. Lesson 29 is blunt about this.

## Proving it helped

The verification discipline is the whole lesson, because everything above is cheap enough
that the temptation is to turn on all of it at once and declare victory.

Change one flag at a time. Build both binaries from the same source at the same commit.
Run the same workload on the same host with the same isolation, pinning and governor
settings, since lesson 30's OS effects dwarf most of these flags. Compare percentiles, not
means: a change that improves the median by 2% and the p99.9 by nothing has not helped the
thing you are paid to improve, and lesson 26 explains why the mean is the wrong summary of
a latency distribution. Then confirm the mechanism. If you believe hiding symbols removed
indirect calls, the indirect-call count should fall. If you believe `--icf=all` reduced
instruction cache pressure, `L1-icache-load-misses` should fall. A change whose expected
counter did not move, but whose wall clock improved, is alignment noise wearing a costume,
and lesson 28 has the counters to check.

```sh One flag, two builds, the same everything else
$ g++ -std=c++23 -O2 -march=icelake-server -flto=auto $SRC -o trader.base
$ g++ -std=c++23 -O2 -march=icelake-server -flto=auto \
      -fvisibility=hidden -fno-semantic-interposition $SRC -o trader.test

$ for b in base test; do
>   taskset -c 4 chrt -f 80 ./trader.$b --replay=cap.pcap --hist=$b.hdr
> done

$ perf stat -r 5 -e cycles,instructions,branch-misses,\
L1-icache-load-misses,iTLB-load-misses \
      -- taskset -c 4 ./trader.test --replay=cap.pcap
```

:::exercise
Take the largest multi-file program you have from an earlier lesson and build it five
ways, changing exactly one thing each time: plain `-O2`; `-O2 -flto=thin`; the same plus
`-fvisibility=hidden -fvisibility-inlines-hidden -fno-semantic-interposition`; the same
plus `-Wl,--icf=safe`; and the `-O2` build run under `LD_PRELOAD` of jemalloc. For each,
record `.text` size from `size --format=sysv`, the dynamic symbol count from
`readelf --dyn-syms`, and the p99 of your replay. Write down your predicted ranking before
you run anything, then find the one you got wrong and explain why.
:::

## Takeaways

- Build-level changes are the cheapest latency available: no design change, no new bug
  surface, an afternoon of work. Exhaust them before refactoring a data structure.
- Tier 1 is a measured `-O` level, an explicitly pinned `-march`, ThinLTO rather than full
  LTO, an allocator swap tested by `LD_PRELOAD`, and hiding your shared library's symbols.
- Inside a shared object, default visibility means every call is interposable and therefore
  not inlinable. `-fvisibility=hidden` with `-fno-semantic-interposition` is the fix, and
  most C++ programmers have never applied it.
- mold and lld buy build time, not run time. The one linker option that changes run time is
  identical code folding, and it breaks function pointer identity.
- `-Ofast` and `-ffinite-math-only` change what your arithmetic means and do not belong in a
  system that prices instruments; `-fno-math-errno` is the safe part of that family.
- Verify by changing one flag, comparing percentiles rather than means, and confirming with
  a hardware counter that the mechanism you predicted is the one that moved.
