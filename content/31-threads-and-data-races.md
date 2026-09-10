---
title: Threads, Races and Why Locks Hurt
part: Part V - Concurrency
summary: What a data race formally is and why it is undefined behaviour, what an uncontended and a contended mutex actually cost, and why fast trading systems mostly arrange never to need one.
time: 35 min
level: advanced
tags: threads, jthread, data-race, mutex, tsan, thread-per-core
---

Concurrency in C++ is two separate subjects that people run together. One is the formal
question of what the language guarantees when two threads touch the same memory, where the
answer is precise and unforgiving. The other is the engineering question of what
synchronisation costs, where the answer is that an uncontended mutex is cheap and a
contended one is a catastrophe for your tail. A trading system needs both answers, and
then it needs the conclusion that follows from them, which is to design so the question
rarely arises.

## Starting a thread, and handing it data

`std::thread` starts a thread of execution immediately on construction. Its destructor
calls `std::terminate` if the thread is still attached, which is a deliberate design choice
to make you decide explicitly between `join` and `detach`.

```cpp Raw std::thread: correct but easy to get wrong
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

std::int64_t total_notional(const std::vector<std::int64_t>& px,
                            const std::vector<std::uint32_t>& qty,
                            std::size_t begin, std::size_t end) {
    std::int64_t sum = 0;
    for (std::size_t i = begin; i < end; ++i)
        sum += px[i] * static_cast<std::int64_t>(qty[i]);
    return sum;
}

int main() {
    const std::vector<std::int64_t>  px(1024, 10'025);
    const std::vector<std::uint32_t> qty(1024, 10);
    std::int64_t a = 0, b = 0;

    std::thread t1([&] { a = total_notional(px, qty, 0, 512); });
    std::thread t2([&] { b = total_notional(px, qty, 512, 1024); });
    t1.join();                      // must join before px, qty, a, b die
    t2.join();
    return static_cast<int>((a + b) != 0);
}
```

C++20's `std::jthread` fixes both problems in that snippet: it joins in its destructor, and
it carries a `std::stop_token` so you have a standard way to ask it to finish. On a hot
path you will pin these threads as in lesson 30, but the lifetime discipline is the same.

```cpp jthread with cooperative cancellation
#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <thread>

struct Ring { std::uint64_t slot; bool full; };   // stands in for lesson 33's SPSC queue

inline bool try_pop(Ring& r, std::uint64_t& out) {
    if (!r.full) return false;
    out = r.slot;
    r.full = false;
    return true;
}
inline void handle(std::uint64_t) {}

void feed_loop(std::stop_token stop, Ring& ring) {
    std::uint64_t tick = 0;
    while (!stop.stop_requested()) {
        if (try_pop(ring, tick)) handle(tick);
    }
}

void run(Ring& ring) {
    std::jthread feed(feed_loop, std::ref(ring));   // stop_token injected first
    std::this_thread::sleep_for(std::chrono::seconds(1));
}   // ~jthread requests stop, then joins
```

Two rules about passing data. Arguments to `std::thread` and `std::jthread` are **copied**
into the new thread's storage, so a reference parameter needs an explicit `std::ref` or
`std::cref` at the call site. And anything captured by reference in a lambda must outlive
the thread, which `jthread` makes tractable because the join happens at the end of the
enclosing scope.

## What a data race actually is

The standard's definition is worth memorising word for word, because every argument about
concurrency reduces to it.

:::key
Two evaluations **conflict** if they access the same memory location and at least one of
them is a write. A program has a **data race** if two conflicting evaluations in different
threads are not ordered by a **happens-before** relation, and neither is an atomic
operation. **A program with a data race has undefined behaviour.**
:::

Three parts of that deserve unpacking. "Same memory location" means the same scalar object
or the same bit-field sequence, not merely the same cache line; two threads writing
adjacent `int`s in a struct is legal, though as lesson 21 showed it is slow. "Ordered by
happens-before" is established by mutexes, atomics, thread creation and join, and by
nothing else. And the consequence is **undefined behaviour**, not "you might read a stale
value".

That last point is the one people get wrong, so here is the concrete failure.

```cpp Not a stale read. Undefined behaviour.
#include <cstdint>

bool g_halt = false;                    // plain bool, no synchronisation

void strategy_loop(std::int64_t& fills) {
    while (!g_halt) {                   // compiler may hoist the load out
        ++fills;
    }
}
```

Because `g_halt` cannot legally be modified by another thread without synchronisation, the
compiler is entitled to load it once before the loop and reuse the value. At `-O2` both GCC
and Clang do exactly that, and the loop becomes `if (!g_halt) for (;;) ++fills;`. Your risk
kill switch sets `g_halt = true` and the strategy keeps trading. No "eventually it will see
it": the load is gone from the machine code.

The fix is a synchronisation primitive, not a `volatile`. `volatile` in C++ means "this
access may have side effects the compiler cannot see"; it constrains elision and reordering
of the volatile accesses themselves but establishes no happens-before relationship with
other memory, so it does not make the surrounding data visible. Lesson 32 covers the
correct tool, `std::atomic`, in detail.

```cpp The minimal correct version
#include <atomic>
#include <cstdint>

std::atomic<bool> g_halt{false};

void strategy_loop(std::int64_t& fills) {
    while (!g_halt.load(std::memory_order_relaxed)) {
        ++fills;
    }
}
```

## What a mutex costs

`std::mutex` on Linux is a futex-backed lock, and it has two completely different
performance regimes.

| Case | Mechanism | Typical cost |
|---|---|---|
| Uncontended lock + unlock | Atomic compare-exchange on a cached line | 15 to 25 ns |
| Contended, brief | Spin, then acquire | Hundreds of ns |
| Contended, blocking | `futex(FUTEX_WAIT)`, deschedule, wake, reschedule | 1 to 10 µs |
| Contended across sockets | Above, plus cache line transfer over the interconnect | Worse still |

The uncontended number is unremarkable: it is one atomic read-modify-write on a line
already in your L1, plus the compiler barriers that stop it moving code across the
boundary. If a mutex is genuinely uncontended, the cost is comparable to a cache miss and
you should stop worrying about it.

The contended number is the problem, and the mean understates it badly for three reasons.

- **The syscall and context switch.** Blocking hands the core to another thread. Coming
  back means a scheduler wakeup, and your L1, L2 and branch predictor state is gone. The
  visible cost is not the microseconds of switching but the cold caches afterwards.
- **The convoy effect.** A thread holding the lock is preempted, or takes a page fault,
  or a cache miss. Every other thread piles up behind it. When it resumes, the queue
  drains in lock-step and the whole system runs at the speed of the slowest holder for a
  while. A brief hiccup in one thread becomes a sustained latency plateau in all of them.
- **Priority inversion.** A low-priority thread holds the lock; a `SCHED_FIFO` hot thread
  wants it and blocks; a medium-priority thread runs and prevents the holder from ever
  finishing. `std::mutex` has no priority inheritance. On a tuned box with real-time
  threads from lesson 30, this is a genuine hang, not a slowdown.

:::perf
The distribution matters more than the average. A mutex that is uncontended 99.9% of the
time has a mean cost of maybe 30 ns and a p99.9 of several microseconds. The mean tells you
the lock is free. The tail tells you it is the largest single item in your latency budget,
and the tail is what the desk measures. Always histogram, never average, as in lesson 26.
:::

## The standard toolbox, used properly

For the cold path, where a lock is entirely appropriate, use the RAII wrappers rather than
`lock()` and `unlock()`.

```cpp Wrapper choice
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <unordered_map>

class InstrumentTable {
public:
    // Many readers, rare writers: reference data changes at a symbol-add.
    std::int64_t tick_size(std::uint32_t id) const {
        std::shared_lock lock(mu_);              // shared ownership
        const auto it = tick_.find(id);
        return it == tick_.end() ? 0 : it->second;
    }

    void add(std::uint32_t id, std::int64_t tick) {
        std::unique_lock lock(mu_);              // exclusive ownership
        tick_[id] = tick;
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::uint32_t, std::int64_t> tick_;
};
```

- `std::lock_guard` locks one mutex for a scope. Cheapest, no extra state, no way to
  unlock early. Default choice.
- `std::scoped_lock` locks **several** mutexes at once using a deadlock-avoidance
  algorithm. Use it whenever you need two locks.
- `std::unique_lock` is a movable, deferrable, early-unlockable guard. Required by
  `condition_variable`. Slightly larger and carries an ownership flag.
- `std::shared_mutex` allows many concurrent readers or one writer. It is **more
  expensive than `std::mutex` even for readers**, because the reader count is itself a
  contended atomic, so it only wins when read critical sections are long. For short reads
  on a hot path, a seqlock (lesson 34) beats it comfortably.

`condition_variable` must always be used in the predicate form, because a wait can return
spuriously and because a notification that arrives before you wait must not be lost.

```cpp Handing work to a cold-path thread
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>

struct FillRecord { std::uint64_t order_id; std::int64_t price_ticks; };

class FillJournal {                 // cold path: writes fills to disk
public:
    void post(FillRecord r) {
        {
            std::lock_guard lock(mu_);
            queue_.push_back(r);
        }
        cv_.notify_one();           // notify outside the lock
    }

    void drain(std::stop_token stop) {
        std::unique_lock lock(mu_);
        while (!stop.stop_requested()) {
            // Predicate form: rechecks after every wake, no lost wakeups.
            cv_.wait(lock, [&] { return !queue_.empty() || stop.stop_requested(); });
            while (!queue_.empty()) {
                const FillRecord r = queue_.front();
                queue_.pop_front();
                lock.unlock();
                write_to_disk(r);
                lock.lock();
            }
        }
    }

private:
    static void write_to_disk(const FillRecord&) {}
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<FillRecord> queue_;
};
```

:::pitfall
`cv_.wait(lock, pred)` with a `stop_token` in the predicate still misses a stop request
that arrives while you are already blocked. The complete solution is
`std::condition_variable_any::wait(lock, stop_token, pred)`, which registers a stop
callback that notifies for you. The version above relies on `post` also being called, which
is fine for a journal that is drained at shutdown and wrong for one that is not.
:::

Deadlock has exactly one general cure: **acquire locks in a globally consistent order**.
Two threads that both need the bid book and the ask book must take them in the same order
every time, and the ordering must be a property of the objects, not of the call site.

```cpp Two ways out of a lock-ordering deadlock
#include <mutex>

struct Side { std::mutex mu; };

// 1. Let the library do it. scoped_lock uses a deadlock-avoiding algorithm.
void cross_both(Side& bid, Side& ask) {
    std::scoped_lock lock(bid.mu, ask.mu);
    // both held, in a safe order chosen by the implementation
}

// 2. Impose a total order yourself when you cannot lock simultaneously.
void ordered(Side& a, Side& b) {
    Side* first  = &a < &b ? &a : &b;
    Side* second = &a < &b ? &b : &a;
    std::lock_guard g1(first->mu);
    std::lock_guard g2(second->mu);
}
```

## What fast trading systems do instead

Everything above is the correct way to use locks. The architectural observation is that the
fastest systems mostly do not use them on the hot path at all, not because locking is
badly implemented but because a design that needs a lock has already given up
determinism. Four patterns replace it.

**Single writer.** Every piece of mutable state has exactly one thread that writes it.
Readers observe published snapshots. A single-writer design has no write-write conflicts by
construction, and the read side can be made wait-free with a seqlock (lesson 34) or an
atomic pointer swap. The order book is written only by the feed thread. Positions are
written only by the fill handler.

**Thread per core with sharded state.** Instead of many threads sharing one book, run one
thread per core, each pinned as in lesson 30, each owning a disjoint set of instruments.
Instrument 4821 lives on core 5 and nowhere else, so there is no sharing and therefore
nothing to synchronise. Sharding by instrument or by exchange is usually natural, and when
it is, it removes the problem rather than solving it.

**Share by communicating, not by sharing memory.** Where threads must interact, pass
messages through a single-producer single-consumer lock-free queue rather than sharing a
structure behind a mutex. The queue is a bounded ring buffer of trivially copyable
messages, and a well-written SPSC push and pop cost on the order of 20 to 50 ns with no
syscall and no possibility of blocking. That is lesson 33 in full.

```cpp Message passing instead of shared state
#include <cstdint>

// Sent by the strategy thread, consumed by the order-gateway thread.
// Trivially copyable, fixed size, no pointers into the sender's memory.
struct alignas(64) NewOrder {
    std::uint64_t client_id;
    std::uint32_t instrument;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint8_t  side;         // 0 = buy, 1 = sell
    std::uint8_t  pad[3];
};
static_assert(sizeof(NewOrder) == 64);
```

**Copy instead of lock.** For small state read on the hot path, copying is often cheaper
than any synchronisation. A 32-byte risk-limit struct that changes twice a day can be
published by an atomic pointer swap into a double-buffer, and read with a relaxed load and
a copy. The read side pays a load and 32 bytes of copying, which is nanoseconds, against a
`shared_lock` at tens of nanoseconds with a contended atomic in it.

And the counterweight: **a lock is perfectly acceptable where latency does not matter.**
Startup and configuration loading, the reference-data table read once per session, the
metrics thread, the journal writer, the admin socket, the shutdown path. Using a mutex
there is correct, obvious, reviewable code, and replacing it with a lock-free structure
buys nothing and costs you a class of subtle bugs. The discipline is not "never lock", it
is "know which side of the line you are on".

:::hft
The uncomfortable version of this: on a hot path, the cost of a mutex is not the 20 ns of
the uncontended case. It is that the mutex makes your latency **conditional on another
thread's schedule**. A tick-to-trade path that can block is a path whose p99.9 is set by
whatever else the operating system decided to do, which is precisely the thing lesson 30
spent a whole session trying to eliminate. A desk will accept a slower mean in exchange for
a path that has no blocking operation in it anywhere, because the number being traded on is
the tail.
:::

## Finding races that code review will not

Data races are invisible in review, non-deterministic in testing and reproduce once a
month in production. **ThreadSanitizer** finds them by instrumenting every memory access
and maintaining the happens-before relation at run time. It reports races that did not
manifest as wrong answers on that run, which is exactly what you need.

```sh
$ g++ -std=c++23 -O1 -g -fsanitize=thread race.cpp -o race
$ ./race
==================
WARNING: ThreadSanitizer: data race (pid=4711)
  Write of size 8 at 0x55e4c8a0 by thread T2:
    #0 Book::apply_quote(Quote const&) book.cpp:41
  Previous read of size 8 at 0x55e4c8a0 by thread T1:
    #0 Book::best_bid() const book.cpp:27
  Location is global 'g_book' of size 96 at 0x55e4c8a0
==================
```

Practicalities. TSan costs roughly 5 to 15 times the run time and 5 to 10 times the memory,
so it is a CI configuration, not a production one. It cannot be combined with
AddressSanitizer, so run two builds. It only reports races on code paths that actually
execute, which means your concurrency tests must genuinely exercise the interleavings, and
a soak test with randomised message ordering finds far more than a unit test. And it does
not understand hand-written inline assembly or `asm volatile` barriers, so lock-free code
from lessons 32 to 34 needs `__tsan_acquire`/`__tsan_release` annotations or explicit
suppressions to avoid a flood of false positives.

:::exercise
Write a program with two `std::jthread`s that both increment a shared plain `std::int64_t`
one million times, then print the total. Run it at `-O2` and record the answer. Now build
the same source with `-fsanitize=thread` and confirm TSan reports the race even on a run
where the total happened to be correct. Finally, take the `g_halt` loop from earlier,
compile it at `-O2` with `-S`, and find the point in the assembly where the load
disappeared from the loop body.
:::

## Takeaways

- `std::jthread` joins in its destructor and carries a `stop_token`. Prefer it to
  `std::thread` everywhere, and remember arguments are copied unless you write `std::ref`.
- A data race is two conflicting accesses not ordered by happens-before, and it is
  undefined behaviour. The compiler may delete your loop's load entirely, so "it will
  eventually see the flag" is false.
- An uncontended mutex costs roughly 15 to 25 ns, which is fine. A contended one costs a
  futex syscall, a context switch and cold caches, typically microseconds, plus convoy and
  priority-inversion effects that hit the tail hardest.
- Use `lock_guard` by default, `scoped_lock` for two mutexes, `unique_lock` with
  `condition_variable`, and always the predicate form of `wait`.
- Fast systems avoid the problem rather than solving it: single writer, thread per core
  with sharded state, SPSC message passing, and copying small state instead of locking it.
  On the cold path, startup and configuration and journals, just use a mutex and move on.
- Run ThreadSanitizer in CI against a soak test. It finds races on runs that produced the
  right answer, which no amount of review does.
