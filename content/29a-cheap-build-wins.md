---
title: Cheap Wins from the Build
part: Part IV - Latency Engineering
summary: The ranked list of things you can change in the build without touching a line of source, what each is typically worth, and how to prove it was the flag and not the weather.
time: 35 min
level: advanced
tags: lto, allocator, visibility, linker, flags
---

Before anyone rewrites a data structure, there is a pile of speed sitting in the build
system. None of it changes your design, none of it adds a new bug surface beyond a rebuild,
and one engineer can work through the list in an afternoon. That is why it goes first: a
refactor of a hot data structure costs weeks of review and can put a correctness bug into a
system that moves money, and it is a bad week when you discover the 4% you fought for was
available from a flag.

This lesson ranks those changes by payoff divided by effort and risk. Lesson 29 covers
profile-guided optimisation, BOLT and text layout; lesson 42 is the flag lookup table. This
is the decision order. Everything assumes x86-64 Linux, with GCC and Clang differences
called out.

## Tier 1: level, target, and link-time optimisation

`-O2` is the baseline you justify deviations from. `-O3` adds an aggressive vectoriser, a
higher inlining budget and loop unrolling. It wins on countable loops over contiguous
arrays: a snapshot walk, a checksum, a fixed-point update. It loses on dispatch-heavy code,
where the extra inlining and unrolling inflate the instruction footprint and push the hot
path out of L1i, the trade lesson 29 takes apart. The answer is per binary, sometimes per
release. Build both and compare the tail.

```sh Both levels, same workload, judged on the tail
$ g++ -std=c++23 -O2 -march=icelake-server $SRC -o trader.o2
$ g++ -std=c++23 -O3 -march=icelake-server $SRC -o trader.o3
$ for b in o2 o3; do taskset -c 4 ./trader.$b --replay=cap.pcap --hist=$b.hdr; done

# Did the footprint move? That is usually why -O3 lost.
$ size --format=sysv trader.o2 trader.o3 | grep -E '^\.text'
$ perf stat -e L1-icache-load-misses -- ./trader.o3 --replay=cap.pcap
```

`-march` decides which instructions may be emitted; `-mtune` decides which
microarchitecture the scheduler favours and adds none. Pin both to the deployment target.
`-march=native` resolves against the *build* host, so a build box one generation ahead of
the cage yields a binary that dies with SIGILL on the first message reaching an AVX-512
path, not at startup and not in staging. Lesson 42 lists the targets. The habit worth
building is running `gcc -march=native -Q --help=target` on a production host and writing
the resolved name into the build file.

### Full LTO versus ThinLTO

Lesson 1 introduced link-time optimisation as what restores cross-translation-unit
inlining. There are two of them and their costs differ enormously.

**Full LTO** (`-flto=full` in Clang) merges every translation unit's intermediate
representation into one module. Best possible view for the optimiser, slowest thing in your
build: the merge is serial, peak memory scales with the whole program rather than one file,
and on a multi-million-line codebase link steps of ten to thirty minutes and tens of
gigabytes resident are ordinary. It also destroys incremental builds.

**ThinLTO** (`-flto=thin`) keeps one module per translation unit plus a compact summary
index, imports only the functions each module actually needs, runs the backends in parallel
and caches them on disk. Published comparisons on large C++ binaries put it within a few
percent of full LTO's run-time benefit at link times close to a non-LTO link, which is why
it is the right default and full LTO is the thing you try once and usually decline. GCC has
no separate thin mode; `-flto=auto` already partitions and parallelises. Both compilers need
the flag at link as well as compile, and archives need `gcc-ar` or `llvm-ar` so the IR
survives.

```sh ThinLTO, and asking it what it did
$ clang++ -std=c++23 -O2 -flto=thin -c feed.cpp book.cpp strategy.cpp
$ clang++ -std=c++23 -O2 -flto=thin -fuse-ld=lld \
      -Wl,--thinlto-cache-dir=/var/tmp/thinlto-cache \
      -Wl,--thinlto-cache-policy=cache_size_bytes=10g \
      feed.o book.o strategy.o -o trader

# Which cross-module calls got inlined?
$ clang++ -std=c++23 -O2 -flto=thin -Rpass=inline -c strategy.cpp 2>&1 | head
$ clang++ -std=c++23 -O2 -flto=thin -fsave-optimization-record -c strategy.cpp

$ g++ -std=c++23 -O2 -flto=auto -flto-report -fdump-ipa-inline-details \
      feed.cpp book.cpp strategy.cpp -o trader
```

LTO moves symbols: a function you watched in `perf` can vanish into its caller or reappear
with a `.constprop` suffix, so a profile from before the switch is not comparable to one
from after.

## Tier 1: swap the allocator

The most underrated item here, and it takes one environment variable to test.

glibc's `malloc` is a general-purpose allocator with per-thread arenas bolted on. tcmalloc,
jemalloc and mimalloc are built around per-thread caches and size-class free lists from the
start. They differ from glibc in two places: under multithreaded allocation churn, where
glibc's arena contention shows, and in the tail rather than the mean. On a single-threaded
allocation microbenchmark all four are often within single-digit percent. On a
sixteen-thread service allocating continuously, p99 allocation differences of tens of
percent are routine while the mean barely moves.

```sh Test it for free, then ship it, then tune it
# 1. Zero-rebuild comparison. Same binary, different malloc.
$ LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 ./trader --replay=cap.pcap
$ LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2         ./trader --replay=cap.pcap
$ LD_PRELOAD=/usr/local/lib/libmimalloc.so                      ./trader --replay=cap.pcap

# 2. Ship it. The library goes after your objects on the link line.
$ g++ -std=c++23 -O2 feed.cpp book.cpp -o trader -ltcmalloc_minimal

# 3. Tune what you picked. Holding freed pages costs RSS and saves the soft
#    page faults you would pay to fault them back in.
$ MALLOC_CONF=background_thread:true,dirty_decay_ms:30000,muzzy_decay_ms:30000 ./trader
$ GLIBC_TUNABLES=glibc.malloc.tcache_count=128:glibc.malloc.arena_max=16 ./trader
```

The honest caveat: lesson 24 is emphatic that the hot path should not allocate at all, so
if you followed it your tick-to-trade path never calls `malloc` and none of this speeds it
up. It makes everything around it cheaper, which still matters.

:::hft
A logging thread or risk snapshot writer that stalls two milliseconds inside the allocator
is not a background problem. While it runs it touches memory, and the lines it pulls in
evict yours from the shared L3 of lesson 19. If it is not pinned away from your hot core it
also takes the core, and your next tick arrives to find its thread descheduled. Judge the
allocator swap on your hot path's p99.9 during a busy period, never on the allocating
thread's own throughput.
:::

## Tier 1: the shared library tax

Most C++ programmers have never thought about what happens when their code sits inside a
`.so`, and it is one of the largest free wins on this list.

Every function with default visibility in a shared library gets a dynamic symbol table
entry. A call to one, *even from inside the same shared object*, goes through the Procedure
Linkage Table: an indirect jump through a slot the dynamic loader fills in. The loader
resolves it by searching the global symbol scope in load order, so the executable or an
`LD_PRELOAD`ed library can define the same symbol and win. That is **semantic
interposition**, and it is a feature, which is exactly the problem. Because the definition
in your own file might not be the one that runs, the compiler may not inline it, may not
propagate constants through it, and may not assume two calls agree.

:::key
Inside a shared library, a default-visibility function is an indirect call the compiler is
forbidden from reasoning about. Hiding it makes the same call direct and inlinable. The
source does not change; only who is allowed to replace the symbol does.
:::

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
// no matter what another library defines.
std::int64_t spread_ticks(std::int64_t bid_ticks,
                          std::int64_t ask_ticks) noexcept;

inline bool is_crossed(std::int64_t bid_ticks, std::int64_t ask_ticks) noexcept {
    return bid_ticks >= ask_ticks;
}
```

`-fvisibility=hidden` sets the default and `-fvisibility-inlines-hidden` also hides inline
member functions, which on a template-heavy library is where most of the symbol count
lives. `-fno-semantic-interposition` tells GCC it may assume your own definitions win even
for exported symbols, restoring inlining without changing the export list; Clang already
behaves this way, so the flag is a GCC necessity and a Clang no-op. `-fno-plt` makes
cross-library calls load the target from the GOT and call it directly, removing the stub
jump. Pair it with `-Wl,-z,now`.

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
magnitude, load time falls with it, and run time improves by low single-digit percent. The
published ceiling is CPython's shared-library interpreter, which measured roughly 1.25x to
1.3x from `-fno-semantic-interposition` alone; that is a dispatch-heavy workload of tiny
functions called constantly, so treat it as an upper bound. Static linking, below, is the
blunt version of the same idea.

## Tier 2: worth a day

**Profile-guided optimisation.** Lesson 29 has the workflow and the representativeness
argument. Two additions. GCC's `-fprofile-partial-training` stops functions the profile
never saw from being treated as cold and optimised for size; in a trading system the paths
a replay misses are often the exception paths that run on the worst day of the year, so
turn it on. And a profile has a shelf life. When the strategy changes, the branch
distribution baked into last quarter's layout is simply wrong, and a stale profile is worse
than none. Either regenerate it as a mandatory release step, or move to sampled AutoFDO
from production so it refreshes itself.

**BOLT and Propeller.** Both do post-compilation layout from a profile. BOLT rewrites an
already-linked binary, accepts GCC or Clang output, needs `-Wl,--emit-relocs`, and holds
the whole binary in memory, which gets awkward on the largest executables. Propeller splits
the work: Clang emits basic-block sections, the profile drives a symbol ordering and lld
does the relayout at link time, so it is incremental and distributable but demands an
all-LLVM build. On GCC, or to optimise a binary you already ship, use BOLT. All-Clang with
a distributed link, use Propeller.

**The linker.** mold and lld beat GNU BFD ld badly, commonly 2x to 5x for lld and another
2x to 4x for mold on large links. Be precise about what that buys, because engineers
confuse the two constantly: **this is build time, not run time.** The linker does not change
your instructions. The exception is identical code folding. `--icf=all` merges functions
whose machine code is byte-identical, which on a template-heavy codebase is a lot of
instantiations, shrinking `.text` and with it instruction cache and iTLB pressure. The
caveat is real: C++ guarantees distinct functions have distinct addresses, so folding breaks
any code comparing function pointers or keying a map on them. `--icf=safe` uses the
address-significance table from `-faddrsig` to fold only functions whose address is never
taken.

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
iTLB coverage; the data side is a separate decision with a separate counter. A process with
a multi-gigabyte book or a large preallocated arena covers far more memory than the data
TLB's few thousand 4 KB entries reach, and every miss is a page walk. Set the system policy
to `madvise` rather than `always`, so you opt in instead of inviting synchronous compaction
under you. Lesson 30 covers that machinery.

```sh Huge pages for the heap; the text segment is lesson 29
$ cat /sys/kernel/mm/transparent_hugepage/enabled     # [always] madvise never

$ GLIBC_TUNABLES=glibc.malloc.hugetlb=1 ./trader --replay=cap.pcap
$ MALLOC_CONF=thp:always ./trader --replay=cap.pcap

$ perf stat -e dTLB-load-misses,dTLB-store-misses,page-faults \
      -- ./trader --replay=cap.pcap
```

**Static linking.** It removes the dynamic loader's relocation processing, the symbol
resolution at startup and PLT indirection entirely, which is the crude way to get
everything the visibility section described. It costs binary size, the ability to patch a
vulnerable library by updating the library, and, with glibc, correct behaviour from
anything using `dlopen` or NSS. The middle ground most desks settle on is
`-static-libstdc++ -static-libgcc` with dynamic glibc, pinning the ABI that actually breaks
across hosts without inheriting the rest.

## Tier 3: measure or regret it

"Just add `-ffast-math`" is bad advice, because it is several separate decisions wearing one
name. Take them individually.

- `-fno-math-errno` lets `sqrt` compile to a single `sqrtsd` instead of a call that sets
  `errno` on a domain error. Nearly always safe; essentially nobody reads `errno` after a
  maths function. Neither compiler enables it by default on GNU targets.
- `-fno-trapping-math` says you installed no floating-point trap handlers, so arithmetic may
  move across branches. Usually fine.
- `-fassociative-math` lets the compiler reassociate sums. This is what unlocks reduction
  vectorisation, and it changes your results in the last bits. A decision to take with
  whoever owns the numbers, not a default.
- `-ffinite-math-only` asserts no operand is ever NaN or infinity, so `std::isnan(x)` folds
  to `false` and your guard silently disappears. In a pricing system that is a correctness
  hazard, not a performance flag.

```sh One decision per flag
$ g++ -std=c++23 -O2 -fno-math-errno pricing.cpp -c
$ g++ -std=c++23 -O2 -fno-math-errno -fno-trapping-math pricing.cpp -c
$ g++ -std=c++23 -O2 -fno-math-errno -fno-trapping-math \
      -fno-signed-zeros -fassociative-math pricing.cpp -c     # results now differ

$ g++ -std=c++23 -Ofast -Q --help=optimizers \
      | grep -E 'finite-math|associative|reciprocal|signed-zeros'
```

:::warn
`-Ofast` turns several of these on at once and links `crtfastmath.o`, which sets
flush-to-zero and denormals-are-zero for the whole process at startup, including libraries
that never asked for it. It has no place in a system that prices instruments. Run the last
command above and read the list before you ever consider it.
:::

**`-fno-exceptions` and `-fno-rtti`.** Lesson 18 makes the policy argument; the build-level
effect is size and layout. Removing exceptions deletes `.eh_frame` and `.gcc_except_table`,
frequently 5% to 15% of a C++ binary but never fetched into the instruction cache, so the
file shrinks and the hot path does not speed up. What does help layout is the disappearance
of landing pads and cleanup code, which are real instructions sitting inside your functions.
`-fno-rtti` removes `typeinfo` objects and the vtable RTTI slot. Both must be identical
across every translation unit and library, or you have the kind of ODR violation lesson 1
describes.

**Unity builds.** Concatenating many `.cpp` files into one translation unit gives the
compiler the cross-file view LTO does, without LTO. Build time moves either way: far fewer
header re-parses, but less parallelism and a one-line edit rebuilds the whole chunk. The
maintenance cost is the real objection, since internal-linkage names collide, anonymous
namespaces merge, `using namespace` leaks into the next file and static initialisation order
changes silently. With LTO you already have most of the benefit; treat unity builds as a
build-time tool.

**`-fno-omit-frame-pointer`.** A genuine trade rather than a win. It costs one
general-purpose register plus a push and pop per non-leaf frame, typically under 1% to 2% on
server code and more in a register-starved inner loop. It buys the ability to profile
production at all, because `perf record -g` then walks the frame-pointer chain instead of
falling back to DWARF unwinding, which copies stack per sample, or LBR, which is depth
limited. Keep it on everywhere including production. The major distributions re-enabled it
by default for this reason, and a profile you cannot take costs more than 2%.

## The payoff table, and the traps

These bands are an order-of-magnitude guide for one workload class, a large multithreaded
C++ trading server replaying recorded market data on x86-64 Linux. They are not promises,
and several rows will be zero on your binary.

| Change | Typical gain | Effort | Risk |
|---|---|---|---|
| `-O2` to `-O3` | -3% to +5% | minutes | Can regress the tail via icache |
| Explicit `-march`/`-mtune` | 0% to 10% | an hour | SIGILL if the target is wrong |
| ThinLTO | 3% to 10% | hours | Flags must match at link; symbols move |
| Allocator swap | ~0% mean, 5% to 30% on allocating-thread p99 | an hour | Low |
| `-fvisibility=hidden -fno-semantic-interposition` | 1% to 5% | hours | May hide a symbol a plugin needed |
| PGO | 5% to 15% | a day, plus profile upkeep forever | Stale profiles pessimise |
| BOLT or Propeller atop PGO and LTO | 5% to 8% | a day | Post-link rewrite needs validation |
| mold or lld | 0% run time, 2x to 10x link time | an hour | Low |
| `--icf=all` | 0% to 2%, via smaller text | an hour | Breaks function pointer identity |
| Huge pages for the heap | 0% to 5% when dTLB-bound | hours | THP compaction stalls |
| Static linking | Startup only, ~0% steady state | a day | Patching, `dlopen`, NSS |
| `-fno-math-errno` | 0% unless `sqrt` is on a hot path | minutes | Very low |
| `-fno-exceptions -fno-rtti` | 0% to 2%, via code size | days | High if a dependency throws |
| `-fno-omit-frame-pointer` | -1% to -2% | minutes | Negative gain; buys profiling |

The traps, all of them common:

- **`-Ofast`.** Not an optimisation level. A process-wide change to what arithmetic means.
- **Assuming `-O3` beats `-O2`.** Frequently a tail regression. It is a measurement.
- **`-march=native` in CI.** Correct on the runner, SIGILL in the cage, and it fails on the
  first packet reaching the wide path rather than at startup.
- **Benchmarking a build that differs from production in any flag.** A `-O2 -g` benchmark
  binary and a `-O3 -flto` production binary are different programs.
- **A PGO profile from unrepresentative data.** A synthetic loop teaches the compiler your
  instrument-lookup branch is always taken. Lesson 29 is blunt about this.

## Proving it helped

Everything above is cheap enough that the temptation is to enable all of it at once and
declare victory. Change one flag at a time, build both binaries from the same commit, and
run the same workload on the same host with the same isolation, pinning and governor
settings, since lesson 30's OS effects dwarf most of these flags. Compare percentiles, not
means: a change that moves the median 2% and the p99.9 not at all has not helped what you
are paid to improve, and lesson 26 explains why the mean is the wrong summary of a latency
distribution. Then confirm the mechanism with counters. A change whose expected counter did
not move but whose wall clock improved is alignment noise wearing a costume, and lesson 28
has the counters to check.

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
Build the largest multi-file program from an earlier lesson five ways, changing exactly one
thing each time: plain `-O2`; `-O2 -flto=thin`; that plus `-fvisibility=hidden
-fvisibility-inlines-hidden -fno-semantic-interposition`; that plus `-Wl,--icf=safe`; and
the `-O2` build run under `LD_PRELOAD` of jemalloc. For each, record `.text` size from
`size --format=sysv`, the dynamic symbol count from `readelf --dyn-syms`, and the p99 of
your replay. Write your predicted ranking down before you run anything, then find the one
you got wrong and explain why.
:::

## Takeaways

- Build-level changes are the cheapest latency available: no design change, no new bug
  surface, an afternoon of work. Exhaust them before refactoring a data structure.
- Tier 1 is a measured `-O` level, an explicitly pinned `-march`, ThinLTO rather than full
  LTO, an allocator swap tested by `LD_PRELOAD`, and hiding your library's symbols.
- Inside a shared object, default visibility makes every call interposable and therefore not
  inlinable. `-fvisibility=hidden` with `-fno-semantic-interposition` is the fix.
- mold and lld buy build time, not run time. The one linker option that changes run time is
  identical code folding, which breaks function pointer identity.
- `-Ofast` and `-ffinite-math-only` change what your arithmetic means and have no place in a
  system that prices instruments; `-fno-math-errno` is the safe part of that family.
- Verify by changing one flag, comparing percentiles rather than means, and confirming with
  a hardware counter that the mechanism you predicted is the one that moved.
