---
title: Profiling: perf, Counters, Top-Down
part: Part IV - Latency Engineering
summary: Reading cycles, IPC and cache misses from perf stat, finding the guilty line with perf record, and using the top-down method to decide whether you are frontend bound, backend bound, or just mispredicting.
time: 35 min
level: advanced
tags: perf, pmu, top-down, flamegraph, pebs, linux
---

A benchmark tells you that something is slow. A profiler tells you why. The CPU in your
trading server contains a performance monitoring unit, a small block of hardware that
counts events, and `perf` is the Linux interface to it. Learning to read four or five of
its counters converts guesswork into a decision procedure, and it takes about an afternoon.

Everything in this lesson is Linux-specific. The counters are broadly the same on any
modern x86-64 server core; the exact event names differ between microarchitectures and
between Intel and AMD.

## perf stat: the first thirty seconds

Never open a profiler before you have run `perf stat`. It costs nothing, it perturbs
almost nothing, and it usually tells you which of the next four sections to read.

```sh
$ perf stat -d ./feed_replay --file=recorded_2026_09_09.pcap

     4,812.61 msec task-clock                #    1.000 CPUs utilized
   14,436,981,204      cycles                #    3.000 GHz
   21,655,471,812      instructions          #    1.50  insn per cycle
    3,101,224,904      branches              #  644.4 M/sec
       92,418,110      branch-misses         #    2.98% of all branches
    5,220,447,301      L1-dcache-loads       # 1084.7 M/sec
      311,902,884      L1-dcache-load-misses #    5.97% of all L1-dcache accesses
       48,110,207      LLC-loads             #   10.0 M/sec
       29,844,996      LLC-load-misses       #   62.03% of all LL-cache accesses
```

Read it in this order.

- **Cycles and instructions** give **IPC** (instructions per cycle). A modern server core
  can retire four to six instructions per cycle at best. IPC is the single most useful
  summary number you have.
- **Branch-misses as a percentage of branches.** Under 1% is healthy. The 2.98% above is
  poor. Each mispredict costs roughly 15 to 20 cycles of wasted frontend work on a typical
  modern core.
- **Cache misses.** `L1-dcache-load-misses` around 5% is normal for a decoder. The 62% LLC
  miss rate above says that when you do fall out of L2, you are almost always going to
  DRAM, which on a typical server is about 80 to 100 ns of latency.

What counts as an acceptable IPC depends entirely on what the code is doing, and this is
where the number becomes diagnostic rather than decorative.

| Workload | Typical IPC | Why |
|---|---|---|
| Tight binary decoder, hot in L1 | 2.5 to 4.0 | Independent integer work, predictable branches |
| Fixed-point arithmetic on a `span` | 2.0 to 3.5 | Vectorisable, few dependencies |
| Order book walking a linked structure | 0.3 to 0.9 | Serialised on load latency, nothing to overlap |
| Spinning on an empty ring buffer | 0.1 to 0.5 | Meaningless; the loop is waiting, not working |

A decoder at IPC 0.6 is a bug. A book walk at IPC 0.6 may be entirely expected, and the
fix is a data layout change (lesson 20), not an instruction-level one.

:::note
`perf stat` needs permission to read the PMU. On most systems you need
`sudo sysctl kernel.perf_event_paranoid=1` or lower, and inside a container you generally
need `--cap-add=CAP_PERFMON` or the older `CAP_SYS_ADMIN`.
:::

## perf record: from a counter to a line

`perf record` samples. It programs a counter to overflow every N events and captures the
instruction pointer, and optionally the call stack, at each overflow. Sample on cycles to
find where time goes; sample on a cache-miss event to find where the misses come from.

```sh
$ perf record -F 999 -g --call-graph dwarf -- ./feed_replay --file=cap.pcap
$ perf report --stdio --percent-limit=1
$ perf annotate --stdio -s decode_incremental      # per-instruction breakdown
```

Three things about call graphs are worth knowing, because getting them wrong wastes hours:

- **Frame pointers.** By default `-O2` omits the frame pointer, and the default
  `--call-graph fp` then produces garbage or one-deep stacks. Build the binary with
  `-fno-omit-frame-pointer`. The cost is one register, typically under 1% on x86-64, and
  it is worth paying permanently on a binary you intend to profile.
- **`--call-graph dwarf`** copies a chunk of the stack with every sample and unwinds
  offline using debug info. It works without frame pointers but produces large files and
  a much higher overhead, so it is a development tool, not a production one.
- **`--call-graph lbr`** uses the Last Branch Record, a hardware ring of recent branches.
  It is nearly free and needs no frame pointers, but the ring is shallow, 16 to 32 entries
  depending on the part, so deep stacks are truncated. On a hot path that is rarely deep,
  LBR is the best of the three.

**Flame graphs** turn a `perf report` into a picture. Width is time, stacking is call
depth, and a wide plateau is where your cycles are.

```sh
$ perf record -F 999 -g --call-graph lbr -- ./feed_replay --file=cap.pcap
$ perf script | stackcollapse-perf.pl | flamegraph.pl > feed.svg
```

### Skid, and why the blame lands next door

When a counter overflows, the CPU raises an interrupt. By the time the interrupt is taken
the machine has moved on, so the recorded instruction pointer is not the instruction that
caused the event but one some distance after it. This is **skid**, and on an out-of-order
core it can be tens of instructions. The visible symptom is a `perf annotate` output where
100% of the cache misses are attributed to an `add` that touches no memory, and the
adjacent load, which is the real culprit, shows zero.

The fix is **PEBS**, Precise Event-Based Sampling. The hardware writes a record with the
architectural state at retirement of the instruction that actually caused the event, into
a buffer, without an interrupt. You ask for it with a `:p` suffix, and more `p`s mean more
precision.

```sh
$ perf record -e cycles:pp -F 999 -- ./feed_replay --file=cap.pcap
$ perf record -e mem_load_retired.l3_miss:pp -c 5000 -- ./feed_replay --file=cap.pcap
```

:::pitfall
Never draw a conclusion from a non-precise memory event annotation. If the event name does
not end in `:p` or `:pp`, the line number is approximate at best. Cheap habit: always
append `:pp` and let `perf` tell you if the event does not support it.
:::

## Top-down: which of the four is it

The **top-down microarchitecture analysis method** is the closest thing to a decision
procedure that exists for this work. It classifies every issue slot in the pipeline into
one of four buckets, and the four are exhaustive.

```text
                     issue slots
                    /           \
        uop issued?              not issued
        /        \                /        \
   retired?    cancelled     frontend    backend
   RETIRING   BAD SPECULATION  BOUND      BOUND
```

```sh
$ perf stat --topdown -a -C 7 sleep 10                  # if supported directly
$ toplev.py --level 2 -- ./feed_replay --file=cap.pcap  # pmu-tools, more detail
```

| Bucket | Meaning | What to do next |
|---|---|---|
| **Retiring** | Useful work completed | Do less work: better algorithm, fewer instructions, SIMD |
| **Bad speculation** | Work done then thrown away | Attack branch mispredicts (lesson 22): branchless, sorting, `[[likely]]` |
| **Frontend bound** | The pipeline starved of instructions | Instruction cache and layout (lesson 29): PGO, hot/cold splitting |
| **Backend bound** | The pipeline stalled on data or ports | Memory bound: layout, prefetch (lessons 19 to 21). Core bound: dependencies, port pressure |

A high retiring percentage is not automatically good news. If you are 70% retiring and
still too slow, the machine is running efficiently and executing too many instructions;
the answer is an algorithmic one. Backend bound splits further into **memory bound** and
**core bound**, and the level-2 breakdown tells you which level of the hierarchy is
responsible.

## Memory: perf mem, perf c2c, and counters worth naming

Two specialised subcommands answer questions that are painful to answer any other way.

`perf mem` records, for sampled loads and stores, the level of the hierarchy that supplied
the data and the access latency. It is the direct way to find the loads that miss.

```sh
$ perf mem record -- ./feed_replay --file=cap.pcap
$ perf mem report --sort=mem,sym,dso --stdio
```

`perf c2c` finds **false sharing**, which is the situation from lesson 21 where two
threads write different variables that happen to occupy the same 64-byte cache line and
therefore bounce the line between cores. It reports lines with high **HITM** counts, a
load that hit a modified line in another core's cache, together with the offsets within
the line and the symbols that touched them.

```sh
$ perf c2c record -a -- sleep 20
$ perf c2c report --stdio -NN
```

If a hot cache line shows two different offsets touched by two different threads, you have
found it, and the fix is `alignas(64)` padding or moving one of the fields.

Counters worth naming explicitly when you want a specific answer:

| Concern | Intel event | What a high value means |
|---|---|---|
| L1 data | `L1-dcache-load-misses` | Working set exceeds 32 to 48 KB, or bad stride |
| Last level | `LLC-load-misses` | Going to DRAM; a miss costs roughly 80 to 100 ns typically |
| Data TLB | `dTLB-load-misses` | Scattered pages; consider huge pages (lesson 30) |
| Instruction TLB | `iTLB-load-misses` | Hot path spread over many code pages (lesson 29) |
| Instruction cache | `L1-icache-load-misses` (or `icache_64b.iftag_miss`) | Code footprint too large or badly laid out |
| False sharing | `mem_load_l3_hit_retired.xsnp_hitm` | Another core owned the line, modified |

## Busy-poll threads and production boxes

The standard profiling intuition, "find the function using the most CPU", breaks
completely on a trading hot path. A pinned thread spinning on a ring buffer uses 100% of
its core by design, and a cycles profile of it says that 98% of the time is inside the
poll loop. That is true and useless.

Three adjustments make profiles of a busy-poller meaningful:

- **Profile the work, not the wait.** Sample on an event that only advances when real work
  happens, such as `instructions` restricted to your decode symbols, or gate recording on
  a marker. Better still, keep the poll loop and the work in separate functions so the
  flame graph separates them visually.
- **Compare against a known-idle profile.** Record the same thread while the feed is
  silent, and subtract. What remains is the work.
- **Trust in-process timestamps for the answer and `perf` for the mechanism.** Lesson 26's
  histogram tells you the latency; `perf` tells you which counter explains it.

In production, keep the overhead honest. A cycles profile at `-F 99` with LBR call graphs
costs well under 1% and is safe to leave running on a live box; `--call-graph dwarf` at
`-F 999` is not. Prefer `perf record --switch-output` with a time or size trigger so you
capture a bounded window around an incident rather than a multi-gigabyte file, and always
attach to a specific CPU with `-C` rather than profiling the whole machine.

:::hft
Profiling data has the same problem as market data: the interesting part is rare. The
cycles profile of a feed handler is dominated by the 99% of messages that are ordinary,
while the latency you are paid to fix belongs to the 0.1% that arrive in a burst after a
quiet period, cold in every cache. Sample-based profiles average that away. The working
practice is to pair a continuous low-rate `perf` profile with a per-message timestamp
histogram, then use the histogram to identify a slow window and `--switch-output` to
capture the profile for that window specifically.
:::

## A worked decoder profile

A market-data decoder replays a captured session in 4.8 seconds. The target is 3.5.

**Step 1, `perf stat`.** IPC is 1.50, which is low for a decoder that should be doing
independent integer work. Branch miss rate is 2.98%, high. LLC load miss rate is 62%.
Two suspects already.

**Step 2, top down.** `toplev.py --level 2` reports 24% retiring, 19% bad speculation,
7% frontend bound, 50% backend bound, and within backend, memory bound dominates. So the
branches matter but memory matters more than twice as much. Memory first.

**Step 3, precise sampling on misses.**

```sh
$ perf record -e mem_load_retired.l3_miss:pp -c 2000 \
      -g --call-graph lbr -- ./feed_replay --file=cap.pcap
$ perf report --stdio --percent-limit=2
    61.4%  feed_replay  [.] Book::level_for_price
    14.9%  feed_replay  [.] decode_incremental
```

**Step 4, annotate.**

```sh
$ perf annotate --stdio -s _ZN4Book14level_for_priceEl
   0.31 :   mov    (%rdx),%rax          # load node->price_ticks
  58.90 :   mov    0x18(%rax),%rdx      # load node->next   <-- the miss
   0.12 :   test   %rdx,%rdx
```

The misses are on a pointer dereference inside a linked list of price levels. One level
per cache line at best, and the traversal is a dependency chain, so nothing overlaps. This
is the IPC 0.6 pattern from the table above, hiding inside a program whose average IPC is
1.5.

**Step 5, the fix and the confirmation.** Replace the linked list with a flat array
indexed by price offset from the top of book, as in lesson 20. Rerun `perf stat`: IPC 2.9,
LLC-load-misses down by roughly an order of magnitude, wall time 3.1 seconds. Then, and
only then, rerun the wire-to-wire histogram to confirm the change moved the number the
desk actually cares about.

:::exercise
Build any program from an earlier lesson with `-O2 -g -fno-omit-frame-pointer` and run
`perf stat -d` on it. Write down its IPC and branch miss rate before looking at anything
else, and predict which top-down bucket will dominate. Then run `perf record -e cycles:pp`
and check whether the hot function is the one you expected. The gap between your
prediction and the counters is the thing worth remembering.
:::

## Where sampling stops

Everything in this lesson is a sampling profiler, and sampling has a boundary worth naming
before you walk into it. A profile is a statistical answer to "where do the cycles go",
built from an interrupt every few thousand events. That is the right tool for finding the
function your hot path spends its time in, and it is structurally incapable of explaining a
tail event.

At a 4 kHz sampling rate you get one sample every 250 microseconds. An event that happens
once in ten million messages and costs you 300 microseconds contributes roughly one sample
to a profile containing millions of them. It is indistinguishable from noise, and it is the
event you are being paid to eliminate.

Lesson 28a covers the other half: instrumentation with Clang XRay, hardware tracing with
Intel Processor Trace, and the always-on ring tracer that most desks end up writing. Use a
profiler to find the hot path, and a tracer to explain the slow occasion.

## Takeaways

- Run `perf stat` first. IPC, branch miss rate and LLC miss rate usually name the problem
  before you open a profiler.
- An acceptable IPC is workload-dependent: 3 for a decoder, under 1 for a pointer-chasing
  book. Judge against the workload, not an absolute.
- Build profiled binaries with `-g -fno-omit-frame-pointer`, prefer LBR call graphs where
  stacks are shallow, and always ask for a precise `:pp` event, because a non-precise
  memory event blames the wrong instruction.
- Top-down splits every cycle into retiring, bad speculation, frontend bound and backend
  bound, and each bucket points at a different chapter of this course.
- `perf c2c` finds false sharing and `perf mem` finds the missing loads. Both answer
  questions that cycles profiles cannot.
- On a busy-polling pinned thread, 100% CPU is the design. Separate the poll from the work
  and subtract an idle baseline before believing a profile.
