---
title: Taming the OS and the Hardware
part: Part IV - Latency Engineering
summary: Core isolation, interrupt affinity, C-states, NUMA placement, huge pages and locked memory. Everything you do to the box so that the code you tuned is allowed to run without interruption.
time: 35 min
level: expert
tags: isolcpus, numa, cstates, hugepages, affinity, linux
---

You have written a decoder that takes 400 nanoseconds. Then the kernel decides to run a
timer tick on your core, or the CPU decides it was idle enough to enter a deep sleep
state, and that one message takes 60 microseconds. No amount of C++ fixes this. Past a
certain point, latency engineering stops being about your code and becomes about
persuading the operating system and the hardware to leave your core alone.

This entire lesson is Linux-specific, and most of it requires root. All figures are typical
values for contemporary x86-64 server hardware; measure your own.

## Pinning threads to cores

A thread that migrates between cores loses its L1 and L2 working set, its branch history
and its TLB entries, and if the new core is on a different socket it loses local memory
access too. The first thing you do to a hot thread is nail it down.

The blunt instrument is `taskset`, which applies to a whole process:

```sh
$ taskset -c 6 ./trader --config=prod.cfg
$ taskset -pc 6,7 12345          # change an existing process
```

Inside the program you can pin per thread, which is what you actually want since a trading
process has a feed thread, a strategy thread and several housekeeping threads with very
different requirements.

```cpp affinity.hpp Linux only
#pragma once
#include <pthread.h>
#include <sched.h>
#include <system_error>
#include <thread>

// Pin the calling thread to exactly one logical CPU.
inline void pin_to_core(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0)
        throw std::system_error(rc, std::generic_category(), "pthread_setaffinity_np");
}

// Name it too: a named thread is findable in perf, top and gdb.
inline void name_thread(const char* name) {
    pthread_setname_np(pthread_self(), name);   // max 15 chars + NUL
}
```

```cpp Starting a pinned feed thread
#include "affinity.hpp"
#include <atomic>
#include <cstdint>
#include <thread>

std::atomic<bool> g_running{true};

void feed_loop(std::uint64_t* messages) {
    pin_to_core(6);
    name_thread("feed");
    while (g_running.load(std::memory_order_relaxed)) {
        // poll the ring, decode, publish
        ++*messages;
    }
}

int main() {
    std::uint64_t messages = 0;
    std::jthread feed(feed_loop, &messages);
    // ... run
    g_running.store(false, std::memory_order_relaxed);
}
```

:::pitfall
Pin by physical topology, not by CPU number. `lscpu -e` shows the mapping, and on most
servers logical CPUs 0..N-1 and N..2N-1 are the two hyperthread siblings of the same
physical cores, so pinning to 6 and 7 may give you two threads fighting over one core's
execution resources. Read the topology, do not assume it.
:::

## Taking the core away from the kernel

Pinning your thread to core 6 does not stop the kernel from putting other things there.
Three boot parameters progressively evict the kernel from a core, and they work together.

```text /etc/default/grub, then update-grub and reboot
GRUB_CMDLINE_LINUX="isolcpus=2-7 nohz_full=2-7 rcu_nocbs=2-7 \
                    irqaffinity=0,1 intel_pstate=disable idle=poll \
                    processor.max_cstate=1 intel_idle.max_cstate=0 \
                    nosoftlockup mce=ignore_ce"
```

| Parameter | What it removes |
|---|---|
| `isolcpus=2-7` | The scheduler's load balancer. Nothing is placed on these cores unless explicitly pinned there |
| `nohz_full=2-7` | The periodic scheduler tick, when exactly one runnable task is on the core. Removes a ~1 ms interruption |
| `rcu_nocbs=2-7` | RCU callback processing, moved to the housekeeping cores |
| `irqaffinity=0,1` | The default target for device interrupts |

`nohz_full` has a condition attached that is easy to miss: the tick is only suppressed when
the core has **exactly one** runnable task. Two busy threads on one isolated core and the
tick comes back. Verify it is actually working rather than assuming:

```sh
$ cat /sys/devices/system/cpu/nohz_full
2-7
$ perf stat -e irq_vectors:local_timer_entry -C 6 -- sleep 10
             3      irq_vectors:local_timer_entry
```

Three interrupts in ten seconds is a working `nohz_full` core. Ten thousand is a broken one.

Interrupts still need somewhere to go. Move every device IRQ off the isolated cores, then
put your NIC's receive-queue interrupts on a housekeeping core near the NIC:

```sh
$ systemctl stop irqbalance                       # it will undo everything you do
$ for f in /proc/irq/*/smp_affinity_list; do echo 0-1 > "$f" 2>/dev/null; done
$ grep 'eth0-TxRx' /proc/interrupts               # find the queue IRQ numbers
$ echo 1 > /proc/irq/142/smp_affinity_list        # queue 0 -> CPU 1
```

Finally, scheduling policy. `SCHED_FIFO` makes your thread preempt anything of lower
priority and run until it yields.

```cpp Real-time priority, Linux only
#include <pthread.h>
#include <sched.h>
#include <system_error>

inline void set_fifo_priority(int priority) {   // 1..99, higher preempts
    sched_param p{};
    p.sched_priority = priority;
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    if (rc != 0)
        throw std::system_error(rc, std::generic_category(), "pthread_setschedparam");
}
```

:::warn
A `SCHED_FIFO` thread that spins without yielding, on a core that is not isolated, can
starve the kernel's own housekeeping threads and hang the machine. Linux's real-time
throttle (`/proc/sys/kernel/sched_rt_runtime_us`, 950000 by default) exists to stop that
and caps you at 95% of the core. On a properly isolated core the honest configuration is
usually `SCHED_FIFO` **plus** isolation, with the throttle disabled; on a shared box, do
not use `SCHED_FIFO` at all.
:::

## Frequency, idle states and hyperthreading

A CPU that has been idle enters a **C-state**. C1 is a light nap; C6 powers down the core
and flushes its caches. Exit latencies are published in sysfs and are the numbers that
matter to you:

```sh
$ cat /sys/devices/system/cpu/cpu6/cpuidle/state*/name
POLL
C1
C1E
C6
$ cat /sys/devices/system/cpu/cpu6/cpuidle/state*/latency
0
2
10
133
```

Typical figures: C1 exit around 2 microseconds, C1E around 10, C6 around 130. A quote
arrives after a quiet second, your core is in C6, and you pay over a hundred microseconds
before your 400-nanosecond decoder starts. **On a trading host you disable deep idle
entirely**, either with the boot parameters above or at runtime by holding
`/dev/cpu_dma_latency` open with a zero written to it:

```cpp Pin the whole system's acceptable wakeup latency to 0 us
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>

int hold_cpu_dma_latency() {                 // keep the fd open for process lifetime
    const int fd = ::open("/dev/cpu_dma_latency", O_WRONLY);
    if (fd < 0) return -1;
    const std::int32_t target_us = 0;
    (void)::write(fd, &target_us, sizeof(target_us));
    return fd;                               // closing it releases the constraint
}
```

**P-states** are the frequency the core runs at when it is awake, and **turbo** raises it
opportunistically when few cores are busy and the package is cool. Turbo is a
determinism problem: your core runs at 4.2 GHz for the first minute of the session and
3.6 GHz once the box is warm, so the same code path takes a measurably different number of
nanoseconds at 09:30 and at 14:00. The desk argument is to **fix the frequency rather than
maximise it**: pick a clock the part sustains all day, disable turbo, and get the same
latency at every hour.

```sh
$ cpupower frequency-set --governor performance
$ echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
$ cat /proc/cpuinfo | grep MHz | sort -u          # confirm it stopped moving
```

**Hyperthreading** puts two logical CPUs on one physical core sharing the execution units,
the L1 and L2 caches and the TLBs. Two positions are defensible and one is not:

- **Disable SMT in BIOS.** Halves your logical core count and gives every remaining thread
  the full core deterministically. Simple, and the usual choice.
- **Leave SMT on and leave the sibling idle.** Keeps the extra cores for housekeeping
  while the hot core's sibling is never scheduled, which requires that the sibling is in
  your `isolcpus` set and that nothing is ever pinned to it. Gives the same isolation with
  more flexibility, at the cost of a configuration mistake being invisible.
- **Leave SMT on and run a second busy thread on the sibling.** Do not. Your hot thread's
  L1 is now shared with whatever that thread does, and `nohz_full` stops working because
  the core has two runnable tasks.

## NUMA and huge pages

On a multi-socket server, each socket has its own memory controller. Access to memory on
your own socket is **local**; access across the interconnect is **remote**. Typical
figures are roughly 80 to 90 ns local and 130 to 150 ns remote, with meaningfully lower
bandwidth remotely.

Linux allocates with a **first-touch** policy: `malloc` reserves address space, and the
physical page is allocated on the NUMA node of whichever thread first writes to it. This
has a direct consequence for your program's structure. If `main` allocates the ring buffer
and then hands it to a feed thread pinned to node 1, the pages live on node 0 and every
access is remote for the life of the process. **Allocate and touch on the thread that will
use it.**

```sh
$ numactl --hardware                       # node distances and free memory per node
$ numactl --cpunodebind=1 --membind=1 ./trader --config=prod.cfg
$ cat /sys/class/net/eth0/device/numa_node # which node the NIC hangs off
```

The rule to hand to whoever builds the box: **NIC, memory and thread on the same node.**
A kernel-bypass receive path whose buffers are on the far node throws away most of what
bypass bought you.

Huge pages address the TLB rather than the memory controller. A 4 KB page needs one TLB
entry per 4 KB; a 2 MB huge page covers the same span with one entry.

| Mechanism | How you get it | Behaviour |
|---|---|---|
| **THP** (transparent) | `/sys/kernel/mm/transparent_hugepage/enabled`, or `madvise` | Kernel promotes pages in the background. Convenient, and can stall |
| **hugetlbfs** (explicit) | `hugepages=` at boot, `mmap` with `MAP_HUGETLB` | Reserved at boot, never swapped, never compacted, deterministic |

:::warn
Set transparent huge pages to `madvise`, never `always`, and set
`/sys/kernel/mm/transparent_hugepage/defrag` to `madvise` or `never`. With `always`, a
page fault can trigger synchronous memory compaction, which is a multi-millisecond stall
inside an ordinary allocation. This is one of the classic causes of an unexplained
millisecond outlier on an otherwise well-tuned box.
:::

```cpp Explicitly reserved 2 MB pages for a ring buffer, Linux only
#include <sys/mman.h>
#include <cstddef>
#include <cstdint>
#include <new>

std::byte* alloc_hugetlb(std::size_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc{};
    return static_cast<std::byte*>(p);
}
```

## Page faults, syscalls, and things that are secretly syscalls

A page fault on the hot path is a trap into the kernel, a page allocation and possibly a
zeroing of 4 KB. You eliminate them at startup by locking memory and pre-faulting it.

```cpp Lock everything and pre-fault the ring, Linux only
#include <sys/mman.h>
#include <sys/resource.h>
#include <cstddef>
#include <cstdint>
#include <vector>

void lock_and_prefault(std::vector<std::byte>& buf) {
    // Lock current and future mappings; MCL_ONFAULT is available if you prefer.
    ::mlockall(MCL_CURRENT | MCL_FUTURE);

    // Touch one byte per 4 KB page so the fault happens now, not at 09:30:00.
    for (std::size_t i = 0; i < buf.size(); i += 4096) buf[i] = std::byte{0};
}
```

Check the result rather than trusting it. `getrusage` reports minor and major faults; a
correctly warmed process takes essentially none after the first second of trading.

```cpp
#include <sys/resource.h>
#include <print>

void report_faults() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    std::print("minor faults {}, major faults {}\n", ru.ru_minflt, ru.ru_majflt);
}
```

A system call costs roughly 100 to 300 nanoseconds of pure transition on a modern kernel,
and considerably more with Spectre and Meltdown mitigations enabled. Some of the things
that look like ordinary function calls are syscalls in disguise:

- Any logging that reaches a file or socket: `write`.
- `new` and `malloc` when the allocator needs more arena: `mmap` or `brk`. Lesson 24.
- `std::mutex` under contention: `futex`. Lesson 31.
- `std::this_thread::yield`: `sched_yield`.
- `clock_gettime`, which is usually resolved in the vDSO without entering the kernel, but
  falls back to a real syscall if the clocksource is not `tsc`. Check
  `/sys/devices/system/clocksource/clocksource0/current_clocksource`.
- Growing a `std::vector`, throwing an exception that unwinds through a cold page, and any
  first touch of a lazily mapped region.

## Measuring what is left, and the checklist

After all of the above there is residual jitter, and the standard instrument for measuring
it is `cyclictest` from the rt-tests package. It sleeps for a fixed interval and reports
how late it was woken, which is a direct measurement of everything the system does behind
your back.

```sh
$ cyclictest -m -p 95 -t 1 -a 6 -i 200 -D 10m -h 200 -q
T: 0 ( 9812) P:95 I:200 C:3000000 Min: 1 Act: 2 Avg: 2 Max: 14
```

A well-tuned isolated core gives a maximum in the low tens of microseconds over ten
minutes. A maximum in the hundreds means something is still interrupting you, and the next
step is `ftrace` or `perf sched` to find out what.

:::hft
Hand this to whoever builds the box, and audit it before every production deployment. It
is worth more than a week of C++ optimisation and it is routinely half-configured.

**BIOS:** disable C-states below C1, disable turbo, disable SMT (or plan to idle the
siblings), set power profile to maximum performance rather than balanced, disable any
"processor power management" or hardware P-state autonomy, disable NUMA node interleaving
so the nodes stay visible, and set fan profile to a level that avoids thermal throttling.

**Kernel command line:** `isolcpus`, `nohz_full`, `rcu_nocbs` covering the trading cores;
`irqaffinity` naming only the housekeeping cores; `intel_pstate=disable`,
`processor.max_cstate=1`, `intel_idle.max_cstate=0`; `hugepages=N` if you use hugetlbfs.

**Runtime:** `irqbalance` stopped and disabled, device IRQs moved off the trading cores,
NIC queue IRQs pinned near the NIC's NUMA node, governor set to `performance`, turbo off,
THP set to `madvise` with defrag `madvise` or `never`, clocksource `tsc`.

**Process:** every thread pinned and named, `mlockall` at startup, all buffers pre-faulted,
allocation done on the consuming thread's node, `SCHED_FIFO` on the hot threads only.

**Verify, do not assume:** `local_timer_entry` count per isolated core, `cyclictest`
maximum, `numastat` for remote allocations, `getrusage` fault counts after an hour of
trading. Every one of these has been silently wrong on somebody's production box.
:::

:::exercise
Run `cyclictest -m -p 95 -t 1 -a <cpu> -i 200 -D 60 -q` on an ordinary core and record the
maximum. Then apply what you can without a reboot: stop `irqbalance`, move IRQs away, set
the governor to `performance`, and hold `/dev/cpu_dma_latency` open at zero. Rerun and
compare. Finally run `perf stat -e irq_vectors:local_timer_entry -C <cpu> -- sleep 10` and
see how far you are from a genuinely isolated core, which needs the boot parameters.
:::

## Takeaways

- Pin every hot thread by physical topology, and name it so it is findable in a profile.
- `isolcpus`, `nohz_full` and `rcu_nocbs` each remove a different interruption, and
  `nohz_full` only works when the core has exactly one runnable task. Verify with the
  local timer interrupt count.
- Deep C-states cost tens to over a hundred microseconds on wakeup, which dwarfs any code
  you have written. Disable them, and prefer a fixed frequency over the highest frequency,
  because determinism beats a few percent of peak clock.
- First-touch NUMA placement means the thread that will use the memory must be the one
  that touches it first. Keep NIC, memory and thread on one node.
- Use `madvise` huge pages, never `always` with defrag on, or you will buy multi-millisecond
  compaction stalls inside a page fault.
- `mlockall` plus pre-faulting removes page faults after startup; check with `getrusage`
  and measure residual jitter with `cyclictest`.
