---
title: Hot Path Discipline
part: Part VI - Trading Systems
summary: The rules that hold everything else together. What is banned from the critical path and why, how to warm it before the open, and how to make the compiler and CI enforce it instead of a code reviewer's memory.
time: 30 min
level: expert
tags: hot-path, determinism, warm-up, tail-latency, enforcement
---

Everything in this series so far has been a technique. This lesson is the policy that turns
those techniques into a system that stays fast after twenty engineers have worked on it for
three years. The rules below are not subtle and they are not clever. They are a short list of
things you do not do, and the entire value is that they are absolute, because a rule with an
exception is a rule that gets negotiated away in a code review at 6pm on a Friday.

First, define the term. The **hot path**, or critical path, is the code executed between a
market data packet arriving and an order leaving. Not the code that runs often — the code
that runs *in that window*. A function called ten million times an hour during position
reconciliation is not hot. A function called once per tick is.

## The rules

| Banned on the critical path | The failure it prevents |
|---|---|
| Allocation | `malloc` can take a lock, walk a free list, or call `mmap`; the tail is unbounded. |
| Locks | Any contended lock means a syscall and a scheduler round trip, tens of microseconds. |
| Syscalls | A syscall is a mode switch, a TLB and branch-predictor disturbance, and the kernel may not give the core back promptly. |
| Blocking I/O | A page fault on a memory-mapped file or a disk write stalls you for milliseconds. |
| Formatted logging | `printf` and `<format>` parse a format string, allocate, and often lock a stream. |
| Exceptions | Throwing walks unwind tables, may take the global unwinder lock, and costs microseconds. |
| Virtual dispatch | An indirect call the predictor cannot resolve is a pipeline flush, and it blocks inlining across the call. |
| Unbounded loops | Any loop whose iteration count depends on input data has a tail as long as the worst input. |
| Shared mutable state | Every write to a line another core reads costs a coherence round trip, 50-100 ns typically. |

Each of those is covered in depth elsewhere: allocation in lesson 24, locks and atomics in
lessons 31 and 32, virtual dispatch in lesson 23, exceptions and the alternative in lesson 18.
What is new here is treating them as one list with one answer: not on the hot path, ever.

:::key
Notice that almost every entry is about the *tail*, not the mean. `malloc` is 20 ns most of
the time. It is 20 microseconds when it needs a new arena. The rules exist because a system
that is fast on average and occasionally slow is, in trading, just a slow system with good
marketing.
:::

## Preallocate, pre-touch, pin

Everything the hot path will ever touch is allocated during startup, while you are still
allowed to be slow. That includes the book from lesson 37, the ring buffers, the message
staging areas, the log queue, and the strategy's own state.

Allocating is not enough. A fresh page from the kernel is a copy-on-write mapping of the zero
page; the first store to it takes a minor page fault, which is a few microseconds you will
pay on the first message of the day, which is exactly the message you cared about. Touch
every page at startup.

```cpp warmup.hpp
#pragma once
#include <cstddef>
#include <cstdint>

// Force a private, resident, dirty page for every page of the range.
inline void pre_touch(void* p, std::size_t bytes) noexcept {
    auto* b = static_cast<volatile std::uint8_t*>(p);
    for (std::size_t i = 0; i < bytes; i += 4096) b[i] = b[i];
    if (bytes != 0) b[bytes - 1] = b[bytes - 1];
}
```

On Linux, back the big structures with huge pages (`madvise(MADV_HUGEPAGE)` or an explicit
`mmap` with `MAP_HUGETLB`) and lock them down with `mlockall(MCL_CURRENT | MCL_FUTURE)` so
the kernel can never reclaim them. Pin the thread to an isolated core, as lesson 35 covers.
Then never allocate again.

## Warming the path

A cold instruction cache costs you more than any algorithm you are likely to change. The
first execution of your tick handler after an idle period may be five to ten times slower
than the steady-state figure — instruction cache misses, data cache misses, TLB misses, and
a branch predictor with no history for any of your branches.

The fix is to run the real code path with real-shaped data before the open, and again during
any quiet period, with the send suppressed. Not a simplified version — the real path, so that
the real lines and the real branches are the ones that get warmed.

```cpp Warm the code you will actually run
#include <array>
#include <cstddef>
#include <cstdint>

enum class Action : std::uint8_t { None, Quote };

struct Message { std::uint8_t type; std::uint64_t id; std::int64_t px; std::uint32_t qty; };
struct Signal  { Action action; std::int64_t px; std::uint32_t qty; };
struct View    { std::int64_t bid, ask; std::uint64_t seq; };
struct State   { std::int32_t position; std::uint64_t seq; };

// Stand-ins for the components built in lessons 36, 37 and 40.
struct Book     { void apply(const Message&) noexcept {}
                  [[nodiscard]] View top(std::uint64_t s) const noexcept { return {0, 0, s}; } };
struct Strategy { [[nodiscard]] Signal evaluate(const View&) noexcept { return {Action::None, 0, 0}; } };
struct Risk     { [[nodiscard]] bool check(const Signal&) noexcept { return true; } };
struct Encoder  { std::size_t encode(const Signal&, std::byte*) noexcept { return 0; } };
struct Session  { void send(const std::byte*, std::size_t) noexcept {} };

class Engine {
public:
    void on_message(const Message& m) noexcept {
        book_.apply(m);
        const Signal sig = strategy_.evaluate(book_.top(seq_++));
        if (sig.action == Action::None) return;
        if (!risk_.check(sig)) [[unlikely]] return;
        const std::size_t n = encoder_.encode(sig, out_.data());
        if (!suppress_sends_) [[likely]] session_.send(out_.data(), n);
    }

    void warm(std::size_t iterations) noexcept {
        suppress_sends_ = true;
        const State saved = snapshot();        // warming must not leak into live state
        for (std::size_t i = 0; i < iterations; ++i)
            on_message(synthetic_message(i));
        restore(saved);
        suppress_sends_ = false;
    }

private:
    [[nodiscard]] State snapshot() const noexcept { return {position_, seq_}; }
    void restore(const State& s) noexcept { position_ = s.position; seq_ = s.seq; }
    [[nodiscard]] static Message synthetic_message(std::size_t i) noexcept {
        return Message{'A', i, 10'000 + static_cast<std::int64_t>(i % 16), 100};
    }

    Book      book_;
    Strategy  strategy_;
    Risk      risk_;
    Encoder   encoder_;
    Session   session_;
    std::array<std::byte, 64> out_{};
    std::uint64_t seq_{0};
    std::int32_t  position_{0};
    bool suppress_sends_{false};
};
```

:::pitfall
`suppress_sends_` is a runtime branch on the hot path, which the next section argues against.
It earns its place because the alternative — a separate warm-up code path — warms the wrong
instructions and gives you false confidence. Keep it, mark it `[[likely]]`, and make sure the
warm-up cannot mutate live state; a warm-up that leaves a synthetic order in your position is
a bad day.
:::

The other half of staying warm is staying small. A typical L1 instruction cache is 32 KB. If
the whole tick-to-trade path fits, it stays resident between messages and you never re-fetch
it. This is the real argument for the code layout work in lesson 29: it is not that
instruction fetch is slow, it is that a hot path spread across 200 KB of text cannot stay
resident and a hot path packed into 20 KB can.

## Moving work off the path

Most of what looks like necessary work on the hot path is really *bookkeeping about* the hot
path, and bookkeeping can be deferred. The pattern is always the same: on the critical path,
write a small fixed-size record into a wait-free SPSC queue from lesson 33; on a cold thread
pinned to a different core, drain it and do the expensive part.

```cpp Binary logging: 24 bytes and one queue push on the hot path
#include <cstdint>

struct LogRecord {              // no strings, no formatting, no allocation
    std::uint64_t tsc;          // the raw counter value at the call site
    std::uint32_t site_id;      // index into a static table of format strings
    std::uint32_t u32;
    std::int64_t  i64;
};
static_assert(sizeof(LogRecord) == 24);

// Each call site gets a compile-time id; the format string never enters the hot path.
enum : std::uint32_t { kSiteQuoteSent = 0, kSiteRiskReject = 1, kSiteGap = 2 };

template <class Queue>          // the wait-free SPSC queue from lesson 33
void log_quote(Queue& q, std::uint64_t tsc, std::uint32_t qty, std::int64_t px_ticks) noexcept {
    (void)q.try_push(LogRecord{tsc, kSiteQuoteSent, qty, px_ticks});   // drop, never block
}
```

The cold thread turns `site_id` back into text with `std::format`, does the TSC-to-wall-clock
conversion, and writes to disk. The hot path did one store of 24 bytes and one relaxed atomic
increment. Note `try_push` and the discarded return: if the queue is full the record is
dropped. Dropping a log line is always better than delaying an order, and a full queue is
itself a signal worth counting.

The same deferral applies elsewhere. Position and P&L updates can be applied from a queue of
fill records rather than computed inline. Statistics — counts, sums, histograms — can be
plain non-atomic counters read by another thread with a seqlock (lesson 34) rather than
atomics contended on every tick. Anything a human will look at can be late.

Runtime configuration should be resolved at compile time. A `if (config_.enable_audit)` on the
hot path is a load, a branch, and a chunk of cold code sitting between hot code in the
instruction stream. A policy template removes all three.

```cpp Compile-time policy instead of a runtime flag
#include <cstdint>

struct TopOfBook { std::int64_t bid_px, ask_px; std::uint64_t bid_qty, ask_qty; };
struct Quote     { bool act; std::int64_t px; std::uint32_t qty; };

struct Production { static constexpr bool kAudit = false; static constexpr bool kSimulate = false; };
struct Research   { static constexpr bool kAudit = true;  static constexpr bool kSimulate = true;  };

template <class Cfg>
class Quoter {
public:
    [[nodiscard]] Quote evaluate(const TopOfBook& tob) noexcept {
        if constexpr (Cfg::kAudit) record_inputs(tob);          // not compiled in production
        const Quote q = decide(tob);
        if constexpr (Cfg::kSimulate) return Quote{false, 0, 0};  // no code emitted either way
        return q;
    }
private:
    void record_inputs(const TopOfBook& t) noexcept { last_ = t; }
    [[nodiscard]] static Quote decide(const TopOfBook& t) noexcept {
        return Quote{t.ask_px - t.bid_px > 2, t.bid_px + 1, 100};
    }
    TopOfBook last_{};
};
```

Build both binaries in CI so the research path cannot rot.

## Determinism over average speed

There is a real tension in these rules. Branchless code is often slower on average than
branchy code with a well-predicted branch, because the branchless version computes both
sides. A fixed-size loop that always runs 16 iterations is slower on average than one that
exits after 3. Preallocating a worst-case buffer wastes cache that a right-sized one would
not.

You take those trades anyway, because the objective function is not the mean. The value of
being fast is entirely in the messages where being fast changed the outcome, and those arrive
in bursts, at the open, on the news — precisely when your caches are cold, your queues are
deep, and your average-case optimisations are least likely to hold. Optimise the p99.9. Report
the p99.9. If someone shows you a mean, ask for the distribution, as lesson 26 insists.

## Enforcement

A rule nobody can violate is worth ten rules everybody agrees with.

```cpp Make an allocation on the hot path abort, in tests and in staging
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace hot { inline std::atomic<bool> armed{false}; }

void* operator new(std::size_t n) {
    if (hot::armed.load(std::memory_order_relaxed)) {
        std::fputs("FATAL: allocation on the hot path\n", stderr);
        std::abort();                       // a core file names the culprit
    }
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
void  operator delete(void* p) noexcept              { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n)                    { return ::operator new(n); }
void  operator delete[](void* p) noexcept              { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }
```

Arm it around the tick handler in every unit test and every replay run. It costs one relaxed
load in a function the hot path is not supposed to call anyway.

The rest of the enforcement toolkit:

- **Count syscalls.** `strace -c -f ./trader` over a replay should show zero in the steady
  state. For a hard guarantee, install a `seccomp` filter that returns `SIGSYS` for everything
  except the handful you allow after startup.
- **Static analysis.** `clang-tidy` with `cppcoreguidelines-no-malloc`,
  `cppcoreguidelines-owning-memory` and `performance-*` on the hot-path directory, plus
  `-Wpadded` on the message and book types so nobody silently doubles a struct.
- **`-fno-exceptions` on the hot-path translation units.** A `throw` then does not compile,
  which is a much better error than a 3 µs unwind found in production.
- **Latency regression tests in CI.** Replay a fixed recorded feed, emit the histogram, and
  fail the build if p99 or p99.9 rises more than a set percentage against the committed
  baseline. Run it on a dedicated, isolated, pinned machine or the noise will exceed the
  signal.
- **A review checklist**, which is lesson 43, applied to every diff that touches the path.

:::hft
The single highest-value item on that list is the latency regression test, and it is the one
most desks skip because it needs dedicated hardware and a stable feed capture. Without it,
performance decays by one or two percent per change until someone spends a quarter finding
the twenty commits responsible. With it, the commit that costs 200 ns is rejected the day it
is written, by the person who wrote it, who still remembers why.
:::

:::exercise
Take any hot-path component you have written — the book from lesson 37 will do — and arm the
aborting `operator new` around a replay of a hundred thousand messages. Something will fire:
a `std::string` in an error path, a `std::function`, a vector that grows on the first message
of a new symbol. Fix each one, then add a CI job that runs the replay with the allocator armed
and a second job that compares the p99 against a checked-in baseline. Watch how quickly the
second job starts catching things.
:::

## Takeaways

- No allocation, no locks, no syscalls, no I/O, no formatted logging, no exceptions, no
  virtual dispatch, no unbounded loops, no shared mutable state. On the hot path, without
  exceptions.
- Every rule is about the tail. The banned operations are all fast on average and catastrophic
  occasionally.
- Preallocate and pre-touch at startup, then warm the real code path with synthetic messages
  and the send suppressed.
- Push work off the path: fixed-size binary log records into an SPSC queue, deferred
  bookkeeping, lazy statistics, compile-time flags instead of runtime branches.
- Optimise and report p99.9, not the mean. Determinism is worth paying average speed for.
- Enforce mechanically — an aborting `operator new`, a syscall count of zero, `clang-tidy`, and
  a CI job that fails the build on a tail-latency regression.
