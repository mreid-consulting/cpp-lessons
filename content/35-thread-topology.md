---
title: Cores, Pinning and Spinning
part: Part V - Concurrency
summary: Read the machine's real topology, put each thread where it belongs, and never let the hot one sleep. Plus the startup ritual that proves you got it right before the open.
time: 30 min
level: expert
tags: affinity, numa, smt, spinning, topology
---

You can write a perfect wait-free queue and still lose to a competitor whose code is worse,
because they put their two threads on cores that share an L2 and you let the scheduler put
yours on different sockets. Placement is not tuning you do at the end. It is part of the
design, and it starts with knowing what your machine actually looks like, which is almost
never what the core count suggests.

## Reading the machine

A "32-core" server might be 16 physical cores with SMT, or 32 real cores across two sockets,
or one socket in sub-NUMA clustering mode presenting two NUMA nodes. The logical CPU numbers
the kernel gives you are enumeration order, not a map. Never assume CPU 0 and CPU 1 are
neighbours; on many x86 servers they are, and on many others CPU *n* and CPU *n+cores* are
the two SMT threads of one physical core.

```sh
$ lscpu -e=CPU,NODE,SOCKET,CORE,CACHE
CPU NODE SOCKET CORE L1d:L1i:L2:L3
  0    0      0    0 0:0:0:0
  1    0      0    1 1:1:1:0
 ...
 24    1      1    0 24:24:24:1
 ...
 48    0      0    0 0:0:0:0          <- same CORE as CPU 0: SMT sibling
```

Two logical CPUs with the same `CORE` and `SOCKET` are SMT siblings sharing one physical
core's L1 and execution units. Equal `L3` index means shared last-level cache. Different
`NODE` means a memory access from one to the other's memory crosses the interconnect.

The authoritative source is sysfs, and it is what you should read from code rather than
parsing `lscpu` output:

```sh
$ cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
0,48
$ cat /sys/devices/system/cpu/cpu0/topology/core_siblings_list      # same socket
0-23,48-71
$ cat /sys/devices/system/cpu/cpu0/cache/index2/shared_cpu_list     # L2
0,48
$ cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list     # L3
0-23,48-71
$ cat /sys/devices/system/node/node0/cpulist
0-23,48-71
$ cat /sys/devices/system/cpu/cpu0/cpuidle/state2/latency           # C-state exit, us
133
```

For anything programmatic, use **hwloc**. `lstopo --of console` prints the whole hierarchy
including cache sizes, and `hwloc-calc` converts between logical and OS indices so your
config file does not have to hard-code kernel enumeration:

```sh
$ lstopo --of console
$ hwloc-calc --intersect PU core:3            # which OS CPUs make up physical core 3
$ hwloc-bind core:3.pu:0 -- ./trader          # launch already bound
```

## The thread map

A trading process has three temperature zones, and they want different placement.

| Thread | Behaviour | Placement |
|---|---|---|
| Network / feed | busy-polls the NIC, decodes, pushes to a queue | Dedicated physical core, isolated, same NUMA node as the NIC |
| Strategy | spins on the queue, prices, decides, pushes orders | Dedicated physical core sharing L2 or at least L3 with the feed core |
| Order sender | drains the order queue, writes to the NIC | Dedicated core near the NIC, or merged into strategy |
| Logging, telemetry, admin | blocking, allocating, occasionally slow | Anywhere *except* the isolated cores. Let the scheduler have it. |

Three rules make the table fall out.

**Give each hot thread a whole physical core, not a logical one.** If you pin the feed thread
to CPU 0 and something else lands on CPU 48, they share execution ports, the L1 and the
uop cache. The usual practice is to isolate the whole physical core, running the hot thread
on one sibling and leaving the other idle, or disabling SMT entirely in firmware. Some desks
deliberately co-schedule a *cooperating* pair on siblings to exploit the shared L1; that only
works if the pair genuinely shares data and never both saturate the same port.

**Put communicating threads close.** The queue from lesson 33 hands a cache line from one core
to another, and the cost of that transfer is entirely a function of distance:

| Handoff | Typical one-way latency |
|---|---|
| SMT siblings, shared L1 | 15 to 25 ns |
| Two cores sharing an L2 | 25 to 40 ns |
| Same socket, shared LLC only | 30 to 80 ns |
| Across sockets over UPI or Infinity Fabric | 130 to 350 ns |

Those are typical modern-x86-server figures, not guarantees. Measure yours; the point is the
ratio, which is roughly an order of magnitude from best to worst. A cross-socket handoff on
your tick-to-trade path can easily be a larger cost than your entire decode stage.

**Keep the cold threads off the hot cores.** Isolation is what makes this stick: `isolcpus`
and `nohz_full` on the kernel command line, plus `irqbalance` disabled and IRQ affinity
steered away from the isolated set. That is lesson 30's territory; this lesson assumes it.

## Pinning, and verifying it

```cpp affinity.hpp
#pragma once
#include <pthread.h>
#include <sched.h>
#include <cstdio>

// Linux. g++ defines _GNU_SOURCE, so pthread_setaffinity_np is visible.
inline bool pin_to_cpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

inline bool pin_and_verify(int cpu) noexcept {
    if (!pin_to_cpu(cpu)) return false;
    ::sched_yield();                       // let the migration take effect
    const int actual = ::sched_getcpu();
    if (actual != cpu) {
        std::fprintf(stderr, "pin: wanted cpu %d, running on %d\n", cpu, actual);
        return false;
    }
    return true;
}
```

Setting affinity can succeed while leaving you somewhere else, if the mask was already
restricted by a cgroup or by `taskset` on the parent. Always read back `sched_getcpu()`.
A silently unpinned hot thread is the single most common cause of "the latency was fine in
staging".

## Spin, sleep, or block

The hot thread never sleeps. Not "should rarely sleep": never.

The reason is the wakeup path. A `std::condition_variable::wait` that actually blocks parks
the thread in the kernel via futex. Waking it costs a syscall, a scheduler enqueue, usually
an inter-processor interrupt, and a context switch, a typical total in the range of 2 to
10 microseconds. Then, if the core went idle, it has fallen into a C-state, and the exit
latency for a deep one is published in sysfs and is typically tens to low hundreds of
microseconds. Against a decode-and-decide budget measured in hundreds of nanoseconds, a
single sleep-wake cycle is not a regression, it is a different sport.

So the hot thread spins. It burns a core continuously, at full power, doing nothing useful
most of the time. That is the trade: one core's worth of electricity and heat, in exchange
for never paying the wakeup path. It is obviously correct for a feed handler and obviously
absurd for a log writer, and the interesting cases are in between.

```cpp Politely
#include <atomic>
#include <cstdint>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();                          // or __builtin_ia32_pause()
#elif defined(__aarch64__)
    asm volatile("isb" ::: "memory");
#endif
}

// Hot path: spin forever, never yield, never sleep.
template <typename Poll>
void run_hot(std::atomic<bool>& running, Poll poll) {
    while (running.load(std::memory_order_relaxed)) {
        if (!poll())          // returns true if it did work
            cpu_relax();
    }
}
```

`pause` earns its place three times over. It tells the core this is a spin loop, which avoids
the memory-order-violation pipeline flush that would otherwise hit when the awaited store
finally lands. It yields front-end and issue resources to the SMT sibling, so a bare
`while (!flag) {}` loop can slow a co-scheduled sibling by a large factor. And it drops power
draw, which on a machine with any turbo headroom left keeps neighbouring cores' frequency up.
On Skylake and later, `pause` takes roughly 140 cycles rather than the ~10 of earlier
generations, so do not put ten of them in a row.

For everything that is not on the critical path, bounded spinning is the right compromise:
spin for as long as a wakeup would have cost, then block.

```cpp Spin briefly, then hand the core back
#include <atomic>
#include <condition_variable>
#include <mutex>

class Waiter {
public:
    void signal() {
        ready_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(m_);
        cv_.notify_one();
    }

    void wait(int spins) {
        for (int i = 0; i < spins; ++i) {
            if (ready_.load(std::memory_order_acquire)) return;
            cpu_relax();
        }
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return ready_.load(std::memory_order_acquire); });
    }

private:
    std::atomic<bool> ready_{false};
    std::mutex m_;
    std::condition_variable cv_;
};
```

Pick `spins` so the spin phase costs about what a block-and-wake would: a few thousand
iterations is the usual starting point, then measure. Never apply this to the feed thread.

## Thread per core

Once every hot thread owns a physical core and never sleeps, the natural architecture is
**thread-per-core**: shard the state so that each core owns a disjoint slice of it
exclusively, and move work between cores only as messages over the SPSC queues from lesson
33. No shared mutable state means no locks, no atomics on the data, and no coherence traffic
except the queue lines themselves.

```cpp Sharded, message-passing pipeline
#include <array>
#include <cstdint>
#include "spsc_queue.hpp"      // lesson 33

struct Tick { std::uint64_t recv_ns; std::uint32_t instrument_id;
              std::uint32_t qty;     std::int64_t  price_ticks; };

struct OrderBook {
    std::array<std::int64_t, 32> bid_px_ticks{};
    std::array<std::int64_t, 32> ask_px_ticks{};
    std::array<std::uint32_t, 32> bid_qty{};
    std::array<std::uint32_t, 32> ask_qty{};
};

struct Shard {
    SpscQueue<Tick, 8192> inbox;      // written by feed, read by this shard only
    OrderBook books[256];             // owned outright: no synchronisation anywhere
};

inline std::uint32_t shard_of(std::uint32_t instrument, std::uint32_t n_shards) noexcept {
    return instrument % n_shards;     // n_shards a power of two makes this an AND
}
```

Two placement details decide whether this is fast or merely tidy.

**Instrument affinity beats round robin.** Route by instrument so a given book stays in one
core's L2 forever. Round-robin sharding gives you perfect load balance and terrible cache
behaviour.

**Place the queue memory deliberately.** Linux allocates a page on the NUMA node of the
thread that *first touches* it, not the one that called `new`. If the producer's startup
code memsets the ring, the whole ring lives on the producer's node, and every consumer read
of a evicted line crosses the interconnect. Keep both ends on one node if you possibly can;
if you cannot, have the consumer first-touch the buffer, or place it explicitly with
`numa_alloc_onnode` from libnuma and bind with `numactl --membind`.

:::hft
The cost of getting this wrong is not subtle. A feed thread on socket 0 handing to a strategy
thread on socket 1, over a queue whose pages live on socket 0, pays a cross-socket line
transfer on the queue index, another on the payload, and a remote memory access on every
cache miss inside the strategy. Desks routinely find 300 to 600 nanoseconds of tick-to-trade
hiding in exactly that arrangement, and the fix is a two-line change to a config file. Check
placement before you optimise anything else.
:::

## Measure it, then prove it at startup

```cpp pingpong.cpp -- one-way handoff cost between two CPUs
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "affinity.hpp"        // pin_and_verify, cpu_relax

alignas(64) std::atomic<std::uint64_t> g_ping{0};
alignas(64) std::atomic<std::uint64_t> g_pong{0};

void responder(int cpu, std::uint64_t n) {
    pin_and_verify(cpu);
    for (std::uint64_t i = 1; i <= n; ++i) {
        while (g_ping.load(std::memory_order_acquire) != i) cpu_relax();
        g_pong.store(i, std::memory_order_release);
    }
}

int main(int argc, char** argv) {
    const int cpu_a = argc > 1 ? std::atoi(argv[1]) : 2;
    const int cpu_b = argc > 2 ? std::atoi(argv[2]) : 4;
    constexpr std::uint64_t kN = 1'000'000;

    pin_and_verify(cpu_a);
    std::thread t(responder, cpu_b, kN);

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 1; i <= kN; ++i) {
        g_ping.store(i, std::memory_order_release);
        while (g_pong.load(std::memory_order_acquire) != i) cpu_relax();
    }
    const auto t1 = std::chrono::steady_clock::now();
    t.join();

    const double rt = std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
    std::printf("cpu %d <-> %d: round trip %.1f ns, one way ~%.1f ns\n",
                cpu_a, cpu_b, rt, rt / 2.0);
}
```

Run that across every interesting pair on a new server and you have your machine's real
distance matrix, which is the input to the thread map. Then encode the conclusion in a
startup sequence that refuses to trade if the machine is not what you expect:

1. Read the intended map from config: which logical CPU each role gets.
2. Pin each thread and verify with `sched_getcpu()`. Abort on mismatch.
3. Check the isolation assumptions: the hot CPUs appear in `isolcpus`, their IRQ affinity
   masks exclude them, and the C-state governor is where you left it.
4. First-touch every queue, every arena and every book slab from the thread that will own
   it, so NUMA placement is decided by you and not by whichever thread happened to run.
5. Warm the path: push a few thousand synthetic ticks through decode, book update and order
   construction so the instruction cache, branch predictors and TLB are populated. Discard
   the output. Lesson 39 goes into why the first real message is otherwise the slowest one
   of the day.
6. Run the ping-pong above between the feed and strategy cores and assert the result is
   under your expected threshold. If someone re-imaged the box with SMT re-enabled, you find
   out at 07:00, not at the open.

:::exercise
Run the ping-pong program across four pairs on one machine: SMT siblings of one core, two
cores sharing an L2, two cores sharing only the LLC, and two cores on different sockets. Use
`lscpu -e` and the sysfs `shared_cpu_list` files to pick the pairs correctly rather than
guessing from CPU numbers. Tabulate the four one-way figures, then re-run the shared-LLC pair
with `_mm_pause` removed from both spin loops and explain the change.
:::

## Takeaways

- Logical CPU numbers are enumeration order, not topology. Read `thread_siblings_list` and
  the cache `shared_cpu_list` files, or use hwloc, before you pin anything.
- Give each hot thread a whole physical core, and put communicating threads as close as the
  cache hierarchy allows. Best to worst handoff distance is roughly an order of magnitude.
- Pinning can succeed and still put you elsewhere. Verify with `sched_getcpu()` and abort.
- The hot thread never sleeps: the futex wake path costs microseconds and a deep C-state exit
  costs more. Bounded spinning with a blocking fallback is for the cold threads only.
- Always `pause` in a spin loop. It avoids the pipeline flush, frees resources for the SMT
  sibling, and cuts power.
- Thread-per-core with instrument-sharded state and SPSC message passing removes shared
  mutable state entirely. First-touch the queue memory from the right thread.
