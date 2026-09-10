---
title: Error Handling Without Exceptions
part: Part II - Modern C++23
summary: What a throw actually costs, what noexcept changes in the generated code, and a written error policy for a trading system where the hot path never branches on failure.
time: 30 min
level: advanced
tags: exceptions, noexcept, expected, assert, policy
---

Ask a room of C++ programmers whether exceptions are slow and you will get a fight. Both
sides are wrong the same way: they argue about throughput when the question is about
distribution. An exception costs nothing until it is thrown, and when it is thrown it costs
an amount you cannot predict. On a hot path the second half is what disqualifies it.

## What a throw actually costs

Modern implementations use **zero-cost**, or table-driven, exception handling. Alongside
your function the compiler emits tables in cold sections of the binary: `.eh_frame` says how
to restore registers and unwind one frame, `.gcc_except_table` says which instruction ranges
are covered by which handlers and which objects need destroying. On the path where nothing
throws those tables are never read. No register to set, no flag to check, no entry cost.
"Free when not taken" is literally true for instruction count.

They are not free for size. The tables inflate the binary, and the landing pads, the small
blocks that run destructors during unwinding, sit in `.text` interleaved with real code
unless the compiler moves them out. That competes for instruction cache, a real latency cost
covered in lesson 29.

The throw path is where the numbers go bad:

1. `__cxa_allocate_exception` allocates the exception object: a call into the runtime,
   backed by `malloc` with an emergency static pool as fallback.
2. `_Unwind_RaiseException` begins a two-phase walk. Phase one searches up the stack for a
   handler, decoding frame description entries as it goes. Phase two walks again, running
   destructors.
3. Decoding those tables means a binary search over a sorted index and then interpreting a
   bytecode. On libgcc, locating frame data in a dynamically linked object has historically
   taken a global lock, so two threads throwing at once serialise.

:::perf
A throw-and-catch across a handful of frames typically costs 1 to 3 microseconds on a
modern x86-64 Linux server, and considerably more the first time, when the unwind tables
are cold and have to be faulted in from the page cache. Measure it on your own binary with
the benchmark in lesson 27; the number scales with frame count and with how much debug
information the linker produced.
:::

One to three microseconds is not catastrophic in isolation. The problem is the variance. A
two-microsecond tick-to-trade budget is destroyed by a single throw, and the throw happens
exactly when the market is doing something unusual, which is exactly when latency matters
most. The framing is not "exceptions are slow" but **exceptions convert a rare condition
into an unbounded pause**, and unpredictability is what a trading system cannot absorb.

## noexcept: a promise with teeth

`noexcept` declares that a function will not propagate an exception. It is not a hint. If an
exception tries to escape one, the runtime calls `std::terminate` immediately, without
unwinding, and the process dies with the stack intact.

It changes generated code in three places.

**Unwind path elision.** A call to a `noexcept` function needs no landing pad in the caller,
so the caller's tables shrink and the interleaved cleanup blocks vanish. For a small
function that is often the difference between being inlined and not.

**Move-versus-copy in `std::vector`.** On reallocation a vector must leave the source valid
if an element operation throws, which it can only guarantee by moving when moving cannot
throw, so it calls `std::move_if_noexcept`. A type whose move constructor is not `noexcept`
gets **copied** on every growth.

```cpp
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

struct OrderSlow {
    std::int64_t price_ticks;
    std::string  client_tag;                       // move ctor is noexcept in practice
    OrderSlow(OrderSlow&& o)                       // but ours is not declared so
        : price_ticks(o.price_ticks), client_tag(std::move(o.client_tag)) {}
};

struct OrderFast {
    std::int64_t price_ticks;
    std::string  client_tag;
    OrderFast(OrderFast&& o) noexcept
        : price_ticks(o.price_ticks), client_tag(std::move(o.client_tag)) {}
};

static_assert(!std::is_nothrow_move_constructible_v<OrderSlow>);
static_assert( std::is_nothrow_move_constructible_v<OrderFast>);
```

Growing a vector of one million `OrderSlow` copies every string on every reallocation;
`OrderFast` moves them. On a typical server that is roughly 40 milliseconds against roughly
4 for the whole sequence of reallocations, dominated by the string copies.

**Destructors and the defaults.** Destructors are implicitly `noexcept`, and so are
implicitly-declared move operations when every member's is. The rule that follows is short:
write `noexcept` on move constructors, move assignment, swap and destructors, and let
`static_assert` prove it.

:::pitfall
`noexcept` is not a licence to skip error handling; it declares that errors leave by another
door. A `noexcept` function calling `std::vector::push_back` terminates on allocation
failure. That may be exactly what you want on a hot path, but decide it deliberately rather
than discovering it in production.
:::

## Turning exceptions off

`-fno-exceptions` makes `throw` a compile error and removes the tables entirely. You give up
more than you might expect:

- Every standard library function that reports failure by throwing now calls
  `std::terminate` instead. `vector::at` out of range, `std::stoi` on garbage,
  `std::optional::value` on an empty optional and every allocation failure become process
  death.
- `dynamic_cast` to a reference type reports failure by throwing `std::bad_cast`, so it is
  unusable. Cast to a pointer and check for null.
- Third-party headers containing `throw` will not compile, which is why most desks do not
  use the flag on the whole build.

The common middle position is to build with exceptions enabled so libraries work, and to
enforce by policy and by `noexcept` that nothing on the hot path can throw.

## Return-based errors

If failure is not exceptional, return it. An error **code** is a small enum returned
alongside or instead of the result: predictable, one register, and easy to ignore, which
`[[nodiscard]]` mostly fixes. `std::expected<T, E>` from `<expected>`, introduced in lesson
15, is the modern shape: a value or an error in one return, with monadic composition so the
happy path stays flat.

```cpp
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

enum class DecodeError : std::uint8_t { Truncated, BadChecksum, UnknownType, StaleSeq };

struct TradeMsg {
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t seq;
};

[[nodiscard]] std::expected<TradeMsg, DecodeError>
decode_trade(std::span<const std::byte> buf, std::uint32_t last_seq) noexcept {
    if (buf.size() < sizeof(TradeMsg))
        return std::unexpected(DecodeError::Truncated);

    TradeMsg m{};
    std::memcpy(&m, buf.data(), sizeof m);          // layout audited in lesson 36

    if (m.seq <= last_seq)
        return std::unexpected(DecodeError::StaleSeq);
    return m;
}
```

For a type this small the return is a register pair plus a discriminant, and the error path
is an ordinary branch the predictor learns is never taken. Same information as a throw,
bounded cost, no tables.

:::note
`std::expected` puts the value and the error in the same storage, so
`sizeof(std::expected<TradeMsg, DecodeError>)` is `sizeof(TradeMsg)` plus alignment for the
flag. For a large `T` in a hot function, a status code plus an out-pointer can generate
tighter code. Look at the assembly before assuming either way.
:::

## Assertions and the boundary

`assert` from `<cassert>` evaluates its expression, aborts with a message if it is false,
and compiles to nothing when `NDEBUG` is defined. That switch is the whole design:
assertions state facts that are true by construction, so a release build has nothing to
check. The useful discipline is a third mode. Ship a **checked build** with assertions on,
run the replay harness and the integration environment against it, and ship the release
build to production with them compiled out.

```cpp
#include <cassert>
#include <cstdint>
#include <cstdlib>

// Always-on, even under NDEBUG. For invariants whose violation means the process
// is already corrupt and must not continue.
#define HFT_FATAL_IF(cond)                                      \
    do { if (cond) [[unlikely]] { std::abort(); } } while (0)

// Debug and checked builds only. For facts the boundary has already validated.
#define HFT_ASSERT(cond) assert(cond)

struct Order { std::int64_t price_ticks; std::uint32_t qty; };

// Boundary: called once per inbound message. Validates everything.
[[nodiscard]] bool accept(const Order& o, std::int64_t tick_size) noexcept {
    if (o.qty == 0) return false;
    if (o.price_ticks <= 0) return false;
    if (o.price_ticks % tick_size != 0) return false;
    return true;
}

// Hot path: assumes the boundary did its job. Zero checks at -O2 -DNDEBUG.
std::int64_t notional(const Order& o) noexcept {
    HFT_ASSERT(o.qty != 0);
    HFT_ASSERT(o.price_ticks > 0);
    return o.price_ticks * static_cast<std::int64_t>(o.qty);
}
```

This is what makes the whole policy work. **Validation happens once, at the edge, where a
message arrives from the network or a human.** Everything downstream may assume validity,
which is what lets the hot path be branch-free, and the assertions prove in the checked
build that the assumption holds. Lesson 17's `[[assume]]` is the aggressive version of the
same idea, and belongs only on a condition that also carries an `HFT_ASSERT`.

## A written policy

Put this in the repository, not in someone's head.

1. **The boundary validates.** Every field from a socket, a file, a config or an operator is
   range-checked exactly once at the point of entry and rejected with a returned error.
   Downstream code never re-checks.
2. **The hot path asserts.** Tick-to-trade functions are `noexcept`, return values or codes
   rather than throwing, and use `HFT_ASSERT` to document what the boundary guaranteed. They
   allocate nothing (lesson 24) and log nothing inline.
3. **The cold path may throw.** Startup, config parsing, reference data loading, end-of-day
   reconciliation. Latency is irrelevant there and exceptions with RAII are the clearest way
   to handle failure. Catch at the top of the subsystem.
4. **Impossible states terminate loudly.** A crossed book, an impossible position, a sequence
   number that went backwards after gap recovery. `HFT_FATAL_IF`, capture a
   `std::stacktrace` in the terminate handler, and die.
5. **Expected errors are not errors.** A stale sequence number, a momentarily empty book, a
   rejected order. These are `std::expected` returns or status codes, handled in the flow,
   counted, never logged a line at a time.

:::hft
Rule 4 is the one people argue about, and the argument is always "surely we should keep
trading". No. The worst incidents on a desk are not the ones where a process died; those are
noisy, obvious and quickly failed over. They are the ones where a process kept running with
a corrupted book and quoted into it for eleven minutes. Terminate is a risk control. Make
the restart path fast and the decision is easy.
:::

### The logging corollary

Rule 2 says the hot path logs nothing inline, and this is where good policies die. The
moment a rare condition occurs the instinct is to write a line about it, and that line does
string formatting, takes a mutex and calls `write`. You have replaced an exception's
unpredictable microseconds with a logger's.

```cpp
#include <atomic>
#include <cstdint>

enum class Event : std::uint16_t { StaleSeq, GapDetected, BookCrossed, RejectSent };

struct LogRecord {                    // 24 bytes, trivially copyable, no strings
    std::uint64_t tsc;
    std::uint64_t arg;
    Event         ev;
};

// Hot path: publish a fixed-size record and return. No formatting, no I/O, no lock.
// The queue is the wait-free SPSC ring of lesson 33; a separate thread formats and writes.
void log_event(Event ev, std::uint64_t arg) noexcept;

// Counters cost one increment and are usually enough on their own.
inline std::atomic<std::uint64_t> stale_seq_count{0};
```

Record a timestamp, an event enum and one integer argument, push it into a lock-free queue,
and let an unpinned consumer on a spare core turn it into text. Often you do not need the
queue at all: a per-event counter sampled once a second tells you everything the log line
would have, for one increment.

:::exercise
Benchmark a function returning `std::expected<TradeMsg, DecodeError>` one million times at a
1-in-10000 error rate, and an equivalent that throws at the same rate. Report the median,
99th percentile and maximum for both, not the mean. Then rerun at 1-in-10. Watch what
happens to each tail, and decide which of the two distributions you could put a budget
around.
:::

## Takeaways

- Exceptions cost nothing on the non-throwing path and typically 1 to 3 microseconds when thrown on a modern x86-64 server. The problem is the variance, not the mean.
- `noexcept` is a promise enforced by `std::terminate`. It elides unwind paths and decides whether `std::vector` moves or copies your type on reallocation.
- Validate once at the boundary and let the hot path assume and assert. That is what makes a branch-free hot path honest rather than reckless.
- `std::expected` or status codes for expected failures, exceptions only in cold startup and config code, immediate termination for impossible states.
- Hot-path error reporting is a fixed-size record pushed to a queue, or an integer counter. Never a formatted line, never inline I/O.
