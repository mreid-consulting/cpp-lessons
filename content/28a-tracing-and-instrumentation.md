---
title: Tracing: XRay, Processor Trace and Friends
part: Part IV - Latency Engineering
summary: Sampling tells you where the average microsecond goes. Tracing tells you what happened on the one occasion that was slow, which is the only occasion you are paid for.
time: 40 min
level: expert
tags: xray, intel-pt, tracing, ebpf, instrumentation
---

A sampling profiler answers the question "where does the average microsecond go". For a
trading system that is the wrong question. You are paid on the tail: the message that took
400 microseconds when the median took 4, once in a million, on the burst that followed the
number. A profiler sampling at 4 kHz takes one sample every 250 microseconds and will never
see that event, and if it did it would fold it into an average and hide it. Averaging away
the tail is precisely the operation you must not perform.

Tracing answers a different question: *what actually happened, in order, on the occasion
that was slow*. Lesson 28 covered the sampling half of the toolkit. This lesson covers the
other half, and ends with the one tool most desks eventually write themselves. Everything
here except the last two sections is Linux-specific, and Processor Trace is Intel-specific.

## Sampling and tracing are different trades

**Sampling** interrupts the program at a fixed rate and records where it was. Overhead is a
function of the sample rate alone, *independent of how often your events occur*: at 999 Hz
it costs well under 1% whether the decoder runs a thousand times a second or ten million.
What you get back is statistical. An event occupying 0.001% of wall time is expected to
appear in 0.1 samples of a ten-second capture at 999 Hz. You will not see it.

**Tracing** records every occurrence of a chosen event, so overhead scales with the event
rate: instrumenting a function called ten million times a second at 20 ns a record costs 20%
of a core. What you get back is exact, and the rare event is in it because every event is.

| | Sampling | Tracing |
|---|---|---|
| Overhead scales with | sample rate | event rate |
| Result | statistical profile | exact ordered record |
| Finds the rare event | no | yes |
| Answers | where do cycles go | what happened, and when |

The honest workflow uses both, in order. Sample first to find the hot path, because you
cannot afford to instrument everything. Then trace the few points that matter. Reaching for
a tracer first buys a gigabyte of records about innocent code.

## Clang XRay: patchable sleds in your own binary

XRay is Clang and LLVM's function-level tracing framework, and one trick makes it
deployable. Compiling with `-fxray-instrument` emits a **nop sled** at the entry and exit of
each instrumented function: a short run of no-op instructions, sized so the runtime can
later overwrite them with a call to a logging handler. Unpatched, the sleds are inert, so a
binary that is not recording executes a few no-ops per call and runs at close to full speed.
You ship the *same* binary to production and turn recording on when you want it.

```sh
$ clang++ -std=c++23 -O2 -g -fxray-instrument \
      -fxray-instruction-threshold=200 feed.cpp -o feed_traced
```

`-fxray-instruction-threshold=N` is the volume knob: XRay instruments only functions whose
bodies exceed it, on the reasoning that an entry and exit record around a two-line accessor
tells you nothing and costs everything. Raise it to instrument less, then override by hand
where it matters.

```cpp pipeline.hpp
#include <cstdint>
#include <span>

struct Book;
struct Signal;

// Stage boundaries: always traced, whatever the threshold says.
[[clang::xray_always_instrument]] [[clang::xray_log_args(1)]]
std::uint64_t decode_incremental(std::span<const std::byte> wire, Book& book);

[[clang::xray_always_instrument]]
Signal evaluate(const Book& book);

[[clang::xray_always_instrument]] [[clang::xray_log_args(1)]]
void send_order(std::int64_t price_ticks, std::uint32_t qty);

// Accessors on the hot path: never traced. A sled here would cost more than
// the function body and would bury the stage records in noise.
struct Level {
    [[clang::xray_never_instrument]] std::int64_t price() const noexcept { return px_; }
    [[clang::xray_never_instrument]] std::uint32_t qty() const noexcept { return qty_; }
    std::int64_t px_;
    std::uint32_t qty_;
};
```

That is the shape of a good XRay configuration: the stage boundaries you would draw on a
whiteboard are always instrumented, everything small and hot is excluded, and the threshold
handles the middle. `[[clang::xray_log_args(1)]]` records the first argument alongside the
timestamp, which is how you tie a record back to a sequence number.

Recording is controlled at run time through the `XRAY_OPTIONS` environment variable.

```sh
$ XRAY_OPTIONS="patch_premain=true xray_mode=xray-fdr verbosity=1 \
      xray_fdr_log_func_duration_threshold_us=5" ./feed_traced --file=cap.pcap
$ llvm-xray account xray-log.feed_traced.* --sort=sum --top=10 --format=text
$ llvm-xray graph   xray-log.feed_traced.* --color=sum --edge-label=avgdur > g.dot
$ llvm-xray stack   xray-log.feed_traced.* --aggregate-threads --stack-format=flame
$ llvm-xray convert xray-log.feed_traced.* --output-format=trace_event -o trace.json
```

`patch_premain=true` patches the sleds before `main` runs. The two modes matter more than
anything else here:

- **`xray-basic`** writes every record to a log as it happens. Simple, and the file grows
  without bound. Fine for a replay harness, wrong for a live process.
- **`xray-fdr`**, the **flight data recorder**, writes into a fixed-size in-memory ring and
  flushes only when asked. This is the mode that matters for trading. The process runs for
  hours; when a monitor sees a latency breach it flushes, and you get the last few
  milliseconds of function-level history leading up to it.

The `llvm-xray` subcommands are the reading half: `account` gives per-function call counts
and latency percentiles, `graph` a call graph annotated with timings, `stack` the hot stacks,
and `convert` a JSON trace you read as a timeline in Chrome or Perfetto.

Be honest about the cost. Every instrumented call writes an entry and an exit record, on the
order of tens of nanoseconds per call pair while recording, on a typical modern server. A
stage boundary crossed 50,000 times a second is free. A function called ten million times a
second is not a candidate, and no threshold makes it one.

## Intel Processor Trace: the CPU keeps the log for you

**Intel Processor Trace** (PT) is a hardware facility on Intel server parts from Broadwell
onwards, and *not* on AMD, which has its own different mechanism. When enabled, the CPU
records the outcome of every conditional branch and the target of every indirect branch into
a memory buffer as a compressed packet stream. Because it stores branch decisions rather
than addresses, and dedicated hardware does the writing, the cost is small: typically a few
percent on branch-heavy code, often under 2% on a tight decoder, on recent Xeon parts.

Given those branch outcomes plus the binary, a decoder reconstructs the exact instruction
path the CPU took. Not a sample of it. All of it.

```sh
$ perf record -e intel_pt//u -C 7 -- ./feed_replay --file=cap.pcap
$ perf script --insn-trace --xed -C 7 | head -40
$ perf script --call-trace                 # function-level view, far smaller
```

What makes this decisive for a desk is the property that makes XRay's flight data recorder
decisive: you run PT **continuously in a small circular buffer** and dump only when a
trigger fires.

```sh
$ perf record -e intel_pt//u --snapshot=e -m ,16M -a -- sleep 3600 &
$ kill -USR2 %1        # the monitor sends this when a latency breach is detected
```

With `--snapshot`, `perf` overwrites a 16 MB ring and writes nothing to disk until it
receives `SIGUSR2`. What lands on disk is the instruction-level history of the microseconds
immediately before the breach: a recording of the slow event itself, not a summary of the
fast ones. No other tool gives you that.

Two caveats, both real. The data rate is large, hundreds of megabytes per second per core on
branchy code, which is why the snapshot workflow exists. And decoding is far slower than
capture, seconds to minutes of CPU for a few milliseconds of trace, so treat decode as an
offline batch step rather than something you do while an incident call waits.

### magic-trace

Jane Street's **magic-trace** is Intel PT packaged for exactly that workflow, and it is the
shape good tooling takes here. You run or attach to a process, specify a trigger, and get a
snapshot you open in the Perfetto UI as a flame-graph timeline.

```sh
$ magic-trace attach -pid $(pgrep feed_handler) -duration 0.5ms
$ magic-trace run ./feed_replay -trigger send_order -- --file=cap.pcap
```

The trigger can be a signal, a symbol, or a probe you place yourself, and the result is a
timeline of which functions ran, nested, with real durations, in the microseconds before it
fired. When the complaint is "our p99.9 is bad and nobody knows which code path causes it",
this ends the argument: you stop reasoning about which path *might* be slow and read which
path *was*.

## perf's other half: probes, syscalls and off-CPU

Lesson 28 used `perf` as a sampler. It is also a tracer, and four subcommands carry that
weight.

```sh
$ perf probe -x ./feed_handler --add 'decode_incremental seq=%di'   # dynamic tracepoint
$ perf record -e probe_feed_handler:decode_incremental -aR -- sleep 10
$ perf trace -p $(pgrep feed_handler) --duration 50                  # syscalls over 50us
$ perf sched record -- sleep 10 && perf sched latency --sort max
```

`perf probe` places a dynamic tracepoint on any function in your own binary, by symbol and
with argument capture, **without recompiling**, which means you can instrument the
production binary you already deployed. `perf trace` is `strace` done properly, and its
`--duration` filter finds the one `write` that blocked for 8 ms.

`perf sched` is the underused one. It reports how long a runnable thread waited for a core
and how long it spent off-CPU. **Off-CPU analysis matters because the failure mode it
catches is invisible to every on-CPU profiler.** The thread you believed was spinning on a
ring buffer was descheduled for 300 microseconds because an unrelated process woke on its
core. A cycles profile of it shows only fewer samples, and fewer samples look like less work,
which looks like good news. Lesson 35 covers the pinning that prevents this.

## eBPF and bpftrace

**eBPF** runs small verified programs inside the kernel, attached to tracepoints, kprobes,
uprobes and perf events, with no module to load and no restart. That last property is the
point on a production box: you attach to a running process and detach again, and if the
verifier accepted the program it cannot crash the kernel. **bpftrace** is the one-liner
front end.

```sh
# Histogram of block-device write latency, in nanoseconds, until Ctrl-C.
$ sudo bpftrace -e '
    kprobe:blk_mq_start_request { @s[arg0] = nsecs; }
    kprobe:blk_mq_end_request   /@s[arg0]/ {
        @us = hist((nsecs - @s[arg0]) / 1000); delete(@s[arg0]);
    }'
```

Syscall counts and latencies, scheduler wakeups and run-queue delay, block-device and
network-stack latency, page faults, TCP retransmits: eBPF sees all of it, cheaply, in
production. Know its boundary. It sees the kernel extremely well and your user-space hot
loop poorly, because a uprobe costs on the order of a microsecond per hit on a typical
server, one to two orders of magnitude more than what you are measuring inside a decoder.
Use eBPF below your process and something else inside it.

## Roll your own, and why most desks do

Here is the tracer that actually lives in production trading systems: a fixed-size per-thread
ring of small records, each holding a TSC timestamp, an event id and two payload words,
written with a relaxed store and no allocation, drained by a cold thread and decoded offline.

```cpp trace_ring.hpp
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

// The rdtsc wrapper and its calibration are from lesson 26.
std::uint64_t tsc_raw() noexcept;

enum class Ev : std::uint32_t { wire_rx = 1, decoded, book_applied, signal, order_sent };

struct TraceRec {                 // 32 bytes: two per cache line, no false sharing
    std::uint64_t tsc;
    std::uint32_t ev;
    std::uint32_t pad;
    std::uint64_t a;              // sequence number, instrument id, ...
    std::uint64_t b;              // price in ticks, qty, ...
};
static_assert(sizeof(TraceRec) == 32);

template <std::size_t N>
class TraceRing {
    static_assert(std::has_single_bit(N), "N must be a power of two");
public:
    // Hot path. One rdtsc, one 32-byte store, one counter bump. Never blocks.
    void put(Ev e, std::uint64_t a = 0, std::uint64_t b = 0) noexcept {
        const std::uint64_t i = head_.load(std::memory_order_relaxed);
        TraceRec& r = buf_[i & (N - 1)];
        r.tsc = tsc_raw();
        r.ev  = static_cast<std::uint32_t>(e);
        r.a   = a;
        r.b   = b;
        head_.store(i + 1, std::memory_order_release);   // publish
    }

    // Cold thread. Copies out the most recent records; a record being
    // overwritten as we read it is acceptable, the ring is a best-effort log.
    std::size_t snapshot(TraceRec* out, std::size_t cap) const noexcept {
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::size_t have = static_cast<std::size_t>(h < N ? h : N);
        const std::size_t take = have < cap ? have : cap;
        for (std::size_t k = 0; k < take; ++k)
            out[k] = buf_[(h - take + k) & (N - 1)];
        return take;
    }

private:
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::array<TraceRec, N> buf_{};
};

inline thread_local TraceRing<8192> g_trace;   // 256 KB per thread
```

`tsc_raw()` is declared here rather than defined, because its body is the `__rdtsc()`
wrapper from lesson 26 and belongs there. A complete version that also works on ARM, plus
the calibration that turns ticks into nanoseconds, ships with this series as
`examples/trace_ring.hpp`.

Using it is one line per stage boundary, and a decode step that never runs on the hot path.

```cpp hot_path.cpp
#include <cstdint>

void handle(std::uint64_t seq, std::int64_t px_ticks, std::uint32_t qty) noexcept {
    g_trace.put(Ev::wire_rx, seq);
    // decode ...
    g_trace.put(Ev::decoded, seq);
    // apply to book, evaluate, send ...
    g_trace.put(Ev::order_sent, seq, static_cast<std::uint64_t>(px_ticks) ^ qty);
}
```

Why this wins over every tool above. You choose exactly which points deserve a timestamp, so
there is no threshold to tune and no noise. The cost is one `rdtsc` plus one store, roughly
7 to 10 ns on a typical modern x86-64 server, and it does not grow with call depth. No
allocation, lock or syscall, so it is safe inside the allocation-free hot path of lesson 24.
Most importantly it is **always on**: when the p99.9 breach happens at 09:31:07, the trace of
that exact message already exists and you flush it.

Two details to get right. Use a single-producer single-consumer queue (lesson 33) instead of
a raw ring if the drain must be lossless, and keep the drain thread on another core so
publishing never contends. And record raw TSC in the hot path, converting to nanoseconds
offline, for the reasons lesson 26 gives.

:::hft
This pattern is near-universal on desks for one reason. The event you must explain happens
once a day, on the live feed, in the process currently holding risk, and you cannot reproduce
it in a replay harness, because what made it slow was a cold instruction cache after a quiet
period, or a neighbour thread waking on your core, or a page fault on a buffer untouched
since the open. A tool you switch on *after* the incident was, by definition, off when it
mattered. The ring tracer is always on at under 10 ns a point, so the record of the bad event
exists before anyone knows they need it. XRay's flight data recorder and `perf`'s PT snapshot
mode are the same idea with more machinery.
:::

:::exercise
Add `g_trace.put` calls at five stage boundaries of any pipeline you have built in this
course: packet received, decode complete, book updated, signal evaluated, order on the wire.
Replay a few million messages, drain the ring, and build a histogram of the `wire_rx` to
`order_sent` delta as in lesson 26. Now find the slowest 10 messages and print their full
five-record sequences beside a median message. Which *stage* was slow, and was it the same
stage every time? Then time the same replay with the `put` calls compiled out, and confirm
the tracer's overhead is where you expect it.
:::

### Run it

`examples/capstone/trace_demo.cpp` is the exercise above, already done against the pipeline
from lesson 41.

```sh
$ cd examples/capstone && make trace_demo && ./trace_demo
$ make clean && make trace_demo TRACE=0 && ./trace_demo
```

The second build compiles the tracer out through a no-op ring, which is how you price it
rather than estimate it. On one x86-64 server the traced run costs about 73 ns per message
against roughly 50 ns of pipeline, so five trace points come to about 20 ns, which matches
the per-point figure this section claims.

The output is the part that matters. It prints the median and p99.9 stage breakdown, then
the ten slowest messages with the stage that dominated each. On that machine the median
message spends 20 ns in decode and 10 ns in each later stage, while the p99.9 spends 220 ns
in decode alone. Read the worst-stage column: the same stage every time is a code problem,
a different stage each time is a machine problem, and lesson 30 is where you go next.

:::warn
The program prints its counter frequency, and you should read that before anything else.
`rdtsc` on x86-64 runs at the core's nominal frequency, so a tick is a fraction of a
nanosecond. `cntvct_el0` on Apple silicon ticks at 24 MHz, so a tick is 41.7 ns, coarser
than an entire pipeline stage. On such a counter every per-stage figure quantises and only
the large outliers are real. Instrumentation that does not report its own resolution will
happily let you conclude your pipeline is free.
:::

## The microbenchmark boundary, and how to choose

Google Benchmark is deliberately absent from this lesson. It answers "is version A of this
function faster than version B, in the steady state", a genuinely useful question that lesson
27 covers. It is not the question "why was my system slow at 09:31:07", and reaching for a
microbenchmark harness on a tail problem is a category error: the harness runs the function
ten million times with everything warm, the exact opposite of the conditions that produced
the tail.

| Tool | Question it answers | Overhead | In production? | Granularity |
|---|---|---|---|---|
| Sampling profiler (lesson 28) | Where do most cycles go | Under 1% at 999 Hz | Yes, at a low rate | Function, statistical |
| Clang XRay (FDR mode) | What was the call sequence and stage durations | Tens of ns per instrumented call | Yes, if instrumentation is sparse | Function entry and exit, exact |
| Intel PT, magic-trace | Exactly which instructions ran before the trigger | Roughly 2 to 5%, Intel only | Yes, in snapshot mode | Instruction, exact |
| eBPF, bpftrace | What is the kernel doing to my process | Low for kernel probes, ~1 us per uprobe | Yes | Kernel event, exact |
| Custom ring tracer | How long did each of my stages take, on this message | Under 10 ns per point | Yes, always on | Points you chose, exact |
| Google Benchmark (lesson 27) | Is A faster than B in the steady state | Irrelevant, offline | No | One function, averaged |

Read the table as a sequence rather than a menu. Sampling narrows the search, the ring tracer
names the stage that owns the tail, XRay or PT says what happened inside that stage, and
eBPF says whether the answer was never in your process at all.

## Takeaways

- Sampling overhead scales with sample rate and yields a statistical profile; tracing
  overhead scales with event rate and yields an exact record. The tail lives in the record,
  never in the profile.
- XRay's patchable nop sleds keep an instrumented binary cheap when it is not recording, so
  one build ships to production. Use `xray-fdr`, flush on a breach, and instrument stage
  boundaries and never accessors.
- Intel PT records every branch in hardware for a few percent, and its snapshot mode hands
  you the instruction history of the slow event itself. Intel-only, large data rate, offline
  decode.
- `perf probe`, `perf trace` and `perf sched` are the tracing side of `perf`. Off-CPU
  analysis catches the thread you believed was spinning but was actually descheduled.
- eBPF sees everything below your process cheaply and in production, and everything inside
  your hot loop badly. Do not put a uprobe in a decoder.
- The tracer most desks run is 40 lines: a per-thread ring of TSC-stamped records, always on,
  under 10 ns a point. Its value is that the trace of the bad event already exists when you
  go looking.
