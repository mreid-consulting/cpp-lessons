---
title: The Seqlock: Publishing Market Data
part: Part V - Concurrency
summary: One writer, many readers, nobody blocks and the writer never slows down. The protocol, a working implementation, and an honest look at the data race in the middle of it.
time: 35 min
level: expert
tags: seqlock, market-data, shared-memory, memory-model, publish-subscribe
---

The SPSC queue of lesson 33 solves handoff: every message the producer writes is delivered
exactly once. Market data distribution is a different problem. A feed handler holds the
current top of book for an instrument and updates it hundreds of thousands of times a
second. Five strategy threads want to know what it is *right now*. They do not want the
history, they do not want every intermediate state, and above all they must not slow the
writer down. That is a seqlock.

## The problem, and three wrong answers

State it precisely. One writer, publishing a small, fixed-size, trivially copyable value
such as a top-of-book quote or a book summary. N readers, each of which wants a *consistent
snapshot* of that value: never a torn mixture of an old bid and a new ask. Requirements:

- The writer must never wait for a reader, ever, for any reason.
- Readers must never block. A reader that is descheduled must not stall the writer.
- Readers may miss updates. Conflation is not a bug here; it is the specification.

**A mutex is wrong.** The writer would have to wait for readers, and one reader preempted
inside the critical section stalls the entire feed. Even uncontended, a lock and unlock pair
costs two atomic read-modify-writes, typically 40 or more cycles, on every publish.

**A queue is wrong.** It delivers every update to every reader, so a reader that falls
behind either applies stale data or forces the writer to drop. You do not want the history;
you want the latest. Building conflation on top of a queue means the reader drains and
discards, which is work proportional to the update rate rather than the read rate.

**A `shared_ptr` swap is wrong.** `std::atomic<std::shared_ptr<Quote>>` is correct but slow:
each publish allocates a control block, each read is an atomic increment on a refcount that
every reader shares, which is the contended-cache-line problem from lesson 32, and the
allocation puts the heap on the hot path against everything lesson 24 said. A typical read
is tens of nanoseconds and every read invalidates the line for every other reader.

## The protocol

A seqlock is a version counter with a parity convention.

```text
writer:                                  reader:
  seq  0 -> 1   (odd: "I am writing")      read seq   -> if odd, retry
  write payload                            copy payload
  seq  1 -> 2   (even: "settled")          read seq   -> if changed, retry
```

The reader's copy is only valid if the sequence was even when it started and identical when
it finished. Anything else means a write overlapped the copy, and the reader simply tries
again. Note the asymmetry that makes this work: the writer's path is unconditional, three
stores and a copy, with no branches and no waiting. All of the cost of contention is paid by
the readers, which is exactly the trade a trading system wants.

## A working implementation

```cpp seqlock.hpp
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

template <typename T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_default_constructible_v<T>);

public:
    // Exactly one writer thread.
    void store(const T& v) noexcept {
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);            // odd: in progress
        std::atomic_thread_fence(std::memory_order_release);     // seq before payload
        std::memcpy(&payload_, &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);     // payload before seq
        seq_.store(s + 2, std::memory_order_relaxed);            // even: settled
    }

    // Any number of reader threads.
    T load() const noexcept {
        T out;
        for (;;) {
            const std::uint32_t before = seq_.load(std::memory_order_acquire);
            if (before & 1u) continue;                           // writer mid-update
            std::memcpy(&out, &payload_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire); // payload before seq
            if (seq_.load(std::memory_order_relaxed) == before)
                return out;                                      // clean snapshot
        }
    }

    std::uint32_t version() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<std::uint32_t> seq_{0};
    T payload_{};
};
```

```cpp Publishing top of book
#include <cstdint>

struct TopOfBook {
    std::int64_t  bid_ticks;
    std::int64_t  ask_ticks;
    std::uint32_t bid_qty;
    std::uint32_t ask_qty;
    std::uint64_t exchange_ns;
};
static_assert(sizeof(TopOfBook) == 32);

inline Seqlock<TopOfBook> g_tob[4096];   // one per instrument

void on_book_change(std::uint32_t instrument, const TopOfBook& t) noexcept {
    g_tob[instrument].store(t);          // never blocks, never allocates
}

std::int64_t spread_ticks(std::uint32_t instrument) noexcept {
    const TopOfBook t = g_tob[instrument].load();
    return t.ask_ticks - t.bid_ticks;
}
```

The four ordering constraints are worth spelling out, because three of them are invisible on
x86 and all four matter on ARM.

1. The odd `seq_` store must be visible *before* the payload writes. Otherwise a reader sees
   an even sequence, reads a half-updated payload, and reads the same even sequence again.
   The first release fence enforces it.
2. The payload writes must be visible *before* the even `seq_` store. Second release fence.
3. The reader's first `seq_` load must happen before the payload copy: `acquire` on the load.
4. The payload copy must happen before the reader's second `seq_` load. This is the one
   people forget, and it needs the acquire fence, not an acquire load: an acquire load orders
   things *after* it, and what we need is to stop the *earlier* copy from sinking below.

## The race in the middle

Read the reader's `memcpy` again. It reads `payload_` with an ordinary, non-atomic access
while the writer may be writing it with an ordinary, non-atomic access. By the letter of the
C++ memory model that is a data race, and a data race is undefined behaviour for the whole
program, not merely a garbage value in `out`. The retry check discards the garbage, but the
standard does not give you the right to have produced it.

This is not a hypothetical. A sufficiently clever compiler is permitted to assume no race
occurred and, for instance, re-read `payload_` after the validity check rather than keeping
the copy, which would defeat the protocol entirely. Hans Boehm's paper *Can Seqlocks Get
Along With Programming Language Memory Models?* is the standard reference for exactly this
gap, and WG21 has an open line of work, P1478, proposing a byte-wise atomic memcpy to close
it. As of C++23 it is not in the standard.

Three practical resolutions, in increasing order of pedantry:

**Use `std::atomic_ref` over the payload words.** C++20 lets you apply atomic operations to
an ordinary object without changing its type. Relaxed loads and stores through
`std::atomic_ref` are race-free by definition and on x86-64 compile to the same `mov`
instructions the `memcpy` would have produced.

**Store the payload as an array of relaxed atomic words.** The blunt version of the same
idea, and the one most production seqlocks I have read actually use.

```cpp Race-free payload, same instructions
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

template <typename T>
class WordSeqlock {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0, "pad T to 8 bytes");
    static constexpr std::size_t kWords = sizeof(T) / sizeof(std::uint64_t);

public:
    void store(const T& v) noexcept {
        std::uint64_t tmp[kWords];
        std::memcpy(tmp, &v, sizeof(T));                     // private, no race
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(tmp[i], std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_relaxed);
    }

    bool try_load(T& out) const noexcept {
        const std::uint32_t before = seq_.load(std::memory_order_acquire);
        if (before & 1u) return false;
        std::uint64_t tmp[kWords];
        for (std::size_t i = 0; i < kWords; ++i)
            tmp[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != before) return false;
        std::memcpy(&out, tmp, sizeof(T));                   // private, no race
        return true;
    }

private:
    alignas(64) std::atomic<std::uint32_t> seq_{0};
    std::atomic<std::uint64_t> words_[kWords]{};
};
```

**Or use the plain `memcpy` and accept it.** Every mainstream implementation compiles the
first version to the code you want, the Linux kernel has shipped this exact pattern for
twenty years, and the practical risk is low. It is a defensible engineering choice, but make
it knowingly and write the comment. The word-array version costs you the compiler's ability
to vectorise the copy, which for payloads above roughly 64 bytes is a real, measurable loss.

:::warn
Whichever form you choose, the payload must contain no pointers into the writer's address
space and no self-referential state. A reader can be looking at a mixture of two generations
mid-copy; if that mixture is a length field from one generation and an array from another,
you will index out of bounds *before* the retry check ever runs. Keep the payload flat,
fixed-size and self-consistent for any bit pattern.
:::

## Sizing, alignment, and shared memory

The retry rate is the entire performance story, and the arithmetic is simple. If the writer
publishes every `P` nanoseconds and a reader's copy takes `C` nanoseconds, then a read that
starts at a uniformly random moment collides with a write with probability roughly `C / P`.
Retries compound: the expected number of attempts is about `1 / (1 - C/P)`.

| Payload | Typical copy time | Writes every 1 us | Writes every 100 ns |
|---|---|---|---|
| 32 B (top of book) | ~5 ns | ~0.5% retry | ~5% retry |
| 256 B (book summary) | ~20 ns | ~2% retry | ~25% retry |
| 4 KB (full book) | ~300 ns | ~30% retry | livelock |

Those copy times are typical L1-resident figures on a modern x86 core, not guarantees.
The conclusion is robust regardless: **publish small things.** A seqlock over a full 4 KB
order book updated at exchange rates will starve its readers. Publish the top five levels,
or publish a version number that readers use to decide whether to take a slower path.

On alignment: the sequence counter is read by every reader on every attempt and written by
the writer on every publish, so it belongs on its own cache line, away from anything else
the writer touches independently. The exception is a payload of one cache line or less,
where some implementations deliberately co-locate the counter and the payload so that a
reader's whole snapshot is a single line transfer. Both choices are reasonable; measure.

The seqlock's real superpower is that it works **across processes**. Nothing in the protocol
requires the reader to be in the writer's address space: it is just bytes and a counter.

```cpp Market data plane in shared memory
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

struct MdPlane {
    std::uint32_t magic;
    std::uint32_t instrument_count;
    Seqlock<TopOfBook> tob[4096];
};

MdPlane* map_plane(const char* name, bool writer) {
    const int fd = ::shm_open(name, writer ? (O_CREAT | O_RDWR) : O_RDONLY, 0666);
    if (fd < 0) return nullptr;
    if (writer && ::ftruncate(fd, sizeof(MdPlane)) != 0) { ::close(fd); return nullptr; }
    void* p = ::mmap(nullptr, sizeof(MdPlane),
                     writer ? (PROT_READ | PROT_WRITE) : PROT_READ,
                     MAP_SHARED, fd, 0);
    ::close(fd);
    return p == MAP_FAILED ? nullptr : static_cast<MdPlane*>(p);
}
```

One feed handler process writes; strategy processes, risk monitors, the GUI and the recorder
all map the same segment read-only and sample it. No IPC, no serialisation, no kernel on the
read path. This is how a large fraction of production market data distribution actually
works inside a colo rack.

:::hft
The failure mode you must design for is a writer that dies mid-update, leaving `seq_` odd
forever. Every reader then spins in `load()` and the whole process hangs. Give the reader a
bounded retry count and a fallback, and have a supervisor watch a heartbeat counter in the
segment. A seqlock reader loop with no exit condition is the single most common way a
shared-memory market data plane takes down a trading system, and it always happens on the
day the feed handler segfaults.
:::

## Limits, and what else there is

A seqlock gives you: one writer, wait-free publishing, non-blocking conflated reads, a flat
fixed-size payload, and cross-process operation for free. It does not give you multiple
writers (you would need a lock around the writer, at which point reconsider the design), it
does not give you every update, and it does not give you variable-size or pointer-rich data.

**Double buffering** is the nearest alternative: keep two payload slots and an atomic index,
have the writer fill the inactive slot then flip the index with a release store. Readers get
a snapshot with no retry loop at all, which bounds reader latency, and the writer is still
wait-free. The catch is knowing when the old buffer is free to overwrite: with two buffers a
slow reader can still be reading the slot the writer wants next, so you need three or more
slots, or reader-presence counters, and now you have most of the complexity of RCU.

**RCU**, read-copy-update, generalises this to pointer-linked structures: readers dereference
a published pointer inside a read-side critical section, writers publish a new version and
defer reclaiming the old one until every pre-existing reader has finished. It handles
variable-size data that a seqlock cannot, at the cost of a reclamation mechanism and
unbounded memory growth if a reader stalls. Reach for it when publishing an order book with
a variable number of levels; stay with the seqlock for anything fixed-size.

:::exercise
Build a `Seqlock<TopOfBook>` with one writer thread publishing a monotonically increasing
`exchange_ns` and a matching `bid_ticks` in a tight loop, and four reader threads sampling
it. Have each reader assert that `bid_ticks` and `exchange_ns` are consistent with each
other, and count retries. Then run the whole thing under `-fsanitize=thread`. Report the
retry rate for a 32-byte payload and again after padding `TopOfBook` to 512 bytes, and
compare the measured rate against the `C / P` estimate above.
:::

## Takeaways

- A seqlock publishes a small fixed-size snapshot with a wait-free writer and non-blocking,
  conflating readers. All the cost of contention lands on the readers.
- Odd sequence means "in progress"; a reader's copy is valid only if the counter was even
  before and unchanged after.
- Four ordering constraints, and the easy one to miss is the acquire *fence* keeping the
  payload copy above the second sequence load.
- The plain `memcpy` form has a formal data race. Relaxed atomic words or `std::atomic_ref`
  fix it; using `memcpy` anyway is defensible but must be a deliberate, documented choice.
- Retry rate is roughly copy time over publish interval. Keep the payload to a few cache
  lines or readers will starve.
- It works unchanged across processes in shared memory, which is why it underpins so many
  market data planes. Always bound the reader's retry loop.
