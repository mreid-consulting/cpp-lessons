---
title: Building a Wait-Free SPSC Queue
part: Part V - Concurrency
summary: One producer, one consumer, no locks and no CAS. Built up in stages from the naive version to a header-only queue, with the cache-line reasoning that makes it fast.
time: 40 min
level: expert
tags: spsc, ring-buffer, wait-free, false-sharing, queues
---

The single-producer single-consumer queue is the most important data structure in a trading
system. A feed handler thread decodes packets and hands them to a strategy thread; the
strategy hands orders to a sender thread. Every one of those handoffs is SPSC, and because
the constraint is exactly one writer and exactly one reader, it can be built with no locks,
no compare-and-swap, and a worst case bounded by a fixed number of instructions. That last
property, wait-freedom, is what makes it usable on a path where a tail latency spike costs
real money.

## The contract

```cpp The interface we are building towards
#include <cstddef>

template <typename T, std::size_t Capacity>
class SpscQueue {
public:
    bool try_push(const T& v) noexcept;   // false if full. Never blocks.
    bool try_pop(T& out) noexcept;        // false if empty. Never blocks.
};
```

Three requirements, and they are non-negotiable:

- **Exactly one thread calls the producer side, exactly one calls the consumer side.** Two
  producers and the whole design collapses. This is a contract you enforce by review.
- **Neither side ever blocks.** A full queue returns `false` and the producer decides what
  to do: drop, count, or spin. It never waits on the consumer.
- **Capacity is a power of two.** Then the wrap from a monotonically increasing counter to a
  slot index is `i & (Capacity - 1)`, a single-cycle `and`, instead of `i % Capacity`, which
  for a runtime divisor is a 20-to-40-cycle hardware division on typical x86 cores.

We track two counters that only ever increase: `write_`, the number of items ever pushed,
and `read_`, the number ever popped. The queue is empty when they are equal and full when
they differ by `Capacity`. Monotonic counters are nicer than wrapped indices because they
distinguish full from empty without wasting a slot, and 64 bits will not wrap in the
lifetime of any exchange.

## The naive version, and why it fails twice

```cpp Do not ship this
#include <cstddef>

template <typename T, std::size_t Capacity>
class NaiveQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "power of two");
    T buf_[Capacity];
    std::size_t write_ = 0;
    std::size_t read_  = 0;
public:
    bool try_push(const T& v) {
        if (write_ - read_ == Capacity) return false;
        buf_[write_ & (Capacity - 1)] = v;
        ++write_;
        return true;
    }
    bool try_pop(T& out) {
        if (write_ == read_) return false;
        out = buf_[read_ & (Capacity - 1)];
        ++read_;
        return true;
    }
};
```

This is **incorrect**, and separately it is **slow**.

Incorrect, because `write_` and `read_` are plain `std::size_t` touched by two threads with
at least one writer: a data race, therefore undefined behaviour. Concretely, the compiler is
entitled to hoist the load of `read_` out of a loop and never reload it, so a producer can
spin forever on a queue the consumer has long since drained. Worse, nothing orders the
store to `buf_[...]` against the increment of `write_`. On a weakly ordered machine the
consumer can observe the new `write_` and then read a slot the producer has not filled yet.
On x86 it will appear to work, which is the trap from lesson 32.

Slow, because `write_` and `read_` are adjacent members and therefore almost certainly on
the same 64-byte cache line. Every producer increment invalidates that line in the
consumer's L1 and vice versa. The line ping-pongs across the interconnect on every single
operation. This is **false sharing**, covered in lesson 21, and in a two-thread SPSC
benchmark removing it is typically worth a factor of two to five in throughput.

## Correct ordering

```cpp Stage 2: atomics with the right orders
#include <atomic>
#include <cstddef>
#include <cstdint>

template <typename T, std::size_t Capacity>
class OrderedQueue {
    static constexpr std::uint64_t kMask = Capacity - 1;
    T buf_[Capacity];
    std::atomic<std::uint64_t> write_{0};
    std::atomic<std::uint64_t> read_{0};
public:
    bool try_push(const T& v) noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);   // mine
        const std::uint64_t r = read_.load(std::memory_order_acquire);    // theirs
        if (w - r == Capacity) return false;
        buf_[w & kMask] = v;
        write_.store(w + 1, std::memory_order_release);                   // publish
        return true;
    }
    bool try_pop(T& out) noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);    // mine
        const std::uint64_t w = write_.load(std::memory_order_acquire);   // theirs
        if (w == r) return false;
        out = buf_[r & kMask];
        read_.store(r + 1, std::memory_order_release);                    // publish
        return true;
    }
};
```

Four orderings, four different reasons:

- **`relaxed` on your own index.** Nobody else writes it, so there is nothing to synchronise
  with. You only need the atomic to stop the compiler caching it in a register across the
  other side's stores. This is genuinely free on every architecture.
- **`acquire` on the other side's index.** If the consumer tells me a slot is free, I must
  see all of the consumer's reads of that slot as having completed. The acquire load pairs
  with the consumer's release store.
- **`release` when publishing my index.** The slot write must not be reordered after the
  index bump. This is the edge that makes the plain, non-atomic `buf_[...]` access safe.
- Nothing anywhere is `seq_cst`. There is no global ordering question here, and on x86 a
  `seq_cst` store would cost you a locked instruction per operation for nothing.

Note what is absent: no CAS, no loop, no retry. Each operation is a bounded, fixed sequence
of instructions regardless of what the other thread is doing. That is wait-free.

## Cache lines, and the optimisation that matters most

First, separate the two indices so the producer and the consumer never write to the same
line. Note that the buffer itself gets its own line too, otherwise the tail of `read_`'s
line overlaps the first few slots.

```cpp
alignas(64) std::atomic<std::uint64_t> write_{0};
alignas(64) std::atomic<std::uint64_t> read_{0};
alignas(64) T buf_[Capacity];
```

That fixes the write-write ping-pong. But the producer still *reads* `read_` on every push,
which means it still pulls the consumer's line into Shared state on every operation, forcing
the consumer's next store to re-invalidate it. Reads are cheaper than writes but this is
still a coherence transaction per message.

The fix is the single biggest win in the whole implementation: **each side keeps a private,
non-atomic copy of the other side's index, and only refreshes it when it appears the queue
is full or empty.**

```cpp The cached-index trick
bool try_push(const T& v) noexcept {
    const std::uint64_t w = write_.load(std::memory_order_relaxed);
    if (w - cached_read_ == Capacity) {              // maybe full, per stale info
        cached_read_ = read_.load(std::memory_order_acquire);   // only now touch their line
        if (w - cached_read_ == Capacity) return false;         // genuinely full
    }
    buf_[w & kMask] = v;
    write_.store(w + 1, std::memory_order_release);
    return true;
}
```

`cached_read_` is a plain `std::uint64_t` written only by the producer, and it lives on the
producer's cache line. A stale value is always *safe*: it can only under-report the free
space, never over-report it, because `read_` only ever increases. So the worst case of
staleness is a spurious `false` that resolves on the next call.

In steady state, where the queue is neither full nor empty, the producer now touches exactly
two cache lines: its own index line, which stays Modified in its L1 and never leaves, and
the slot it is writing. The consumer's line is not read at all. The interconnect traffic per
message drops to the unavoidable minimum, which is the transfer of the data slot itself.
Measured on a same-socket pair, adding index caching to an already-padded queue is typically
worth another factor of two to three in sustained throughput.

## Non-trivial element types, and batching

Assigning into `buf_[w & kMask]` requires every slot to hold a live, default-constructed `T`
from the moment the queue is created. That is wrong for a type with no default constructor,
wasteful for one that allocates, and it forbids move-only types. The fix is raw storage plus
placement construction.

```cpp
#include <memory>     // std::construct_at, std::destroy_at
#include <new>        // std::launder

alignas(64) alignas(T) std::byte storage_[sizeof(T) * Capacity];

T* slot(std::uint64_t i) noexcept {
    return std::launder(reinterpret_cast<T*>(&storage_[(i & kMask) * sizeof(T)]));
}
```

The producer does `std::construct_at(slot(w), args...)`; the consumer moves out of the slot
and then `std::destroy_at`s it. The destructor of the queue destroys whatever is still in
flight. `std::launder` is the incantation that tells the compiler the bytes now hold a live
object; without it you are formally reading through a pointer to storage rather than to an
object.

Batching is the other lever. One release store per *batch* rather than per item amortises
the only expensive part of the operation over the whole batch, and for a feed handler
draining a UDP datagram with 40 ITCH messages in it, that is exactly the shape of the work.

## The finished queue

```cpp spsc_queue.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static constexpr std::uint64_t kMask = Capacity - 1;
    static constexpr std::size_t  kLine = 64;

public:
    using value_type = T;

    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    ~SpscQueue() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            const std::uint64_t w = write_.load(std::memory_order_relaxed);
            for (std::uint64_t r = read_.load(std::memory_order_relaxed); r != w; ++r)
                std::destroy_at(slot(r));
        }
    }

    // ---- producer side: one thread only ----------------------------------
    template <typename... Args>
    bool try_emplace(Args&&... args) noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        if (w - cached_read_ == Capacity) {
            cached_read_ = read_.load(std::memory_order_acquire);
            if (w - cached_read_ == Capacity) return false;
        }
        std::construct_at(slot(w), std::forward<Args>(args)...);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    bool try_push(const T& v) noexcept { return try_emplace(v); }

    std::size_t try_push_n(const T* src, std::size_t n) noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        std::uint64_t room = Capacity - (w - cached_read_);
        if (room < static_cast<std::uint64_t>(n)) {
            cached_read_ = read_.load(std::memory_order_acquire);
            room = Capacity - (w - cached_read_);
        }
        const std::size_t count =
            static_cast<std::uint64_t>(n) < room ? n : static_cast<std::size_t>(room);
        for (std::size_t i = 0; i < count; ++i)
            std::construct_at(slot(w + i), src[i]);
        write_.store(w + count, std::memory_order_release);   // one publish per batch
        return count;
    }

    // ---- consumer side: one thread only ----------------------------------
    bool try_pop(T& out) noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        if (r == cached_write_) {
            cached_write_ = write_.load(std::memory_order_acquire);
            if (r == cached_write_) return false;
        }
        T* p = slot(r);
        out = std::move(*p);
        std::destroy_at(p);
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    std::size_t try_pop_n(T* dst, std::size_t n) noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        std::uint64_t avail = cached_write_ - r;
        if (avail < static_cast<std::uint64_t>(n)) {
            cached_write_ = write_.load(std::memory_order_acquire);
            avail = cached_write_ - r;
        }
        const std::size_t count =
            static_cast<std::uint64_t>(n) < avail ? n : static_cast<std::size_t>(avail);
        for (std::size_t i = 0; i < count; ++i) {
            T* p = slot(r + i);
            dst[i] = std::move(*p);
            std::destroy_at(p);
        }
        read_.store(r + count, std::memory_order_release);
        return count;
    }

    // Approximate: safe to call from either side, exact from neither.
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(write_.load(std::memory_order_acquire) -
                                        read_.load(std::memory_order_acquire));
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    T* slot(std::uint64_t i) noexcept {
        return std::launder(reinterpret_cast<T*>(&storage_[(i & kMask) * sizeof(T)]));
    }

    alignas(kLine) std::atomic<std::uint64_t> write_{0};
    std::uint64_t cached_read_{0};            // producer-private

    alignas(kLine) std::atomic<std::uint64_t> read_{0};
    std::uint64_t cached_write_{0};           // consumer-private

    alignas(kLine) alignas(T) std::byte storage_[sizeof(T) * Capacity];
};
```

```cpp Using it for a feed-to-strategy handoff
#include <cstdint>
#include <thread>

struct Tick {
    std::uint64_t recv_ns;
    std::uint32_t instrument_id;
    std::uint32_t qty;
    std::int64_t  price_ticks;
};
static_assert(sizeof(Tick) == 24);

SpscQueue<Tick, 8192> g_ticks;

void feed_thread(const Tick* decoded, std::size_t n) {
    std::size_t done = 0;
    while (done < n)
        done += g_ticks.try_push_n(decoded + done, n - done);   // caller decides on full
}

void strategy_thread(std::atomic<bool>& running) {
    Tick batch[64];
    while (running.load(std::memory_order_relaxed)) {
        const std::size_t got = g_ticks.try_pop_n(batch, 64);
        for (std::size_t i = 0; i < got; ++i) { /* price the book, maybe quote */ }
    }
}
```

:::hft
The `while (done < n)` spin above is a policy decision disguised as a loop. If the queue is
full it means the strategy is not keeping up, and spinning in the feed thread means you stop
draining the NIC, which means the kernel drops packets and you take a feed gap. Most desks
prefer to drop into a slow path instead: count the overflow, publish it to telemetry, and
either shed the message or fall back to a conflated snapshot. The queue tells you the truth;
what you do with a `false` is the interesting engineering.
:::

## Numbers and alternatives

Typical figures on a modern same-socket x86 server, for a 24-byte payload, are a sustained
throughput in the range of 50 to 200 million messages per second when both sides are busy,
and a one-way inter-core handoff latency somewhere between 30 and 80 nanoseconds when both
cores are spinning and the queue is empty. Cross-socket, expect the latency to roughly
triple. Measure on your own hardware before quoting either number: the spread between a
2-socket Cascade Lake and a single-socket Sapphire Rapids is large.

Known-good implementations worth reading and benchmarking against:

| Implementation | Notes |
|---|---|
| rigtorp `SPSCQueue` | Header-only, MIT, the closest published relative of the above; caches both indices and pads properly. The reference point. |
| folly `ProducerConsumerQueue` | Battle-tested, runtime capacity, uses `%`-free wrapping but check whether your version caches the opposite index. |
| `boost::lockfree::spsc_queue` | Portable and correct, with a heavier interface; historically slower than the two above in microbenchmarks. |

If you truly need multiple producers, an MPSC queue costs you a `fetch_add` or CAS on the
write index, which turns a wait-free push into a contended read-modify-write, and gives you
the superlinear degradation from lesson 32. MPMC on top of that needs a per-slot sequence
number, the Vyukov bounded-queue design, which adds a second atomic load and store per
element. A typical penalty is two to four times the SPSC latency at low contention and much
worse at high contention. Before paying it, check whether N SPSC queues plus a small
round-robin drain gives you the same thing for free. Usually it does.

:::exercise
Take the final queue and produce three variants: (a) with `alignas(kLine)` removed from both
indices, (b) with the cached indices removed so both sides always load the other's atomic,
and (c) unchanged. Pin a producer and a consumer to two cores sharing an LLC, push 100
million `Tick`s through each variant, and record messages per second. Then repeat with the
two threads pinned to SMT siblings of the same physical core, and explain why the ranking
changes.
:::

## Takeaways

- SPSC is wait-free precisely because it has one writer per index: no CAS, no loop, bounded
  instruction count on every path.
- Relaxed on your own index, acquire on theirs, release when you publish. Nothing needs
  `seq_cst`.
- Pad the two indices onto separate cache lines, then cache the other side's index locally.
  The second change is worth more than the first.
- Use raw storage with `std::construct_at` and `std::destroy_at` so the queue works for
  types that are not default-constructible or trivially copyable.
- Batch the publish, not just the copy: one release store per batch is the amortisation that
  matters.
- A full queue is information, not an error. Decide the drop policy explicitly, in the feed
  thread, before the market opens.
