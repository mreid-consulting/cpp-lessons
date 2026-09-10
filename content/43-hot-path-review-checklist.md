---
title: Hot Path Review Checklist
part: Appendix
summary: The questions to ask, in order, when reviewing a change to code that runs between a packet arriving and an order leaving, plus a symptom-to-cause table for when the numbers look wrong.
time: 10 min
level: advanced
tags: review, checklist, latency, diagnosis
---

Most latency regressions are not clever. They are a `std::string` in a log line, a
`push_back` past capacity, a `virtual` call added to make testing easier. They pass code
review because the reviewer was reading for correctness, which is the wrong lens for the
twenty functions between the packet and the wire.

This is the lens. Every item is a question a reviewer asks out loud, with the reason it
matters and the lesson that explains it. It is deliberately dense; skim the section
headings, then read the section that matches the diff.

## What the reviewer is looking for

A hot path review is not a correctness review with extra steps. It answers one question:
**does this change make the ninety-ninth-point-ninth percentile worse, and how would we
know?** Three ground rules make the rest of the checklist work.

First, the hot path must be identified. If nobody can say where it starts and ends, there
is no hot path, only code. Second, a claim about speed in a review comment is either
accompanied by a number and the host it came from, or it is an opinion. Third, the reviewer
who owns latency reads every diff that touches the identified path, even a one-line one.

```sh Four minutes of evidence, gathered before the review starts
$ perf stat -e cycles,instructions,branches,branch-misses,cache-misses,page-faults ./trader
$ strace -c -f ./trader                  # every syscall on the path, with counts
$ nm --size-sort -S ./trader | tail -20  # what grew since the last build
$ objdump -d --no-show-raw-insn -M intel ./trader | c++filt > new.asm  # diff against old.asm
```

:::hft
Desks that keep their tail latency flat all do the same unglamorous thing: the hot path is
a named, small, listed set of files, and changes to it go through a different review lane
from everything else. The lane is not slower, it is narrower. The reviewer's job is not to
approve the design, it is to ask the questions below and to require a before-and-after
p99.9 from the same host, taken with the same binary flags. "It should be faster" is not a
review answer; neither is "the mean improved".
:::

## Memory: allocation, lifetime and layout

### Allocation and lifetime

| Ask | Why | Lesson |
|---|---|---|
| Does this path call `new`, `delete`, or `malloc` at all? | Any allocation can take a lock, touch a new page, or fall through to `mmap`. The tail is unbounded. | 24 |
| Is every container sized to its worst case before the path starts? | One `push_back` past capacity reallocates and copies the whole buffer. | 24, 10 |
| Does a `std::string`, `std::function`, `std::any` or `std::vector` appear in a signature here? | Each one hides an allocation or a type-erased indirect call. | 14, 24 |
| Are objects taken from a pre-built pool rather than constructed per event? | Construction cost becomes startup cost, where it is free. | 24 |
| Does any destructor on this path do real work? | The cost is at the closing brace, where no reviewer is looking. | 4, 8 |
| Does a `std::shared_ptr` get copied? | Two atomic read-modify-writes and a cache line bouncing between cores. | 8, 32 |
| Does a lambda capture a container by value? | A silent copy per invocation, invisible at the call site. | 14 |
| Can anything on this path throw during normal operation? | Throwing costs microseconds and historically serialises on a global lock during unwinding. | 18 |

### Data layout

| Ask | Why | Lesson |
|---|---|---|
| How many distinct cache lines does one event touch? Count them. | This number predicts the latency better than the instruction count does. | 19 |
| Are the fields read together adjacent, with cold fields moved to a side structure? | A 64-byte line brings in whatever is next to what you asked for. | 20 |
| Should this array of structs be a struct of arrays? | Halving the lines touched per event is the largest cheap win available. | 20 |
| Are two fields written by two different threads in one cache line? | False sharing turns an independent write into a coherence round trip. | 21 |
| Is `sizeof` the hot struct pinned with a `static_assert`? | A struct that quietly grows past a line boundary is a silent regression. | 21 |
| Is a lookup a pointer chase through `std::map` or `unordered_map` where a dense index would do? | Each hop is a dependent load that cannot be prefetched. | 10, 37 |
| Are indices used rather than pointers into a container that can reallocate? | Indices survive growth and are half the size. | 20 |
| Is the per-event working set within L1 or L2? | Once it exceeds L2 the latency distribution changes shape, not just scale. | 19 |

## Execution: control flow and dispatch

### Control flow

| Ask | Why | Lesson |
|---|---|---|
| Is the common case the fall-through path? | A taken branch costs a fetch bubble even when predicted. | 22 |
| Are reject, error and risk-breach paths marked `[[unlikely]]` and pushed out of line? | Cold code interleaved with hot code wastes instruction cache. | 22, 29 |
| Is there a loop-invariant branch inside the loop that could be hoisted? | The compiler often cannot hoist it if a call intervenes. | 22 |
| Is any branch data-dependent and genuinely unpredictable? | If so it should be branchless: a conditional move, a mask, or arithmetic. | 22, 25 |
| Can the compiler see the loop bound? | An opaque bound blocks vectorisation and unrolling entirely. | 25 |
| Is the hot loop free of calls the compiler cannot inline? | One opaque call kills vectorisation for the whole loop. | 25, 23 |
| Is logging, metrics or assertion code inline here rather than behind a compile-time switch or a ring buffer? | Formatting a message is orders of magnitude more expensive than the work it describes. | 39 |

### Function calls and dispatch

| Ask | Why | Lesson |
|---|---|---|
| Is there a `virtual` call per event? | An indirect call the branch target buffer must predict, and an inlining barrier. | 23 |
| Could it be a template parameter, a `std::variant` visit, or a switch over a small enum? | All three give the compiler a fixed set of targets to devirtualize. | 23, 15 |
| Is the callee visible: defined in a header, or reachable through LTO? | Optimisation stops at the translation unit boundary without it. | 1, 23 |
| Did anyone check that it was actually inlined? | `-fopt-info-inline-missed` or `-Rpass=inline` answers in seconds. | 23, 42 |
| Is something large and rare being inlined into the hot loop? | Over-inlining evicts the instruction cache and shows up only as p99 damage. | 29 |
| Do calls cross a shared-library boundary through the PLT? | An extra indirect jump per call, removable with `-fno-plt`. | 42, 29 |
| Are hot functions grouped so the hot text is contiguous? | Layout is worth several percent, and PGO or BOLT does it for you. | 29 |

## Concurrency and the kernel

### Concurrency

| Ask | Why | Lesson |
|---|---|---|
| Is there a lock on this path at all? | A contended mutex means a syscall and a scheduler decision inside your latency budget. | 31 |
| Is every shared variable atomic, or provably not shared? | A plain race is undefined behaviour, not merely a stale read. | 31, 32 |
| Is each memory order the weakest that is still correct, and is the argument in a comment? | Ordering is not reviewable unless the intended happens-before is written down. | 32 |
| Is `seq_cst` used where acquire/release would do? | Sequential consistency adds a fence on the store side on x86-64. | 32 |
| Do the producer's index and the consumer's index share a cache line? | The classic ring-buffer false-sharing bug. | 33, 21 |
| Is market data published through a seqlock rather than a lock? | Readers must never be able to block the writer. | 34 |
| Are threads pinned, cores isolated, and interrupts moved elsewhere? | An unpinned hot thread will be migrated at the worst possible moment. | 35, 30 |
| Is a spin loop using a pause instruction, and is spinning justified over blocking? | Spinning burns a core and only pays if the wait is shorter than a context switch. | 35 |
| Is hot data on the same NUMA node as the thread that reads it? | A remote node access roughly doubles memory latency. | 30, 19 |
| Is `thread_local` used here inside a shared library? | The general TLS model calls a function on every access; `initial-exec` does not. | 30, 42 |

### System calls and I/O

| Ask | Why | Lesson |
|---|---|---|
| Does this path make any syscall? Count them with `strace -c` or `perf trace`. | Every one is a mode switch with a tail you do not control. | 30, 38 |
| How often is a clock read, and does it go through the vDSO? | Timestamping is not free and is easy to do three times per event. | 26 |
| Is there a `write`, a flush, or a `printf` here? | Blocking I/O in a latency path is the most common single cause of a bad p99.9. | 39 |
| Is the receive path a syscall per packet, or busy-polling or kernel bypass? | This choice dominates every other decision in the feed handler. | 38 |
| Are buffers pre-faulted and locked with `mlockall`, and are huge pages in use? | Otherwise the first touch of each page is a fault inside the hot path. | 30, 24 |
| Is anything demand-paged on the first message after a quiet period? | Yes, unless someone deliberately warmed it. | 30, 39 |
| Is the send path warmed so the first order out is not the slowest? | The one that matters most is usually the one that has not run recently. | 39, 38 |

## Arithmetic, correctness and the build

### Arithmetic and correctness

| Ask | Why | Lesson |
|---|---|---|
| Are prices integer ticks rather than `double`? | Floating point makes equality, rounding and reproducibility all negotiable. | 40, 2 |
| Does the file compile clean under `-Wconversion -Wsign-conversion`? | Prices are signed and quantities unsigned, so every notional is a conversion boundary. | 2, 42 |
| Can any intermediate overflow at the venue's maximum price and size? | Test the boundary the exchange permits, not the one you expect. | 40 |
| Is there a division by a runtime value in the loop? | Integer division is tens of cycles; a reciprocal multiply or shift is one or two. | 2, 40 |
| Are rounding rules for price and size defined once and tested? | Two definitions in two files is a reconciliation break waiting to happen. | 40 |
| Are risk checks on the same path as the send, and unconditional? | A risk check that can be bypassed is not a risk check. | 40, 41 |
| Does a test exercise this exact code, or a mock of it? | A mocked hot path is a test of the mock. | 27, 40 |

### Build and measurement

| Ask | Why | Lesson |
|---|---|---|
| Was the quoted number measured, and on which host? | Numbers without provenance are not evidence. | 26 |
| Is it a percentile from a histogram, not a mean? | The mean hides exactly the events you care about. | 26 |
| Does the benchmark keep its result alive so the compiler cannot delete the work? | An optimised-away benchmark reports an impossibly good number. | 27 |
| Was the measurement taken with the production kernel, governor and core layout? | Frequency scaling alone moves results by tens of percent. | 30, 26 |
| Do the benchmark and production builds use the same flags, `-march` and LTO setting? | Otherwise you measured a different program. | 42, 27 |
| Is the claimed improvement larger than the build-to-build layout noise? | Below roughly five percent, assume layout until proven otherwise. | 29, 27 |
| Is the PGO or BOLT profile still representative after this change? | A stale profile marks the new code cold and lays it out accordingly. | 29 |

## Symptom to cause

When the numbers look wrong, start here. The middle column is what to measure next; the
right column is what it usually turns out to be. Lesson 28 explains how to read each of
these counters properly; the commands below are the first cut.

```sh Narrowing a symptom to a counter
$ perf stat -e cycles,instructions,branch-misses,LLC-load-misses,\
L1-icache-load-misses,iTLB-load-misses,page-faults,context-switches ./trader
$ perf c2c record -- ./trader && perf c2c report --stdio   # cache line contention
$ perf record -e cycles:pp -g ./trader && perf report --stdio --sort=symbol
$ grep -E 'Huge|Thp' /proc/meminfo && numastat -p $(pgrep trader)
```

| Symptom | Measure next | Usual causes |
|---|---|---|
| Good p50, bad p99.9 | Histogram plus `perf stat` for `page-faults`, `context-switches` | Allocation falling through to the kernel, a lock, a syscall, a page fault, an interrupt landing on the hot core, or periodic batch work such as a rehash, a vector growth or a log flush. Lessons 24, 30, 39. |
| Instructions per cycle below about 1 | `perf stat` top-down, then `perf mem` or `perf c2c` | Memory-bound stalls: pointer chasing, array-of-structs layout, or a working set past L2. Lessons 19, 20, 28. |
| Branch miss rate above about 2% | `perf stat -e branches,branch-misses`, then `perf annotate` | Data-dependent branches on unsorted input, virtual dispatch over mixed types, or an indirect branch with many targets thrashing the branch target buffer. Lessons 22, 23. |
| High last-level cache miss rate | `perf stat -e LLC-loads,LLC-load-misses`, `numastat` | Working set exceeds cache, random access patterns, cold prefetch, or memory allocated on a remote NUMA node. Lessons 19, 20, 30. |
| High iTLB or L1 instruction-cache misses | `perf stat -e L1-icache-load-misses,iTLB-load-misses` | Code footprint too large: over-inlining, unrolling, cold code interleaved with hot, no PGO or BOLT, no huge pages backing the text segment. Lesson 29. |
| Latency degrades over hours | Resident set and allocator statistics over time; re-measure after a restart | Heap fragmentation, a container that only ever grows, an unbounded map, a log file, or transparent huge page compaction stalls. Lessons 24, 39. |
| Fine in the benchmark, bad in production | Run the benchmark with production data volume, on a production host | The benchmark's data fits in cache, its input distribution trains the branch predictor, it runs alone with no competing interrupts, or the compiler deleted the work. Lessons 27, 22, 30. |
| First message after idle far slower than the rest | Timestamp the first N events after a gap separately | Cold instruction and data caches, cold TLB, untrained branch predictor, lazy PLT binding on first call, demand-paged memory, or the core in a deep C-state. Fixes: keep-warm traffic, `-Wl,-z,now`, `mlockall`, disable deep C-states. Lessons 30, 39, 42. |
| Several percent difference between builds with no source change | Rebuild the identical source twice and compare; re-run each binary several times | Code layout: function alignment, symbol ordering, and hot loops landing across a 32-byte fetch boundary. Mitigate with `-falign-functions=32`, PGO and BOLT, and treat sub-five-percent differences as noise unless reproducible across link orders. Lessons 29, 27. |
| Context switches climbing under load | `perf stat -e context-switches,cpu-migrations` | Blocking on a mutex or condition variable, an unpinned thread, or a syscall that sleeps. Lessons 31, 35, 30. |

:::exercise
Take the last change that was merged into your hot path and run this checklist against the
diff, writing down every question it fails rather than fixing anything. Then check whether
the review comments at the time asked any of them. The gap between those two lists is the
value of this page.
:::

## Takeaways

- Review the hot path against a fixed list of questions, not against general good taste; the failures are repetitive and cheap to catch.
- Allocation, locks, syscalls and logging are the four causes of almost every bad p99.9; find them before looking at anything cleverer.
- Count cache lines per event and cache-line sharing between threads; these two numbers explain most of the rest.
- Require a before-and-after percentile from the same host with the same flags, and treat improvements under about five percent as code layout noise.
- A symptom names a measurement, not a cause; go from the symptom to the counter to the explanation, in that order.
- The questions that catch the most regressions are the dullest ones, which is exactly why a checklist beats a careful reader.
