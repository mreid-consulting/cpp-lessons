---
title: Branches, Prediction and [[likely]]
part: Part III - The Machine
summary: What a mispredicted branch costs, what the predictor can and cannot learn, and the honest trade-off between branchy and branchless code.
time: 35 min
level: advanced
tags: branches, prediction, likely, branchless, cmov
---

An `if` statement looks like nothing. It is one comparison and one jump. On a modern
out-of-order core it is either free or it costs you the equivalent of a last-level cache
hit, and which of those two you get depends entirely on whether the CPU guessed right.
That is a strange property for a language construct to have, and it is the reason hot-path
code is written the way it is.

## The pipeline and why it must guess

A modern x86 core does not execute one instruction at a time. It has a deep pipeline —
roughly fifteen to twenty stages on recent Intel and AMD server parts — and it keeps
several hundred instructions in flight in various states of completion. To keep that
machinery fed it must fetch instructions long before it knows the result of the compare
that decides where to fetch from.

So it guesses. This is **speculative execution**: the core predicts which way a branch
will go, fetches and executes down that path, and holds the results in a state where they
can be thrown away. If the guess was right, the speculative work is committed and nothing
was lost. If the guess was wrong, everything after the branch is squashed and fetch
restarts from the correct target.

```text
predicted taken:   cmp | jne | fetch A | decode A | exec A | ... commit
actual: not taken            ^-- squash everything from here, refill from B
```

:::key
A correctly predicted branch costs approximately zero — it does not even occupy an
execution slot in most cases. A mispredicted branch costs a full pipeline refill,
typically **15 to 20 cycles** on a modern x86 server core, which is around 5 ns at
3.5 GHz. Two branches are not twice as expensive as one; a predictable branch and an
unpredictable branch are in different cost classes entirely.
:::

## What the predictor learns, and where it fails

You do not need the microarchitectural detail, but you do need the shape of it.

The predictor is a **history-based** machine. It hashes the address of the branch together
with a record of the recent global taken/not-taken pattern, and looks that up in a table of
saturating counters. Modern designs are TAGE-like: several tables indexed with different
history lengths, and the one with the longest history that has a confident entry wins. This
means the predictor can learn genuinely complex patterns — a branch that alternates, a
branch that is taken on every fourth iteration, a branch correlated with an earlier branch.

Three consequences matter for the code you write.

1. **A branch with a stable outcome is free.** A bounds check that never fires, a null
   check that never fires, a `if (unlikely_error)` — the predictor pins these at close to
   100% accuracy and they cost nothing measurable.
2. **The tables are finite.** A few thousand entries. A hot loop containing thousands of
   distinct branches, or a huge switch dispatched from many sites, will evict its own
   history. Code size is a branch-prediction concern, not only an instruction-cache one.
3. **Data-dependent branches on random data cannot be predicted.** No amount of history
   helps if the answer is a coin flip. This is the case that costs you.

For an indirect branch — a virtual call, a function pointer, a computed jump — the relevant
structure is the **branch target buffer** (BTB), which caches the last target address seen
at that call site. A call site that always reaches the same target predicts perfectly. One
that alternates between four implementations does not, and lesson 23 is about that.

### The sorted array experiment

The canonical demonstration. Two runs of the same code over the same data, differing only
in whether the array was sorted first.

```cpp sorted_vs_unsorted.cpp
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <print>
#include <random>
#include <vector>

std::int64_t sum_large(const std::vector<std::int64_t>& px, std::int64_t threshold) {
    std::int64_t total = 0;
    for (std::int64_t p : px) {
        if (p >= threshold) {          // the branch under test
            total += p;
        }
    }
    return total;
}

int main() {
    std::mt19937_64 rng{42};
    std::vector<std::int64_t> px(1 << 20);
    for (auto& p : px) p = static_cast<std::int64_t>(rng() % 200'000);

    auto run = [&](const char* label) {
        auto t0 = std::chrono::steady_clock::now();
        std::int64_t s = 0;
        for (int rep = 0; rep < 100; ++rep) s += sum_large(px, 100'000);
        auto t1 = std::chrono::steady_clock::now();
        std::print("{:10}: {:6} ms  (checksum {})\n", label,
                   std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(), s);
    };

    run("unsorted");
    std::ranges::sort(px);
    run("sorted");
}
```

On a typical Skylake-class or Zen-class server core the sorted run is roughly three to six
times faster, and the machine is doing exactly the same arithmetic in both cases. Sorted,
the branch is taken for a long run and then not taken for a long run, so the predictor is
right on all but one iteration per run boundary. Unsorted, the comparison is essentially a
coin flip and you eat a mispredict on about half of a million iterations.

:::warn
Compilers are increasingly willing to turn this particular loop branchless on their own, at
which point both runs are fast and the effect vanishes. If you do not see the gap, check the
assembly for `cmov` and, to reproduce the classic result, wrap the accumulate in something the
compiler cannot flatten, such as a call it cannot see through.
:::

## `[[likely]]`, `[[unlikely]]` and what they really do

C++20 gave us two attributes that everybody misreads.

```cpp hot_path.cpp
#include <cstdint>

struct Quote { std::int64_t bid_ticks, ask_ticks; std::uint32_t bid_qty, ask_qty; };

bool validate(const Quote& q) noexcept;
void reject(const Quote& q) noexcept;
void send_order(const Quote& q) noexcept;

void on_quote(const Quote& q) noexcept {
    if (!validate(q)) [[unlikely]] {
        reject(q);                 // cold: moved out of line
        return;
    }
    send_order(q);                 // hot: stays contiguous
}
```

These attributes **do not tell the CPU anything**. There is no hint bit in the instruction
encoding that any current x86 predictor consults; the branch hint prefixes from the
Pentium 4 era are ignored. What the attributes do is inform the compiler's block layout.

The compiler arranges basic blocks so that the likely path falls through in a straight line
and the unlikely path becomes a forward jump to a block placed at the end of the function,
or in a separate cold section entirely with `-freorder-blocks-and-partition`. The wins are:

- The hot path is **contiguous in the instruction cache**. Fewer 64-byte lines touched per
  execution, fewer instruction-TLB entries, better use of the fetch bandwidth.
- Cold code, including error handling and logging, is not interleaved with hot code, so it
  does not pollute the lines you actually run.
- The static prediction and the front end both prefer fall-through, which helps marginally
  on the very first execution before any history exists.

The GCC and Clang spelling that predates the attributes, and which additionally lets you
state a probability, is:

```cpp
#include <cstdint>

// __builtin_expect(expr, expected_value) returns expr.
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

bool risk_ok(std::int64_t notional) noexcept;

inline bool gate(std::int64_t notional) noexcept {
    if (UNLIKELY(!risk_ok(notional))) return false;
    return true;
}

// GCC 9+ / Clang 19+: state the probability explicitly, 0.0 to 1.0.
inline bool gate2(std::int64_t notional) noexcept {
    return __builtin_expect_with_probability(risk_ok(notional), true, 0.98);
}
```

`__builtin_expect_with_probability` matters when the branch is skewed but not overwhelmingly
so. A plain `__builtin_expect` is treated as roughly 90 to 99 percent depending on the
compiler, which can cause over-aggressive cold-path splitting for a branch that really only
goes one way 70 percent of the time.

:::hft
Annotate exactly three kinds of branch: risk-check failures, malformed or unexpected packets
in a feed handler, and error returns. All three are genuinely rare in production and all three
drag verbose, register-hungry code into your hot path if you let the compiler interleave them.
Annotating a branch that is actually 60/40 is worse than annotating nothing, because you have
now told the compiler to pessimise the path you take four times in ten. If you are not sure of
the ratio, measure it with a counter before you annotate.
:::

## Making the branch disappear

When a branch is genuinely unpredictable, the fix is to remove it. Several techniques, in
rough order of how often they are the right answer.

### Conditional move and the min/max idiom

```cpp branchless_clamp.cpp
#include <algorithm>
#include <cstdint>

// Clamp an aggressive limit price into the band the risk system allows.
// std::min/std::max compile to cmov or to a single min/max instruction; no branch.
constexpr std::int64_t clamp_price(std::int64_t px, std::int64_t lo, std::int64_t hi) noexcept {
    return std::min(std::max(px, lo), hi);
}
```

### Arithmetic masking

Build a mask of all-ones or all-zeros from the condition and use it to select. This works
where `cmov` will not, for example when you want to select between two whole computations
or to accumulate conditionally without a data-dependent jump.

```cpp masking.cpp
#include <cstdint>
#include <span>

// Sum only the quantities at or above a threshold price, without branching.
std::uint64_t sum_qty_above(std::span<const std::int64_t> px,
                            std::span<const std::uint32_t> qty,
                            std::int64_t threshold) noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < px.size(); ++i) {
        // (px[i] >= threshold) is 0 or 1; negating gives 0 or all-ones.
        const std::uint64_t mask = static_cast<std::uint64_t>(-(px[i] >= threshold));
        total += qty[i] & mask;
    }
    return total;
}
```

### Table lookup indexed by the condition

```cpp table_lookup.cpp
#include <array>
#include <cstdint>

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

// Signed direction without an if: +1 for a buy, -1 for a sell.
constexpr std::int64_t signed_dir(Side s) noexcept {
    constexpr std::array<std::int64_t, 2> kDir{+1, -1};
    return kDir[static_cast<std::size_t>(s)];
}

// Position delta for a fill, still branch-free.
constexpr std::int64_t position_delta(Side s, std::uint32_t qty) noexcept {
    return signed_dir(s) * static_cast<std::int64_t>(qty);
}
```

### Unconditional store of a possibly unchanged value

If the branch exists only to avoid a store, do the store anyway. A store to a line already
in L1 and already dirty is close to free, and it is certainly cheaper than a mispredict.

```cpp unconditional_store.cpp
#include <algorithm>
#include <cstdint>

struct BestPrices { std::int64_t best_bid, best_ask; };

// Branchy: if (px > b.best_bid) b.best_bid = px;
// Branchless: always write, write the same value when nothing changed.
inline void observe_bid(BestPrices& b, std::int64_t px) noexcept {
    b.best_bid = std::max(b.best_bid, px);
}
```

### The honest counterpoint

Branchless code is not free and it is not always faster.

A branch that the predictor gets right is executed at zero cost and, crucially, the CPU
**does not do the work on the path not taken**. A branchless version evaluates both sides,
always. Worse, `cmov` and mask-select turn a control dependency into a **data dependency**:
the result is not available until both inputs and the condition have resolved, which
lengthens the critical dependency chain and cannot be hidden by speculation.

| Situation | Prefer |
|---|---|
| Condition true or false >95% of the time | Branch, annotated |
| Condition close to random | Branchless |
| One side is expensive to compute | Branch, even if unpredictable |
| Inside a loop the compiler could vectorise | Branchless (see lesson 25) |
| Condition is loop-invariant | Hoist it out entirely |

:::perf
The rule of thumb on a modern server core: if the mispredict rate is above roughly 5 to 10
percent and both sides are cheap, branchless wins. Below that, the branch wins. Both numbers
move with the specific microarchitecture and with how long the dependency chain already is,
so this is a hypothesis to test with `perf stat -e branch-misses`, not a law.
:::

## Loops, switches and impossible cases

The cheapest branch is the one you evaluate once instead of a million times.

```cpp unswitch.cpp
#include <cstdint>
#include <span>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

// Bad: the side test is invariant but re-evaluated every iteration.
std::uint64_t depth_slow(std::span<const Level> lv, bool is_bid, std::int64_t limit) noexcept {
    std::uint64_t q = 0;
    for (const Level& l : lv) {
        if (is_bid ? (l.price_ticks >= limit) : (l.price_ticks <= limit)) q += l.qty;
    }
    return q;
}

// Good: two clean loops, each with one predictable comparison and no invariant test.
std::uint64_t depth_fast(std::span<const Level> lv, bool is_bid, std::int64_t limit) noexcept {
    std::uint64_t q = 0;
    if (is_bid) {
        for (const Level& l : lv) if (l.price_ticks >= limit) q += l.qty;
    } else {
        for (const Level& l : lv) if (l.price_ticks <= limit) q += l.qty;
    }
    return q;
}
```

This transformation is **loop unswitching**, and GCC and Clang do it at `-O3` when the
condition is provably invariant and the body is small. They give up when the body is large,
when the condition involves a call they cannot see through, or when duplicating the loop
would blow their code-size budget. Doing it by hand costs you a few lines and removes all
doubt. A template parameter or `if constexpr` achieves the same thing without duplicating the
source, which is the pattern lesson 13 covers.

### Switches: jump table or if-chain

A `switch` is not one thing. The compiler picks a strategy based on the case values.

- **Dense case values** — say `0` through `7` — become a **jump table**: bounds-check, load
  an address from a table, indirect jump. One indirect branch, predicted through the BTB.
  Excellent when the target is stable, poor when the message type sequence is random.
- **Sparse case values** — say `1`, `47`, `1200` — become a **binary search over compares**,
  a tree of predictable direct branches, or a chain of compares if there are only two or
  three cases.
- **A few cases with one very common value** — the compiler may peel that value out into a
  leading compare and handle the rest as a table.

For a market-data feed handler with a dense message-type space, the jump table is what you
want, and you get it by keeping the enumerator values contiguous starting at zero.

```cpp dispatch.cpp
#include <cstddef>
#include <cstdint>
#include <utility>

enum class MsgType : std::uint8_t { Add = 0, Modify = 1, Cancel = 2, Trade = 3, Count = 4 };

void on_add(const std::byte*) noexcept;
void on_modify(const std::byte*) noexcept;
void on_cancel(const std::byte*) noexcept;
void on_trade(const std::byte*) noexcept;

void dispatch(MsgType t, const std::byte* payload) noexcept {
    switch (t) {
        case MsgType::Add:    on_add(payload);    break;
        case MsgType::Modify: on_modify(payload); break;
        case MsgType::Cancel: on_cancel(payload); break;
        case MsgType::Trade:  on_trade(payload);  break;
        case MsgType::Count:  std::unreachable();
    }
}
```

:::asm
Verify which form you got. Build with `-S -masm=intel` and look for an indirect jump through
a table, `jmp QWORD PTR [rax*8+.L4]`, versus a run of `cmp`/`je` pairs. If you expected a
table and got a chain, your case values are not dense enough, or one case body was large
enough that the compiler decided otherwise.
:::

### Deleting impossible cases

C++23 gives you two ways to promise the compiler that something cannot happen, so it can
stop emitting the branch that checks.

```cpp assume.cpp
#include <cstdint>
#include <utility>

// std::unreachable(): reaching this line is undefined behaviour.
// The compiler drops the default case and the range check on the jump table.
constexpr const char* side_name(int side) noexcept {
    switch (side) {
        case 0: return "BUY";
        case 1: return "SELL";
        default: std::unreachable();
    }
}

// [[assume(expr)]]: C++23. The compiler may assume expr is true here.
// Lets it drop the bounds check and, often, vectorise the loop cleanly.
std::int64_t vwap_num(const std::int64_t* px, const std::uint32_t* qty, std::size_t n) noexcept {
    [[assume(n % 8 == 0)]];
    [[assume(n > 0)]];
    std::int64_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) acc += px[i] * static_cast<std::int64_t>(qty[i]);
    return acc;
}
```

:::pitfall
`[[assume]]` and `std::unreachable()` are promises, not checks. If the promise is false the
program has undefined behaviour and the compiler is entitled to produce anything at all — and
because it has deleted the check, you will not get a crash at the point of the lie, you will
get corruption somewhere later. Guard every one of them with an `assert` in debug builds:

```cpp
#include <cassert>
#define ASSUME(x) do { assert(x); [[assume(x)]]; } while (0)
```
:::

:::exercise
Take the `sum_large` loop from the sorted-array experiment and write three versions: the
plain branch, a `cmov`-friendly ternary, and the mask version from this lesson. Run all three
against sorted and shuffled data, and collect `perf stat -e branches,branch-misses,cycles` for
each of the six combinations. You should find that the branchless versions are essentially
insensitive to sorting, that they lose to the branch on sorted data, and that they win by a
wide margin on shuffled data. Then check the assembly to confirm the ternary really did become
a `cmov` and not a jump — it often does not.
:::

## Takeaways

- A predicted branch is free; a mispredict costs a full pipeline flush, typically 15 to 20
  cycles on a modern x86 server core. The distinction is between predictable and
  unpredictable, not between few branches and many.
- The predictor is history-based and finite. Stable branches cost nothing however many you
  have; random data-dependent branches cost you regardless of how few.
- `[[likely]]` and `[[unlikely]]` bias **block layout**, moving cold code out of line to keep
  the hot path contiguous in the instruction cache. They do not signal the hardware predictor.
- Branchless code trades a control dependency for a data dependency and always evaluates both
  sides. It wins on unpredictable branches with cheap operands and loses on predictable ones.
- Hoist invariant conditions out of loops, keep `switch` enumerators dense if you want a jump
  table, and use `[[assume]]`/`std::unreachable()` to delete checks — guarded by an `assert`.
