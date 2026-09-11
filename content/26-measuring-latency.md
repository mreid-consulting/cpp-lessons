---
title: Measuring Latency Honestly
part: Part IV - Latency Engineering
summary: Clocks, calibration, histograms and percentiles; coordinated omission, hardware timestamping, and why a 2% improvement is usually noise.
time: 35 min
level: advanced
tags: rdtsc, percentiles, histogram, coordinated-omission, ptp
---

Everything in the previous six lessons was a claim about performance. This lesson is how you
find out whether the claims are true on your machine, with your data. It is the least
glamorous skill in low-latency engineering and the one that separates people who make systems
faster from people who make systems different.

Most latency measurement in the wild is wrong in one of four specific ways: it reports a mean,
it uses a clock that costs more than the thing being timed, it allocates or logs inside the
measurement, or it suffers coordinated omission. Each of those makes a slow system look fast.

## Deciding what to measure

Start with the number the business cares about: **tick-to-trade, wire to wire**, from the first
bit of the market-data packet arriving at your network interface to the first bit of the
resulting order leaving it. That is what decides whether you are at the front of the queue.

Your process cannot see either endpoint. It sees the packet after the driver or bypass stack
hands it over, and the order before the card serialises it. So you need two measurements:

- **External, wire to wire.** Hardware timestamps at the NIC, or a passive tap. This is the
  truth, and the only number worth quoting outside the team.
- **Internal, stage by stage.** Timestamps between parse, book update, decision and encode.
  These do not say how fast you are; they say where the time went, which is what you improve.

```cpp stages.hpp
#pragma once
#include <cstdint>

// One record per message. 32 bytes, written into a preallocated ring.
// Deltas are computed offline; the hot path only ever stores raw ticks.
struct StageStamps {
    std::uint64_t rx_tsc;        // first instruction after the packet is available
    std::uint32_t d_parse;       // ticks from rx to end of decode
    std::uint32_t d_book;        // ticks from end of decode to book updated
    std::uint32_t d_decide;      // ticks from book updated to decision made
    std::uint32_t d_encode;      // ticks from decision to order bytes ready
    std::uint32_t seq;           // message sequence number, for correlation
    std::uint32_t pad;
};
```

Two rules govern the instrumentation. **Store raw ticks and subtract offline**: a division to
convert to nanoseconds inline is 20-plus cycles added to the thing you are measuring. And
**32-bit deltas are enough**: at 3 GHz a `std::uint32_t` covers 1.4 seconds.

:::key
A probe that costs 20 ns is fine around a 5 µs stage and useless around a 30 ns one. Before
you trust any stage timing, measure the probe's own cost and compare it to the interval. If the
probe is more than about 5 percent of what you are timing, measure a coarser boundary or use a
sampling profiler instead (lesson 28).
:::

## The clocks you actually have

### `rdtsc` and `rdtscp`

`rdtsc` reads the CPU's time-stamp counter into `edx:eax`. It is not a syscall, it takes
roughly 15 to 25 cycles on a modern x86 server core, and it is the only clock cheap enough to
put around a sub-microsecond interval.

Two properties you must confirm on your hardware:

- **Invariant TSC.** On every server part since roughly Nehalem, the TSC increments at a
  constant rate regardless of the core's actual frequency, P-state or C-state. Check
  `constant_tsc` and `nonstop_tsc` in `/proc/cpuinfo` flags. Without it, TSC deltas are
  meaningless because the tick rate changes underneath you.
- **Synchronised across cores.** On a single-socket machine the TSCs are normally synchronised
  at reset. Across sockets they may not be, so a timestamp taken on socket 0 and subtracted
  from one taken on socket 1 can be negative. Pin the measured path to one socket, which you
  wanted to do anyway (lesson 35).

```cpp tsc.hpp
#pragma once
#include <cstdint>
#include <x86intrin.h>

// Raw read. May be reordered by the out-of-order engine with surrounding
// instructions, in either direction. Fine for coarse intervals.
[[gnu::always_inline]]
inline std::uint64_t tsc_raw() noexcept { return __rdtsc(); }

// rdtscp waits for all prior instructions to RETIRE before reading, and
// returns the core id in aux. It does NOT stop later instructions from
// starting early, so it fences one side only.
[[gnu::always_inline]]
inline std::uint64_t tsc_end() noexcept {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();                 // now nothing after this can float above it
    return t;
}

// Fully serialised read, for the START of a short interval: lfence blocks
// later instructions from executing before it. Costs more than plain rdtsc.
[[gnu::always_inline]]
inline std::uint64_t tsc_begin() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
```

This is the part people get wrong. `rdtsc` is not a barrier. On an out-of-order core it can
execute before instructions that appear above it and after instructions that appear below it,
so a naive `t0 = rdtsc(); work(); t1 = rdtsc();` can report a delta that excludes part of
`work()` or includes part of its neighbours. Use `tsc_begin` before and `tsc_end` after when
the interval is short. For intervals of many microseconds the reordering window is negligible
and plain `__rdtsc()` is fine — and cheaper, which matters when you are recording per message.

### `clock_gettime` and `std::chrono`

`clock_gettime(CLOCK_MONOTONIC, ...)` on Linux does not enter the kernel. It resolves through
the **vDSO**, a page the kernel maps into every process holding the clock code and the current
TSC-to-time conversion parameters. So it is a call plus an `rdtsc` plus a multiply-shift:
roughly **20 to 25 ns** on a typical modern server, against 6 to 8 ns for a bare `rdtsc`.

`std::chrono::steady_clock::now()` on libstdc++ wraps exactly that and costs the same. Its
`period` advertises nanoseconds, but observed resolution is governed by the call's own cost —
two consecutive calls never differ by less than about 20 ns. `high_resolution_clock` is an
alias for one of the other two, not a distinct guarantee; never use it, and never use
`system_clock` for durations, because NTP steps it.

| Clock | Typical cost | Reorders? | Use for |
|---|---|---|---|
| `__rdtsc()` | 6 to 8 ns | yes | per-message stamps, coarse intervals |
| `lfence`-fenced `rdtsc` | 15 to 30 ns | no | short, precise intervals |
| `clock_gettime` via vDSO | 20 to 25 ns | no | anything at microsecond scale or above |
| `steady_clock::now()` | same as above | no | portable code, calibration |
| `system_clock::now()` | same as above | no | wall-clock logging only, never durations |

Figures are for a typical 3 GHz x86 server with a modern kernel; measure yours.

### Calibrating ticks to nanoseconds

TSC ticks are not nanoseconds, and the nominal frequency in the CPU model name is not
necessarily the TSC rate. Calibrate once at startup against a clock that is defined in real
time, and keep the factor as a `double` used only offline.

```cpp calibrate.cpp
#include <chrono>
#include <cstdint>
#include <thread>
#include "tsc.hpp"

// Ticks per nanosecond. Call once at startup, on a quiet core.
// 200 ms gives ~1e-5 relative error against a vDSO clock; longer is better.
double calibrate_tsc_per_ns() noexcept {
    using clock = std::chrono::steady_clock;
    const auto  w0 = clock::now();
    const auto  c0 = tsc_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto  c1 = tsc_end();
    const auto  w1 = clock::now();
    const double ns = std::chrono::duration<double, std::nano>(w1 - w0).count();
    return static_cast<double>(c1 - c0) / ns;
}
```

On Linux you can cross-check against the kernel's own value: `dmesg | grep -i "tsc:.*MHz"`, or
read `/sys/devices/system/cpu/cpu0/tsc_freq_khz` where the kernel exposes it.

### What one tick is actually worth

Calibration gives you a conversion factor. It also, read the other way round, gives you a
floor, and that is the half people skip. One tick is the finest difference the counter can
express, so any delta smaller than a few ticks is quantisation rather than signal.

The number varies by more than an order of magnitude across machines you will meet:

| Counter | Typical rate | One tick |
|---|---|---|
| `rdtsc`, x86-64 server | 2 to 3 GHz nominal | 0.3 to 0.5 ns |
| `cntvct_el0`, Apple silicon | 24 MHz | 41.7 ns |
| `cntvct_el0`, some ARM servers | 50 to 100 MHz | 10 to 20 ns |
| `clock_gettime` via vDSO | ~20 to 25 ns per call | its own cost |

On x86-64 a tick is a fraction of a nanosecond and you can time a single pipeline stage.
On Apple silicon a tick is 41.7 ns, which is longer than an entire decode stage, so the
same instrumentation reports most stages as zero. That is not a fast pipeline, it is an
unusable ruler.

Measure it rather than assuming it. The smallest non-zero gap between two back-to-back
reads is the counter's practical granularity:

```cpp tick_resolution.cpp
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>

// From tsc.hpp above; reads cntvct_el0 rather than rdtsc on ARM.
std::uint64_t tsc_raw() noexcept;

// Two reads back to back differ by either zero or one step of the counter.
// The smallest non-zero difference is the step.
std::uint64_t tick_granularity(int samples = 10'000) noexcept {
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (int i = 0; i < samples; ++i) {
        const std::uint64_t a = tsc_raw();
        const std::uint64_t b = tsc_raw();
        if (b > a) best = std::min(best, b - a);
    }
    return best == std::numeric_limits<std::uint64_t>::max() ? 0 : best;
}

void report_resolution(double tsc_per_ns) noexcept {
    const double ns_per_tick = 1.0 / tsc_per_ns;
    const std::uint64_t step = tick_granularity();
    std::printf("counter %.1f MHz, %.3f ns/tick, step %llu ticks = %.1f ns\n",
                tsc_per_ns * 1000.0, ns_per_tick,
                static_cast<unsigned long long>(step),
                static_cast<double>(step) * ns_per_tick);
}
```

Print that line at startup, next to the histogram, in anything that reports timings. It
costs one line of output and it is the difference between a reader trusting your numbers
and a reader being misled by them.

:::warn
A measured median of zero never means free. It means below the resolution of the
instrument. The same is true of a histogram whose first bucket is narrower than a tick:
every sample lands in it and the distribution looks impossibly tight. Size the bucket
width to at least a few ticks, and say in the output what the tick is.
:::

## Recording samples without perturbing them

The measurement must not do anything the hot path is forbidden to do. That rules out
allocating, locking, formatting a string, writing to a file, and calling into a logging
framework. Two shapes work.

**A preallocated sample array**, when you can bound the run.

```cpp samples.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// 4 MB of samples, touched at startup, never grown. Dumped after the run.
class SampleBuffer {
public:
    static constexpr std::size_t kCapacity = 1u << 20;

    [[gnu::always_inline]]
    void record(std::uint32_t ticks) noexcept {
        if (n_ < kCapacity) [[likely]] s_[n_++] = ticks;
        else ++dropped_;
    }
    std::size_t size()    const noexcept { return n_; }
    std::size_t dropped() const noexcept { return dropped_; }
    const std::uint32_t* data() const noexcept { return s_.data(); }

private:
    std::array<std::uint32_t, kCapacity> s_{};   // value-init: pre-faulted at load
    std::size_t n_ = 0;
    std::size_t dropped_ = 0;
};
```

Every sample is kept, so any percentile is exact and re-analysable later. The cost is one
store, around 1 ns on a hot line. Pre-fault the array at startup as lesson 24 describes.

**A fixed-bucket histogram**, when the run is unbounded.

```cpp histogram.hpp
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Linear buckets. Choose NsPerBucket so the interesting region has
// resolution and the tail lands in the last, saturating bucket.
template <std::size_t NBuckets, std::uint64_t NsPerBucket>
class Histogram {
public:
    [[gnu::always_inline]]
    void record(std::uint64_t ns) noexcept {
        const std::size_t b = static_cast<std::size_t>(
            std::min<std::uint64_t>(ns / NsPerBucket, NBuckets - 1));
        ++counts_[b];
        ++n_;
        if (ns > max_) max_ = ns;      // exact max, never bucketed away
    }

    // Upper edge of the bucket containing the p-th percentile, p in [0,1].
    std::uint64_t percentile(double p) const noexcept {
        if (n_ == 0) return 0;
        const std::uint64_t target = static_cast<std::uint64_t>(p * static_cast<double>(n_));
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < NBuckets; ++i) {
            cum += counts_[i];
            if (cum >= target) return (i + 1) * NsPerBucket;
        }
        return max_;
    }

    std::uint64_t max()   const noexcept { return max_; }
    std::uint64_t count() const noexcept { return n_; }

private:
    std::array<std::uint64_t, NBuckets> counts_{};
    std::uint64_t n_ = 0;
    std::uint64_t max_ = 0;
};

// 4096 buckets of 10 ns each: 10 ns resolution up to 40.96 µs, then saturating.
using TickToTradeHist = Histogram<4096, 10>;
```

`record` is a divide, a compare, and two increments. The divide by a compile-time constant
becomes a multiply-and-shift, so this is under 5 ns on a typical modern core. Keeping the exact
maximum separately matters: the saturating top bucket would otherwise hide your worst case,
which is the sample you most need to see.

For a range spanning several orders of magnitude, use logarithmic buckets or HdrHistogram,
which stores a fixed number of significant digits per magnitude and gives bounded relative
error across the whole range.

### Subtracting the measurement overhead

```cpp overhead.cpp
#include <algorithm>
#include <cstdint>
#include "tsc.hpp"

// The MINIMUM of many back-to-back reads, not the mean. The mean includes
// interrupts and migrations; the minimum is the pure instruction cost.
std::uint64_t probe_overhead_ticks() noexcept {
    std::uint64_t best = ~std::uint64_t{0};
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t a = tsc_begin();
        const std::uint64_t b = tsc_end();
        best = std::min(best, b - a);
    }
    return best;
}
```

Subtract that constant from each interval. Two caveats: the probe also perturbs the pipeline in
ways a constant cannot capture, and once you are subtracting more than about 20 percent of the
measured value you need a coarser boundary rather than better arithmetic.

## Reading the distribution: percentiles, not means

The mean latency of a trading system is a number with no operational meaning. Latency
distributions are heavily right-skewed: a tight body around the common case, and a tail from
cache misses, page faults, interrupts, scheduling and NUMA effects. A mean blends those two
populations into a value describing neither. Two systems with identical means can have maximums
differing by a factor of a hundred, and it is the maximum that costs you the trade.

Report exactly these, always together:

| Statistic | What it tells you |
|---|---|
| p50 | the common case; whether your algorithm is right |
| p99 | the ordinary bad case; usually cache and branch behaviour |
| p99.9 | the systemic bad case; page faults, interrupts, scheduling |
| max | the worst thing that happened; the only bound you can promise |
| count | without it, none of the above means anything |

The sample-count question is the one people skip. A percentile is estimated from the samples
*above* it, and you need enough of those for the estimate to be stable. A working rule: aim for
at least **100 samples beyond the percentile you are quoting**.

| Percentile | Samples for ~1 beyond | Samples for ~100 beyond |
|---|---|---|
| p99 | 100 | 10,000 |
| p99.9 | 1,000 | 100,000 |
| p99.99 | 10,000 | 1,000,000 |

So a p99.9 quoted from a 10,000-sample run rests on ten observations and will move by tens of
percent between runs. Quote p99.9 from a million samples or do not quote it. And run long
enough to capture the slow events you care about: if a garbage-collection-like effect happens
once a minute, a thirty-second run has a fifty percent chance of not seeing it at all.

:::hft
Report the distribution the same way every time, in the same units, with the count attached:
"p50 1.8 µs, p99 3.1 µs, p99.9 9.4 µs, max 61 µs over 4.2 M messages, session 2026-09-09". A
single number in an email is not a measurement, it is an opinion. The max is the one the risk
committee will ask about, because a 61 µs outlier during a volatility spike is a missed fill
and possibly an unhedged position, and no amount of good p50 compensates for it.
:::

## Coordinated omission

This is the failure mode that makes a broken system look excellent, and it is subtle enough
that it survives in production measurement code for years.

The setup: a load generator or a measurement harness that issues one request, waits for the
response, records the latency, and then issues the next. It is a **closed loop**. When the
system stalls, the harness stalls with it, and stops issuing the requests that would have
experienced the stall. It has coordinated with the system under test to omit the bad samples.

Make it concrete. You intend to send one message every 1 ms for 10 seconds: 10,000 messages.
The system normally responds in 100 µs, but at second five it stalls for 1 full second.

- **Closed loop, as measured.** For one second the harness sends nothing, because it is waiting
  on the one outstanding request. It records that single request at 1,000,000 µs and then
  resumes. Total recorded: about 9,001 samples, one of them terrible.
  - p50 is 100 µs, p99 is 100 µs, p99.9 is 100 µs. **The stall is invisible above p99.9.**
- **What actually happened to the market.** 1,000 messages were due during that stall. The
  first would have waited the full second, the next 999 ms, and so on down to 1 ms. Every one
  of them was late.
  - Corrected: p50 is 100 µs, p99 is about 900 ms, p99.9 is about 990 ms, max is 1 s.

The same illusion, minus one number: **one sample says 1 second and one thousand samples say
100 µs**, when the truth is that a thousand messages were delayed by an average of half a
second.

Three fixes, in order of preference.

1. **Measure against the intended send time, not the actual one.** For a message that was
   scheduled at time `T` and completed at `C`, record `C - T`, not `C - actual_start`. In a
   trading system this is easy and correct, because market data arrives when the market says so
   and its arrival timestamp *is* the intended start.
2. **Use an open-loop generator.** Send at a fixed rate from a separate thread or machine that
   does not wait for responses, and let the queue build if the system falls behind.
3. **Compensate after the fact.** HdrHistogram's `recordValueWithExpectedInterval` synthesises
   the omitted samples given an expected interval. It is an estimate, but it is far better than
   pretending the stall did not happen.

:::warn
Real market data is a natural open loop: packets arrive whether or not you have finished
processing the last one. That makes trading systems easier to measure honestly than most
software — but only if you timestamp at packet arrival. Timestamp at "when my handler started"
and you have rebuilt coordinated omission by hand, because a handler that starts late has
already hidden its own queueing delay.
:::

## What your process cannot see, and how to compare fairly

### Hardware timestamping and PTP

Between the wire and your first instruction sit the PHY, the driver or bypass stack, and a
scheduling decision; between your last instruction and the wire sit the transmit path and the
serialiser. Software timestamps miss all of it, and on a badly tuned box that hidden portion
can exceed everything you have optimised.

The instrument is the NIC. Modern cards timestamp packets in hardware at reception with
resolution in the tens of nanoseconds, exposed through `SO_TIMESTAMPING` with
`SOF_TIMESTAMPING_RX_HARDWARE` on Linux, or the equivalent in a bypass stack. Transmit
timestamps come back on the socket's error queue.

For those timestamps to be comparable across machines the clocks must be disciplined to a
common source. That is **PTP**, IEEE 1588: a grandmaster clock, boundary or transparent
switches that correct for their own delay, and `ptp4l` plus `phc2sys` on the host to steer the
NIC clock and then the system clock. On hardware that supports it end to end, PTP holds hosts
within tens to hundreds of nanoseconds of each other, against NTP's tens of microseconds.

The gold standard is a passive tap feeding a capture appliance that timestamps in hardware and
sees both the inbound packet and your outbound order. It includes everything and is independent
of your code, which is why it is the number to quote.

### A/B comparison discipline

You changed something. Is it faster?

The default answer is no. **A 2 percent difference is noise** unless you controlled for these,
and most of them are not about your code at all:

- **Code layout.** An unrelated added function shifts every address after it, changing cache
  set assignment, loop alignment and branch aliasing. Swings of 5 to 10 percent from layout
  alone are well documented; lesson 29 covers controlling them.
- **CPU frequency.** Fix the governor to `performance` and consider disabling turbo, so a warm
  run is not compared against a cold one.
- **Environment size.** The environment block sits on the stack and shifts every stack address,
  so a different shell or CI runner genuinely changes results.
- **Which cores.** Pin to the same isolated cores, on the NIC's NUMA node, every time.
- **Order of runs.** Interleave A and B rather than all of A then all of B, so drift in machine
  state cannot masquerade as a difference.

Run each variant at least five times, compare **distributions** rather than single numbers, and
call the change real only when p50 shifts by more than the run-to-run spread of either variant.
For statistical rigour use a Mann-Whitney U test, which assumes no normality — and latency
distributions are emphatically not normal.

:::exercise
Instrument a simple message handler with the `StageStamps` record and the histogram from this
lesson, then work through the following.

1. Measure `probe_overhead_ticks()` on your machine, with and without the `lfence` pair.
   Compare against the roughly 6 to 8 ns and 15 to 30 ns figures quoted above, and calibrate
   ticks to nanoseconds so you can state the result in real units.
2. Feed one million messages through the handler and report p50, p99, p99.9 and max. Then
   re-run with only 10,000 messages, five times, and record how much your p99.9 moves between
   runs. Confirm the sample-count table empirically.
3. Build coordinated omission deliberately: add a closed-loop driver that waits for each
   response, and inject a 100 ms stall every 10,000 messages using a sleep. Report the
   percentiles both ways, closed loop and timestamped against the intended send time. The gap
   between the two p99.9 figures is the size of the lie.
4. Make a change you are confident does nothing — reorder two independent statements, or add an
   unused function — rebuild, and measure. Whatever difference you see is your noise floor, and
   no future improvement smaller than it is real.
:::

## Takeaways

- Measure tick-to-trade wire to wire for the number you quote, and per-stage internally for the
  number you act on. Store raw ticks on the hot path and convert offline.
- `rdtsc` is 6 to 8 ns and reorders; fence it with `lfence` for short intervals.
  `clock_gettime` through the vDSO is 20 to 25 ns and is the right tool above the microsecond
  scale. Confirm invariant TSC and calibrate ticks to nanoseconds at startup.
- Record into a preallocated array or a fixed-bucket histogram. Measurement code obeys the same
  no-allocation, no-logging rules as the path it measures, and keeps the exact maximum.
- The mean is meaningless for a right-skewed distribution. Report p50, p99, p99.9, max and the
  sample count, and do not quote a p99.9 from fewer than about 100,000 samples.
- Coordinated omission makes a stalled system look fast by not sending the requests that would
  have been slow. Time from the intended start, or use an open loop.
- NIC hardware timestamps plus PTP are the only way to see the path outside your process, and a
  passive tap is the number to quote externally. Inside, control code layout, CPU frequency,
  core pinning and run order before believing an A/B result: below a few percent, assume noise
  until the distributions say otherwise.
