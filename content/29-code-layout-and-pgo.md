---
title: Code Layout, iCache, PGO and BOLT
part: Part IV - Latency Engineering
summary: Instructions live in the same memory hierarchy as data. Hot/cold splitting, text ordering, profile-guided optimisation and post-link layout, measured with iTLB and L1i counters rather than by feel.
time: 30 min
level: expert
tags: icache, itlb, pgo, bolt, autofdo, layout
---

Every lesson in Part III treated the memory hierarchy as something data lives in. It is
also where your instructions live. A core fetches its own code through an L1 instruction
cache and an instruction TLB, and if your hot path is scattered across forty pages of a
sixty-megabyte binary, you pay a fetch stall before you execute a single useful
instruction. This lesson is about arranging code so that the first tick after a quiet
period is not the slowest one.

Everything here is Linux and GCC/Clang specific.

## The instruction side of the hierarchy

A typical modern x86-64 server core has a **32 KB L1 instruction cache**, separate from
its data cache, holding 512 lines of 64 bytes. It also has a first-level **instruction
TLB** of roughly 128 entries for 4 KB pages, which covers about 512 KB of code. Ahead of
both sits a decoded-uop cache, on Intel parts typically holding a few thousand micro-ops.

Compare those to a real trading binary. A feed handler, a book, a strategy, a risk check
and an order gateway compiled with aggressive inlining routinely produce a hot path whose
distinct code footprint is a few hundred kilobytes, spread across a text segment of tens
of megabytes. Two consequences follow.

- **The hot path does not fit in L1i**, so a burst of messages after an idle period takes
  instruction-cache misses that a back-to-back benchmark never sees. This is exactly the
  effect that makes warm microbenchmarks lie in lesson 27.
- **The hot path does not fit in the iTLB.** An iTLB miss walks the page table, which is
  itself memory traffic, and unlike a data TLB miss you cannot prefetch around it.

:::key
Instruction fetch stalls appear in the top-down method of lesson 28 as **frontend bound**.
If your top-down breakdown says frontend bound is above roughly 15%, this lesson is where
your next win is. If it says 3%, skip to lesson 30. Layout work is worthless on a program
whose code already fits.
:::

## Hot, cold, and keeping the fall-through dense

The compiler lays out a function's basic blocks in some order and one of them falls
through to the next without a taken branch. What you want is for the entire common path
to be a contiguous run of fall-through blocks, with everything unusual, error handling,
logging, resubscription, exiting the market, moved far away.

C++ gives you attributes for this. `[[gnu::hot]]` and `[[gnu::cold]]` tell GCC and Clang
which functions belong in the hot and cold text sections respectively, and `cold` also
implies that calls to that function are unlikely.

```cpp order_gate.hpp
#pragma once
#include <cstdint>
#include <string_view>

struct OrderMsg {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

// Never inlined, placed in .text.unlikely, and every call site treated as cold.
[[gnu::cold, gnu::noinline]]
void reject_order(const OrderMsg& msg, std::string_view reason);

[[gnu::hot]]
bool send_order(const OrderMsg& msg) noexcept;
```

```cpp order_gate.cpp
#include "order_gate.hpp"

namespace {
constexpr std::int64_t kMaxPriceTicks = 5'000'000;
constexpr std::uint32_t kMaxQty       = 100'000;
}  // namespace

bool send_order(const OrderMsg& msg) noexcept {
    if (msg.qty == 0 || msg.qty > kMaxQty) [[unlikely]] {
        reject_order(msg, "qty");         // one call, body lives elsewhere
        return false;
    }
    if (msg.price_ticks <= 0 || msg.price_ticks > kMaxPriceTicks) [[unlikely]] {
        reject_order(msg, "price");
        return false;
    }
    // ... the common path continues here, uninterrupted by error-handling code
    return true;
}
```

The point of `noinline` on `reject_order` is not that the function is slow. It is that
inlining a 300-byte error handler into the middle of a 90-byte hot function triples the
number of cache lines the hot function occupies, and every one of those lines is fetched
whether the error occurs or not. **Error handling belongs behind a call, not in the middle
of the fall-through path.**

`-freorder-blocks-and-partition`, on by default at `-O2` in GCC, does the same thing at
basic-block granularity: it moves blocks the compiler believes are cold into
`.text.unlikely`. Without profile data the compiler's belief comes from heuristics and
your `[[likely]]`/`[[unlikely]]` annotations from lesson 22, and it is often wrong. With
profile data it is right, which is the argument for the next section.

## Ordering the text section

Even with hot and cold separated, the hot functions themselves are laid out in whatever
order the linker happened to see the object files. You can control it.

```sh
$ g++ -std=c++23 -O2 -ffunction-sections -fdata-sections \
      -Wl,--symbol-ordering-file=hot.txt feed.cpp book.cpp gate.cpp -o trader
```

`-ffunction-sections` puts each function in its own section, which is what makes
reordering possible at all. `--symbol-ordering-file` (LLD) or a linker script with an
explicit `.text` ordering (BFD ld) then places the named symbols first, adjacent, in the
order given. The file is just a list of mangled symbol names, hottest first, which you
generate from a profile:

```sh hot.txt, generated from a cycles profile
$ perf record -F 999 -e cycles:pp -- ./trader --replay=cap.pcap
$ perf report --stdio --sort=symbol --percent-limit=0.1 \
    | awk '/^ +[0-9]/ {print $NF}' > hot.txt
```

Getting the hot functions into one contiguous run of pages is most of the available win,
because it collapses the number of distinct code pages the iTLB must cover. This is also
exactly what BOLT does automatically and better, so treat manual ordering as the thing you
do when you cannot deploy a post-link tool.

## Profile-guided optimisation, end to end

PGO gives the compiler measurements instead of heuristics. The workflow is three steps and
the middle one is the one people get wrong.

```sh Step 1: build an instrumented binary
$ g++ -std=c++23 -O2 -fprofile-generate=/var/tmp/prof \
      feed.cpp book.cpp gate.cpp -o trader-instr
```

```sh Step 2: run a REPRESENTATIVE workload
$ ./trader-instr --replay=recorded_2026_09_09_open.pcap
$ ./trader-instr --replay=recorded_2026_09_09_midday.pcap
$ ./trader-instr --replay=recorded_2026_09_09_close.pcap
```

```sh Step 3: rebuild using the profile
$ g++ -std=c++23 -O2 -fprofile-use=/var/tmp/prof -fprofile-correction \
      feed.cpp book.cpp gate.cpp -o trader
```

Step 2 decides everything. Profile a synthetic benchmark that sends the same message type
a million times and you will teach the compiler that your instrument-lookup branch is
always taken, which is true of the benchmark and false of the market. **Replay recorded
market data**, and cover the open, a quiet period and the close, because the branch
distribution genuinely differs between them. If your workload has a hot path that only
runs on a fraction of a percent of messages, make sure the replay contains those messages.

What PGO actually changes:

- **Inlining decisions.** Call sites with high measured frequency get inlined even when
  they exceed the size heuristic; cold call sites stop being inlined even when they are
  small. This is the largest single effect on a C++ codebase.
- **Basic block layout.** Blocks are ordered so the measured common path falls through,
  and measured-cold blocks are partitioned into `.text.unlikely`.
- **Branch hints and register allocation.** Registers are preferentially kept for values
  live on hot paths.
- **Function ordering and hot/cold splitting** at a granularity you would never annotate
  by hand.

Reported gains vary widely by workload. For a large C++ server, published figures cluster
around **5% to 15%** on throughput, with larger effects on branch-heavy interpreted or
dispatch-heavy code and near-zero effects on numeric kernels that were already tight. On a
trading hot path the interesting effect is usually not average throughput but the tail,
because the layout improvement is worth most on the cold-start message.

:::warn
`-fprofile-use` on stale profile data is worse than no profile. If the source has changed
enough that the counters no longer match the control-flow graph, GCC warns and, with
`-Wno-coverage-mismatch`, silently uses garbage. Regenerate profiles as part of the
release build, and treat a coverage-mismatch warning as a build failure.
:::

### AutoFDO when instrumentation is too intrusive

Instrumented binaries typically run 10% to 30% slower and mutate global counters, which
makes them useless for profiling anything timing-sensitive and awkward to run in
production. **AutoFDO** takes ordinary `perf` samples from an unmodified optimised binary
and converts them into a profile the compiler can consume.

```sh
$ perf record -b -e cycles:pp -- ./trader --replay=cap.pcap      # -b enables LBR
$ create_llvm_prof --binary=./trader --out=trader.afdo
$ clang++ -std=c++23 -O2 -fprofile-sample-use=trader.afdo *.cpp -o trader
```

The profile is less precise than instrumentation, so gains are typically a fraction of
full PGO's, often quoted at around 60% to 80% of the instrumented result. In exchange you
can collect it from a production process at under 1% overhead, which means it stays
current instead of being a six-month-old artefact.

## BOLT: layout after the linker

**BOLT** (Binary Optimization and Layout Tool) rewrites an already-linked binary. Because
it works after linking it sees the final addresses of everything, including code the
compiler never had visibility into, and it can reorder basic blocks across function
boundaries, split functions, and lay out the whole text segment by measured execution
order. It composes with PGO rather than replacing it.

```sh
$ g++ -std=c++23 -O2 -Wl,--emit-relocs -fprofile-use=/var/tmp/prof *.cpp -o trader
$ perf record -e cycles:u -j any,u -- ./trader --replay=cap.pcap   # -j any,u = LBR
$ perf2bolt -p perf.data -o trader.fdata ./trader
$ llvm-bolt ./trader -o trader.bolt -data=trader.fdata \
      -reorder-blocks=ext-tsp -reorder-functions=hfsort+ \
      -split-functions -split-all-cold -dyno-stats
```

`--emit-relocs` at link time is mandatory; BOLT needs the relocations to move code safely.
Published results for large C++ services report improvements in the region of **5% to 8%
on top of an already PGO-and-LTO-optimised binary**, driven almost entirely by reductions
in instruction-cache and iTLB misses. `-dyno-stats` prints BOLT's own before/after
estimate, which is a useful sanity check but is not a measurement of your system.

### Huge pages for the text segment

Once the hot code is contiguous, you can back it with 2 MB pages so that the entire hot
path is covered by one or two iTLB entries instead of dozens. BOLT will do this for you:

```sh
$ llvm-bolt ./trader -o trader.bolt -data=trader.fdata -hugify \
      -reorder-functions=hfsort+ -reorder-blocks=ext-tsp -split-functions
```

Without BOLT, the same effect is available by aligning the text segment to 2 MB at link
time and calling `madvise(MADV_HUGEPAGE)` on it at startup, or by enabling transparent
huge pages for file-backed text where the kernel supports it. Lesson 30 covers the huge
page machinery in general.

## Alignment, unrolling, and measuring instead of guessing

Two flags are worth knowing and neither is worth applying blindly.

| Flag | Effect | When it helps |
|---|---|---|
| `-falign-functions=32` | Start each function on a 32-byte boundary | Reduces cross-boundary fetch for small hot functions; costs padding |
| `-falign-loops=32` | Align loop entry points | Helps loops whose body is close to a fetch-window size |
| `-funroll-loops` | Replicate loop bodies | Helps tiny loops with loop-carried overhead; hurts icache |

Unrolling is the clearest tradeoff in this lesson. Unrolling a four-instruction loop eight
times removes seven compares and seven branches, and multiplies the loop's code footprint
by eight. On a decoder loop that runs a handful of iterations per message, the unrolled
version can easily be slower because it evicts the caller from L1i. Measure it, and
measure it cold.

And measurement here means counters, not stopwatches, because the effect you are chasing
is specifically instruction-side:

```sh
$ perf stat -e cycles,instructions,\
L1-icache-load-misses,iTLB-load-misses,\
idq_uops_not_delivered.core \
      -- ./trader --replay=cap.pcap
```

Compare the before and after values of `L1-icache-load-misses` and `iTLB-load-misses`.
If layout work did what you hoped, both fall substantially and the top-down frontend-bound
percentage falls with them. If they did not move, the change did nothing regardless of
what the wall clock says, and you are looking at the alignment noise from lesson 27.

:::hft
The layout win on a trading system is concentrated in the message you care about most. A
back-to-back replay keeps the hot path resident in L1i, so PGO and BOLT often show a
modest throughput gain and an unimpressive benchmark. The real effect appears on the first
message after a quiet period, when nothing is resident: a hot path packed into four pages
takes a handful of instruction fetches to get going, while the same path scattered across
forty pages takes dozens of misses and several iTLB walks before it executes any of your
logic. Measure this deliberately, by replaying with realistic idle gaps and reading the
p99.9 rather than the mean.
:::

:::exercise
Take any multi-file program from an earlier lesson, build it three ways, and record
`L1-icache-load-misses` and `iTLB-load-misses` for each: plain `-O2`; `-O2` with
`[[gnu::cold, gnu::noinline]]` applied to every error path; and `-O2` with full
`-fprofile-generate`/`-fprofile-use`. Then check `size --format=sysv` on each binary and
find how many bytes moved from `.text` into `.text.unlikely`. Predict the ordering of the
three miss counts before you run them.
:::

## Where this sits among the other build-level wins

Profile-guided optimisation and BOLT are the two most valuable things you can do to a
binary without editing it, which is why they get a lesson of their own. They are not the
only ones, and they are not the first ones: link-time optimisation, a correctly pinned
`-march`, hidden symbol visibility and a better allocator are all cheaper, and two of them
are a single flag. Lesson 29a ranks the whole set by payoff against effort, and says what
each is typically worth, so you can work down the list rather than reaching for the most
sophisticated tool first.

## Takeaways

- Instructions compete for cache and TLB exactly like data: a typical core gives you 32 KB
  of L1i and an iTLB covering a few hundred kilobytes of code. Layout work pays only when
  top-down says frontend bound, so check that first.
- Push error handling behind `[[gnu::cold, gnu::noinline]]` calls so the fall-through path
  stays dense, and let `-freorder-blocks-and-partition` separate the sections.
- PGO's value is entirely in the representativeness of the profiling run. Replay recorded
  market data across open, midday and close, never a synthetic loop.
- AutoFDO buys most of PGO's benefit from production `perf` samples at under 1% overhead,
  which keeps the profile current.
- BOLT rewrites layout post-link and reports roughly 5% to 8% on top of PGO for large C++
  services, mostly by cutting instruction-cache and iTLB misses. Huge text pages compound it.
- Judge all of it by `L1-icache-load-misses` and `iTLB-load-misses`, on a replay with
  realistic idle gaps, not by a back-to-back throughput number.
