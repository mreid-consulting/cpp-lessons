---
title: Microbenchmarking Without Lying
part: Part IV - Latency Engineering
summary: The optimiser deletes the code you are timing, an unrelated edit moves the loop across a cache line and the number shifts, and the win never appears in production. How to build a benchmark you can believe.
time: 30 min
level: advanced
tags: benchmarking, google-benchmark, donotoptimize, noise, alignment
---

Almost every microbenchmark written by a competent engineer on their first attempt is
wrong, and it is wrong in the flattering direction. The compiler removes the work, the
cache stays hot in a way production never is, and the loop happens to land on a friendly
address. Lesson 26 covered how to measure a running system honestly. This lesson is about
the smaller and more treacherous problem of measuring one function.

## The benchmark that measured nothing

Here is a plausible attempt to time the notional-value calculation over a batch of fills.

```cpp bad_benchmark.cpp
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <print>
#include <vector>

std::int64_t sum_notional(const std::vector<std::int64_t>& px,
                          const std::vector<std::uint32_t>& qty) {
    std::int64_t total = 0;
    for (std::size_t i = 0; i < px.size(); ++i)
        total += px[i] * static_cast<std::int64_t>(qty[i]);
    return total;
}

int main() {
    const std::vector<std::int64_t>  px(4096, 10'025);
    const std::vector<std::uint32_t> qty(4096, 10);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1'000'000; ++i)
        sum_notional(px, qty);                  // return value discarded
    const auto t1 = std::chrono::steady_clock::now();

    std::print("{:.2f} ns/call\n",
               std::chrono::duration<double, std::nano>(t1 - t0).count() / 1e6);
}
```

At `-O2` this prints a number close to zero. The compiler can see the whole body of
`sum_notional`, observes that it touches no memory the caller can observe and that its
result is thrown away, and deletes the call. Then it deletes the loop. You have timed an
empty loop, drawn a conclusion, and shipped it to a code review.

The tell is that the number is implausible. Four thousand multiply-adds cannot take one
nanosecond. **Any microbenchmark result that is too good is a bug until proven otherwise**,
and the first check is always to look at the assembly, as in lesson 1, and confirm the
instructions you meant to measure are still there.

## Two barriers: DoNotOptimize and ClobberMemory

You need a way to lie to the optimiser: to claim that a value is observed by something
outside its view, and that memory has been read and written by something it cannot see.
Google Benchmark spells these `benchmark::DoNotOptimize` and `benchmark::ClobberMemory`.
Both are empty inline-assembly statements with carefully chosen constraints, and you can
write them yourself in about five lines.

```cpp bench_barriers.hpp GCC and Clang, any target
#pragma once

// Force `value` into memory and tell the compiler that unknown code has read
// and possibly modified it.
template <typename T>
[[gnu::always_inline]] inline void do_not_optimize(T& value) {
    asm volatile("" : "+m"(value) : : "memory");
}

// Tell the compiler that unknown code has read and written all of memory,
// which forces pending stores to be materialised before this point.
[[gnu::always_inline]] inline void clobber_memory() {
    asm volatile("" : : : "memory");
}
```

`do_not_optimize` is what you apply to a *result* so that the computation producing it
cannot be deleted. `clobber_memory` is what you apply after a *write* so that stores into
a buffer nobody later reads are not elided. A benchmark that builds a message into a
buffer needs the second; a benchmark that computes a checksum needs the first.

:::note
Google Benchmark's own version prefers the multi-alternative constraint `"+r,m"`, which
lets the compiler keep a small value in a register instead of spilling it. That form is
cheaper but not portable: GCC rejects it on AArch64 with `impossible constraint in 'asm'`,
and it cannot accept a type too large for a register. The `"+m"` version above always
compiles, at the cost of one store.
:::

```cpp The same measurement, this time of real work
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <vector>

static void BM_SumNotional(benchmark::State& st) {
    const std::vector<std::int64_t>  px(4096, 10'025);
    const std::vector<std::uint32_t> qty(4096, 10);

    for (auto _ : st) {
        std::int64_t total = 0;
        for (std::size_t i = 0; i < px.size(); ++i)
            total += px[i] * static_cast<std::int64_t>(qty[i]);
        benchmark::DoNotOptimize(total);        // the result escapes
    }
    st.SetItemsProcessed(st.iterations() * static_cast<std::int64_t>(px.size()));
}
BENCHMARK(BM_SumNotional);
BENCHMARK_MAIN();
```

:::warn
`DoNotOptimize` is not free and not neutral. Forcing a value into memory can inhibit
vectorisation of the loop that produced it. Apply it to the smallest thing that keeps the
work alive, usually a single accumulator outside the inner loop, never to every element.
:::

## Structure, and the knobs that matter

The `for (auto _ : st)` loop is not a normal loop. Google Benchmark runs the body an
increasing number of times until the total wall time exceeds a threshold, and reports the
mean per iteration. Three settings change the quality of the answer:

- `--benchmark_min_time=2s` raises how long each configuration runs. The default is short
  enough that a benchmark of a nanosecond-scale function is dominated by startup effects.
- `--benchmark_repetitions=15` reruns the whole thing and reports mean, median and
  standard deviation. Without repetitions you have one sample and no idea of its spread.
- `SetItemsProcessed` converts the result into a rate, which is the number you actually
  reason about for a decoder: messages per second rather than nanoseconds per loop.

```sh Running it so the numbers are comparable
$ g++ -std=c++23 -O2 -g bench.cpp -lbenchmark -lpthread -o bench
$ sudo cpupower frequency-set --governor performance          # Linux
$ taskset -c 7 ./bench --benchmark_min_time=2s \
                       --benchmark_repetitions=15 \
                       --benchmark_report_aggregates_only=true
```

A fixture is worth using as soon as setup costs more than the measurement, which for an
order book is immediately. Setup runs outside the timed region.

```cpp A fixture that pays the construction cost once
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

struct Quote { std::int64_t price_ticks; std::uint32_t qty; std::uint32_t level; };

class BookFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        quotes_.resize(1u << 16);
        for (std::size_t i = 0; i < quotes_.size(); ++i)
            quotes_[i] = Quote{10'000 + static_cast<std::int64_t>(i % 64),
                               100u, static_cast<std::uint32_t>(i % 64)};
    }
    std::vector<Quote> quotes_;
};

}  // namespace

BENCHMARK_F(BookFixture, ApplyQuotes)(benchmark::State& st) {
    std::int64_t best_bid = 0;
    for (auto _ : st) {
        for (const Quote& q : quotes_)
            if (q.price_ticks > best_bid) best_bid = q.price_ticks;
        benchmark::DoNotOptimize(best_bid);
    }
    st.SetItemsProcessed(st.iterations() *
                         static_cast<std::int64_t>(quotes_.size()));
}
BENCHMARK_MAIN();
```

For a quick comparison of two implementations, **quick-bench.com** compiles and runs a
Google Benchmark snippet in the browser and is ideal for settling an argument about two
one-liners. For a dependency-free option in your own repository, **nanobench** is a single
header, reports median and a coefficient of variation by default, and reads hardware
counters through `perf_event_open` on Linux. Neither replaces a measurement on the target
box, but both are better than a hand-rolled `steady_clock` loop.

## Throughput and latency are different numbers

Consider a benchmark of an order-book lookup that runs the lookup a million times in a
loop. If each lookup is independent, the CPU issues several of them at once: out-of-order
execution overlaps ten cache misses, and you measure how many lookups per second the
memory system can sustain. That is **throughput**. If your strategy does one lookup and
then immediately needs the answer, the number you care about is **latency**: how long one
lookup takes from address to result, with nothing to overlap it.

The two can differ by a factor of five or more on a pointer-chasing structure. To measure
latency you must build a **dependency chain**, where each iteration's input is the previous
iteration's output, so the hardware cannot start the next access early.

```cpp Latency and throughput of the same access pattern
#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace {

// next[i] gives the index visited after i, in a single random cycle.
std::vector<std::uint32_t> make_cycle(std::size_t n) {
    std::vector<std::uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::shuffle(order.begin(), order.end(), std::mt19937{1234});
    std::vector<std::uint32_t> next(n);
    for (std::size_t i = 0; i + 1 < n; ++i) next[order[i]] = order[i + 1];
    next[order[n - 1]] = order[0];
    return next;
}

void BM_Latency(benchmark::State& st) {
    const auto next = make_cycle(static_cast<std::size_t>(st.range(0)));
    std::uint32_t i = 0;
    for (auto _ : st) {
        i = next[i];                       // address depends on the last load
        benchmark::DoNotOptimize(i);
    }
}
BENCHMARK(BM_Latency)->Range(1 << 12, 1 << 24);

void BM_Throughput(benchmark::State& st) {
    const auto next = make_cycle(static_cast<std::size_t>(st.range(0)));
    const std::uint32_t n = static_cast<std::uint32_t>(next.size());
    std::uint32_t a = 0, b = n / 4, c = n / 2, d = 3 * n / 4;
    for (auto _ : st) {
        a = next[a]; b = next[b]; c = next[c]; d = next[d];   // independent
        benchmark::DoNotOptimize(a); benchmark::DoNotOptimize(b);
        benchmark::DoNotOptimize(c); benchmark::DoNotOptimize(d);
    }
}
BENCHMARK(BM_Throughput)->Range(1 << 12, 1 << 24);

}  // namespace

BENCHMARK_MAIN();
```

At sizes that fit in L1 the two are close. Once the working set exceeds the last-level
cache, the latency variant on a typical modern server core reports something in the region
of 80 to 120 ns per step while the throughput variant reports roughly a quarter of that
per step, because four misses are in flight at once. Quote whichever matches how your code
actually consumes the result.

## Warmup, cold caches, and what you are really warming

Google Benchmark discards early iterations implicitly by growing the iteration count, but
it is worth knowing what those iterations achieve. They populate the L1 instruction cache
with your hot loop, the data cache and the data TLB with your working set, the branch
predictor with your branch history, and they give the CPU time to ramp from its idle
frequency to its turbo frequency. On a laptop the frequency ramp alone can make the first
milliseconds 40% slower than the steady state.

That is exactly why a warm microbenchmark can be a lie. If a quote for your instrument
arrives every 200 microseconds, then by the time it arrives your decoder's code has been
evicted from L1i by everything else that ran, and your book's hot levels have been evicted
from L1d. The production access pattern is **cold**, and a benchmark that runs the decoder
back to back ten million times measures a condition your system is never in.

```cpp Evicting a buffer before each timed iteration (x86-64, Linux)
#include <benchmark/benchmark.h>
#include <emmintrin.h>          // _mm_clflush, _mm_mfence
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

void evict(const void* p, std::size_t bytes) {
    const auto* c = static_cast<const char*>(p);
    for (std::size_t i = 0; i < bytes; i += 64) _mm_clflush(c + i);
    _mm_mfence();
}

void BM_DecodeCold(benchmark::State& st) {
    std::vector<std::uint8_t> packet(256, 0x2A);
    std::int64_t acc = 0;
    for (auto _ : st) {
        st.PauseTiming();
        evict(packet.data(), packet.size());
        st.ResumeTiming();
        for (std::uint8_t b : packet) acc += b;
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_DecodeCold);

}  // namespace

BENCHMARK_MAIN();
```

:::pitfall
`PauseTiming`/`ResumeTiming` cost on the order of hundreds of nanoseconds per call and
themselves disturb the caches. They are usable when the timed region is microseconds, and
useless when it is nanoseconds. For nanosecond-scale work, batch: flush once, then time a
run of N cold-ish operations and divide.
:::

## Noise you cannot remove, and the discipline that follows

Add an unused function to a translation unit. The linker's layout shifts by 16 bytes, your
hot loop now straddles a 32-byte instruction-fetch boundary instead of sitting inside one,
and the benchmark reports a result several percent different with no change to the code
being measured. This is **code alignment noise**, and it is well documented: measurements
of the same binary built with different, semantically irrelevant, changes routinely move
by more than the effect sizes people publish.

Frequency is the second source. Turbo boost raises the clock when few cores are busy and
drops it as the package heats, so a benchmark run for thirty seconds can be measurably
slower at the end than at the start. AVX-512 workloads on some server parts drop the
frequency further still. And a shared machine gives you a neighbour's cache traffic for
free.

The discipline that follows is not "eliminate the noise", which is impossible, but:

- Pin to an isolated core with `taskset -c <cpu>`, on a core the kernel has been told to
  leave alone (lesson 30 covers `isolcpus` and friends).
- Fix the frequency governor to `performance` rather than trusting turbo, and prefer a
  fixed clock over the highest clock.
- Run repetitions and quote the **median**, not the mean. One preempted iteration ruins a
  mean and barely moves a median.
- Rebuild and rerun on a different day before believing a change. If a 3% improvement does
  not reproduce across builds, it was alignment.
- Treat anything under about 5% on a nanosecond-scale benchmark as unmeasured, unless you
  have counter evidence from lesson 28 explaining the mechanism.

:::hft
A microbenchmark result is a hypothesis, not a result. A branchless order-book update that
benchmarks 12 ns against 19 ns for the branchy version has told you something real about
the function in isolation. It has told you nothing about tick-to-trade, where the branchy
version's mispredict may overlap with a cache miss that dominates both, or where the
branchless version's extra register pressure spills something in the caller. **The rule on
a desk is that a microbenchmark win must be confirmed end to end, on the wire-to-wire
histogram, before anyone believes it.** Many candidate optimisations that win by 30% in
isolation move the p99 of the full path by nothing at all.
:::

:::exercise
Take the `BM_SumNotional` benchmark above and remove the `DoNotOptimize` call. Build both
versions with `-O2 -g`, run each with `--benchmark_repetitions=15`, and compare. Then
disassemble both with `objdump -d --disassemble=_ZL15BM_SumNotional...` and confirm that
the version without the barrier contains no `imul` in the loop body. Finally, add an
unused 200-byte function to the top of the file, rebuild, and see how far the *correct*
version's median moves. That last number is your noise floor.
:::

## Takeaways

- If the result is implausibly good, the optimiser deleted the work. Check the assembly
  before you check anything else.
- `DoNotOptimize` keeps a computed value alive; `ClobberMemory` keeps stores alive. Apply
  the smallest barrier that works, because barriers themselves inhibit optimisation.
- Independent iterations measure throughput; a dependency chain measures latency. Decide
  which one your caller experiences and build the benchmark to match.
- Warm benchmarks flatter cold code paths. If production touches your decoder every
  200 microseconds, benchmark it cold.
- Alignment, turbo and thermal drift put a floor of a few percent under every number.
  Repeat across builds, quote medians, and disbelieve small deltas.
- A microbenchmark win is a hypothesis until the end-to-end histogram confirms it.
