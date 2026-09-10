---
title: Atomics and Memory Ordering
part: Part V - Concurrency
summary: What std::atomic actually guarantees, what each memory order means in plain language, why x86 lets you ship code that is broken on ARM, and what a contended CAS really costs.
time: 45 min
level: expert
tags: atomics, memory-model, lock-free, cas, mesi
---

Lesson 31 left you with a rule: two threads touching the same object, at least one of them
writing, with no synchronisation, is a data race and a data race is undefined behaviour.
Atomics are the primitive that makes such an access defined. They are also the only tool
you have on the hot path, because a mutex costs a syscall the moment it is contended and
a trading thread that blocks has already lost. The price of atomics is that you must now
reason explicitly about *ordering*, which is the part nearly everybody gets wrong.

## What an atomic is

`std::atomic<T>` is a wrapper that makes reads and writes of a `T` indivisible: no other
thread can observe a half-written value, and the compiler and CPU are forbidden from
inventing extra reads, splitting a store into two, or keeping the value in a register
across a synchronisation point.

```cpp atomics_basics.cpp
#include <atomic>
#include <cstdint>

struct Quote {
    std::int64_t bid_ticks;
    std::int64_t ask_ticks;
};

static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic<void*>::is_always_lock_free);

// 16 bytes is target- and flag-dependent: on x86-64 it is lock-free only when the
// compiler may emit cmpxchg16b (-mcx16, implied by most -march= values since Nehalem).
inline bool quotes_are_lock_free(const std::atomic<Quote>& q) noexcept {
    return q.is_lock_free();
}
```

`is_always_lock_free` is `constexpr`, so you can `static_assert` on it; `is_lock_free()`
answers at runtime for this object on this machine. If neither holds, the library falls back
to a hidden mutex table keyed by address and every "atomic" operation becomes a lock acquire.
That is a disaster you will not notice until you profile, so assert the property you need.

`T` must be trivially copyable, which rules out `std::string` and is why a snapshot you want
to publish atomically has to be a flat POD struct.

## The operations

Five families, and you will use all of them.

```cpp
#include <atomic>
#include <cstdint>
#include <limits>

std::atomic<std::int64_t> session_high{std::numeric_limits<std::int64_t>::min()};
std::atomic<std::uint64_t> msgs_seen{0};

void on_trade(std::int64_t px_ticks) {
    // 1. store / load
    (void)msgs_seen.load(std::memory_order_relaxed);

    // 2. fetch_add and friends: read-modify-write, returns the OLD value
    msgs_seen.fetch_add(1, std::memory_order_relaxed);

    // 3. exchange: unconditional swap, returns the old value
    // 4. compare_exchange: conditional swap
    std::int64_t cur = session_high.load(std::memory_order_relaxed);
    while (px_ticks > cur &&
           !session_high.compare_exchange_weak(cur, px_ticks,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
        // On failure the CAS has already reloaded `cur` with the current value.
    }
}
```

`compare_exchange_weak(expected, desired)` compares the atomic against `expected`; if equal
it stores `desired` and returns `true`, otherwise it writes the observed value back into
`expected` and returns `false`. The **weak** form is permitted to fail *spuriously*, that
is, return `false` even when the comparison would have succeeded. That sounds useless until
you know why: on ARM and POWER a CAS compiles to a load-linked/store-conditional pair, and
the store-conditional fails if anything at all disturbed the reservation, including an
interrupt. `_strong` has to wrap that in a retry loop internally. So: **if you are already
in a loop, use `weak`; if you are not, use `_strong`.** Using `_strong` inside a loop buys
you a nested loop for nothing.

Note the two orders. The first applies on success, the second on failure, and the failure
order may not be stronger than the success order and may not be `release` or `acq_rel`
(there is no store on the failure path to release).

## The memory orders

An atomic operation orders *other, ordinary* memory operations around it. That is the whole
point. Here they are in plain language.

| Order | What it promises |
|---|---|
| `relaxed` | Atomicity only. This operation may be reordered freely with respect to everything else. |
| `acquire` | On a load: no read or write that appears *after* it in program order may be moved before it. |
| `release` | On a store: no read or write that appears *before* it may be moved after it. |
| `acq_rel` | Both, for a read-modify-write. |
| `seq_cst` | Acquire/release, plus every `seq_cst` operation in the whole program agrees on one single total order. The default. |

Think of `release` as "publish": everything I did up to here is now visible to whoever picks
this up. Think of `acquire` as "subscribe": everything the publisher did before their
release store is now visible to me.

`memory_order_consume` exists in the standard and is a dead letter. It was meant to express
dependency-ordered reads cheaply on POWER and ARM, no compiler ever implemented it as
specified, all of them promote it to `acquire`, and it has been discouraged since C++17.
Do not use it.

## Synchronises-with, and publishing a pointer

The formal machinery is simple once you see one instance of it. A release store on an atomic
**synchronises-with** an acquire load on that same atomic *that reads the value the release
store wrote*. Synchronises-with feeds **happens-before**, which is what makes ordinary,
non-atomic accesses race-free.

```cpp publish.cpp
#include <atomic>
#include <cstdint>

struct BookSnapshot {
    std::int64_t  bid_ticks;
    std::int64_t  ask_ticks;
    std::uint32_t bid_qty;
    std::uint32_t ask_qty;
};

inline std::atomic<const BookSnapshot*> g_latest{nullptr};

// Feed thread.
void publish(BookSnapshot* slot, std::int64_t bid, std::int64_t ask) {
    slot->bid_ticks = bid;          // plain stores
    slot->ask_ticks = ask;          // plain stores
    slot->bid_qty   = 100;
    slot->ask_qty   = 100;
    g_latest.store(slot, std::memory_order_release);   // publish
}

// Strategy thread.
std::int64_t current_spread() {
    const BookSnapshot* s = g_latest.load(std::memory_order_acquire);   // subscribe
    if (s == nullptr) return 0;
    return s->ask_ticks - s->bid_ticks;   // guaranteed to see all four stores above
}
```

The four field stores are not atomic and never become atomic. They are made *safe* because
the release store and the acquire load form a synchronises-with edge, and everything
sequenced before the release therefore happens-before everything sequenced after the
acquire. Downgrade either one to `relaxed` and the program has a data race on
`bid_ticks`, with no diagnostic and no failure on your x86 dev box.

:::warn
The edge only exists if the acquire load actually reads the value written by that release
store. An acquire load that returns `nullptr` synchronises with nothing.
:::

## What x86 actually does

x86-64 is **total store order**: the hardware already forbids load-load, load-store and
store-store reordering. Only store-load reordering is permitted, via the store buffer.

The consequence is that on x86-64 an acquire load is a plain `mov`, a release store is a
plain `mov`, and a relaxed load and a relaxed store are also plain `mov`. The memory order
argument affects only what the *compiler* may reorder. The single exception is a
sequentially consistent store, which must drain the store buffer:

```asm
; std::atomic<long> x;  x.store(1, std::memory_order_release);
mov     QWORD PTR x[rip], 1

; x.store(1, std::memory_order_seq_cst);   -- GCC and Clang emit
mov     rax, 1
xchg    QWORD PTR x[rip], rax      ; implicitly locked, ~20 cycles even uncontended

; x.fetch_add(1, std::memory_order_relaxed);
lock add QWORD PTR x[rip], 1       ; the lock prefix is not optional at any order
```

Two conclusions follow. First, a `seq_cst` store costs a locked instruction, a typical figure
being 20 to 40 cycles even when the line is already exclusive in L1, while a release store is
free. Every `seq_cst` store you downgrade to `release` is a measurable win.

Second, and this is the one that has ended careers: because relaxed, acquire and release
compile to the same instruction on x86, **wrong ordering annotations produce a program that
passes every test on x86 and corrupts state on ARM.** AArch64 is weakly ordered, and a
relaxed store there really can be observed out of order. Even on an all-x86 fleet you must
still reason in the abstract model, because the compiler reorders too.

:::hft
Desks that moved a feed handler from Xeon to Graviton or Ampere for cost reasons have
found latent races that had run correctly for years. The code was never right; x86 was
hiding it. Run your concurrency test suite under ThreadSanitizer, which models the abstract
machine rather than the hardware, and it will flag the missing acquire on the first pass.
:::

## What it costs, and why

Atomics are not expensive because of the instruction. They are expensive because of cache
coherence. Under **MESI**, every cache line in every core's L1 is in one of four states:
Modified, Exclusive, Shared, or Invalid. To write a line a core must own it Modified, which
requires sending a request-for-ownership on the interconnect and invalidating every other
core's copy.

So an uncontended atomic, where the line is already Modified in your L1, is cheap: a locked
add on modern x86 is typically 15 to 25 cycles. A *contended* atomic pulls the line across
the interconnect every time. A typical figure for a core-to-core line transfer within one
socket is 40 to 90 nanoseconds, and across sockets several hundred.

That is why a CAS loop degrades **superlinearly**. With N threads hammering one counter,
each CAS attempt must first acquire the line, and while it holds it the other N-1 threads
are invalidated. Only one of them wins, so the expected number of attempts grows with N,
and each attempt costs a line transfer. Throughput does not merely stop scaling; it falls.

:::perf
If you must have a shared counter across many threads, do not share it. Give each thread
its own `alignas(64)` counter and sum them when someone asks. `fetch_add` with
`memory_order_relaxed` on a per-thread counter is nearly free; the same call on a shared
one is the bottleneck.

```cpp
#include <atomic>
#include <cstdint>
#include <new>

struct alignas(std::hardware_destructive_interference_size) Stats {
    std::atomic<std::uint64_t> msgs{0};
    std::atomic<std::uint64_t> drops{0};
};
```
:::

Statistics counters are exactly the right home for `relaxed`. You want the increment not to
be lost, you do not care when other threads see it, and you certainly do not want it
ordering the surrounding market data code.

## ABA, fences, and the shapes of progress

**ABA** is the failure mode of CAS-based algorithms. A thread reads value A, is descheduled,
and by the time it runs again another thread has changed the location to B and back to A.
The CAS succeeds because the bits match, but the world it assumed is gone. The classic
victim is a lock-free stack: the head pointer is A again, but the node A now points
somewhere else, or has been freed and reallocated. Standard mitigations are a tagged
pointer (pack a 48-bit pointer with a 16-bit counter and CAS both together), hazard
pointers, or epoch-based reclamation. The simplest mitigation, and the one this course
prefers, is to avoid designs that need reclamation at all, which is what lessons 33 and 34
do.

`std::atomic_thread_fence` gives you ordering without attaching it to a particular variable.

```cpp fence.cpp
#include <atomic>
#include <cstdint>

inline std::int64_t g_payload_ticks = 0;   // ordinary, non-atomic
inline std::atomic<bool> g_ready{false};

void writer(std::int64_t px) {
    g_payload_ticks = px;
    std::atomic_thread_fence(std::memory_order_release);
    g_ready.store(true, std::memory_order_relaxed);
}

bool reader(std::int64_t& out) {
    if (!g_ready.load(std::memory_order_relaxed)) return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    out = g_payload_ticks;
    return true;
}
```

This is equivalent to the release-store/acquire-load pair, but it lets one fence cover
several relaxed operations, which matters when you publish two indices at once. Prefer the
order-on-the-operation form when you can: it is easier to read and it gives the compiler
tighter information.

Finally, the vocabulary of progress guarantees:

- **Lock-free**: at least one thread makes progress in a bounded number of steps, no matter
  how the others are scheduled. A CAS loop is lock-free: some thread's CAS always wins.
- **Wait-free**: *every* thread completes in a bounded number of its own steps. `fetch_add`
  is wait-free. A CAS loop is not, since one unlucky thread can starve indefinitely.
- **Obstruction-free** is a weaker cousin you will rarely need.

For a trading system the distinction is not academic: lock-free bounds throughput,
wait-free bounds *your* latency. The tail of your tick-to-trade distribution is decided by
the worst case of the slowest thread, so on the hot path you want wait-free structures.
That is precisely why the queue in lesson 33 has no CAS in it at all.

When you genuinely must wait, spin politely:

```cpp spin.cpp
#include <atomic>
#include <cstdint>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();                          // or __builtin_ia32_pause()
#elif defined(__aarch64__)
    asm volatile("isb" ::: "memory");     // ARM's closest equivalent
#endif
}

std::uint64_t wait_for_sequence(const std::atomic<std::uint64_t>& seq,
                                std::uint64_t want) noexcept {
    std::uint64_t v = seq.load(std::memory_order_acquire);
    while (v < want) {
        cpu_relax();
        v = seq.load(std::memory_order_acquire);
    }
    return v;
}
```

`pause` does three things: it hints to the core that this is a spin loop so the
memory-order-violation pipeline flush on exit is avoided, it yields execution resources to
the SMT sibling, and it reduces power draw. A bare spin loop without it can slow a
hyperthreaded sibling by a large factor and burns the loop at full issue rate for nothing.

:::exercise
Write a two-thread program: thread A fills a 64-byte struct then sets a flag; thread B
spins on the flag and checksums the struct. Run it 10 million times with a release/acquire
pair, then again with both operations `relaxed`. On x86 both will pass. Now build both
versions with `-fsanitize=thread` and run them: the relaxed version reports a data race on
the payload, the correct version is silent. Then, if you have an ARM machine to hand, run
the relaxed version natively and count the mismatches.
:::

## Takeaways

- `std::atomic<T>` needs `T` trivially copyable, and you should `static_assert` on
  `is_always_lock_free` rather than discover the hidden mutex in a profile.
- Use `compare_exchange_weak` inside a loop you already have, `_strong` when you have none;
  weak's spurious failure is the price of load-linked/store-conditional on ARM.
- A release store synchronises-with an acquire load that reads it, and that edge is what
  makes the surrounding non-atomic writes safe to read.
- On x86 relaxed, acquire and release loads and stores are all plain `mov`, so incorrect
  ordering is invisible until you run on ARM. Trust ThreadSanitizer, not your test suite.
- Atomics cost cache coherence, not instructions; a contended CAS loop degrades
  superlinearly, so shard the state instead of sharing it.
- Lock-free bounds throughput, wait-free bounds latency. On the hot path, want wait-free.
