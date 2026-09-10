---
title: Fixed-Point Maths, Risk and Testing
part: Part VI - Trading Systems
summary: Why a price is never a double, how to do money arithmetic in integers without overflowing, the pre-trade checks that cost a few nanoseconds, and the tests that let you sleep.
time: 35 min
level: advanced
tags: fixed-point, strong-types, overflow, pre-trade-risk, testing
---

Speed is the second requirement. The first is being right, and in this domain being right has
a very specific meaning: your arithmetic agrees with the exchange's to the last unit, your
orders are within limits you can defend, and when something goes wrong you stop rather than
continue. A fast system that sends a wrong order is not a fast system, it is an expensive
one.

## Why prices are never doubles

`double` is a binary floating-point type. It represents a number as a sign, a 53-bit
significand and an exponent, all base two. Decimal fractions like 0.1 have no exact binary
representation, in the same way that one third has no exact decimal one.

```cpp What actually happens
#include <cstdio>

int main() {
    double p = 0.0;
    for (int i = 0; i < 10; ++i) p += 0.1;
    std::printf("%.17g  == 1.0 ? %d\n", p, p == 1.0);   // 0.99999999999999989  == 1.0 ? 0
    std::printf("%.17g\n", 0.1 + 0.2);                  // 0.30000000000000004
    std::printf("%.17g\n", (100.55 + 100.56) / 2.0);    // 100.55500000000001
}
```

Four consequences, each of which has cost somebody money.

- **Representation error.** The price you store is not the price the exchange sent. It is
  within an ulp, which is fine right up until you compare it.
- **Comparison.** `px == limit` is unreliable, so people write `std::fabs(a - b) < 1e-9`, and
  now the correctness of your order book depends on a magic constant that is wrong for
  penny stocks and wrong again for index futures.
- **Accumulated drift.** Sum a day of fills and your P&L is off in the last digits. Reconcile
  against the clearing house and you get a break you cannot explain.
- **Non-associativity.** `(a + b) + c != a + (b + c)` in floating point, so the compiler is
  forbidden from reassociating or vectorising your accumulation loop unless you pass
  `-ffast-math`, which makes results depend on optimisation level. Integers do not have this
  problem, so integer accumulation vectorises freely.

:::key
Every exchange in the world quotes in an integer number of ticks internally. Storing that as
a `double` is a lossy conversion applied to data that arrived exact. Keep the integer.
:::

## Ticks, scale, and the type that remembers it

Two representations, and you will use both. **Integer ticks** counts minimum price
increments: a price of 100.55 with a one-cent tick is 10055, and the tick size lives in the
instrument's static data, not in the price. **Scaled decimals** fix a decimal exponent — say
six digits — so 100.55 is 100'550'000 micro-units. Ticks are best for prices, because the
ladder in lesson 37 indexes on them directly. Scaled decimals are best for money, because
notional values must add across instruments with different tick sizes.

Whichever you choose, the scale must be part of the type's name or documentation, and it must
never be implicit. A raw `std::int64_t` called `price` tells the next reader nothing about
whether it is ticks, cents or micros, and the conversion bug that follows will be found in
production.

```cpp money.hpp
#pragma once
#include <compare>
#include <cstdint>

// Price is a count of exchange ticks. The tick size lives in the instrument record.
class Price {
public:
    constexpr Price() = default;
    constexpr explicit Price(std::int64_t ticks) noexcept : t_(ticks) {}
    [[nodiscard]] constexpr std::int64_t ticks() const noexcept { return t_; }
    constexpr auto operator<=>(const Price&) const = default;

    friend constexpr Price        operator+(Price p, std::int64_t d) noexcept { return Price{p.t_ + d}; }
    friend constexpr std::int64_t operator-(Price a, Price b)        noexcept { return a.t_ - b.t_; }
private:
    std::int64_t t_{0};
};

class Qty {
public:
    constexpr Qty() = default;
    constexpr explicit Qty(std::uint32_t q) noexcept : q_(q) {}
    [[nodiscard]] constexpr std::uint32_t count() const noexcept { return q_; }
    constexpr auto operator<=>(const Qty&) const = default;
private:
    std::uint32_t q_{0};
};

// Notional is currency in micro-units: 1'000'000 == one unit of account.
class Notional {
public:
    static constexpr std::int64_t kPerUnit = 1'000'000;
    constexpr Notional() = default;
    constexpr explicit Notional(std::int64_t micros) noexcept : m_(micros) {}
    [[nodiscard]] constexpr std::int64_t micros() const noexcept { return m_; }
    constexpr auto operator<=>(const Notional&) const = default;
    constexpr Notional& operator+=(Notional o) noexcept { m_ += o.m_; return *this; }
private:
    std::int64_t m_{0};
};
```

Note what these types refuse to do. `Price` has no `operator*`, because a price times a price
is meaningless. `Price` minus `Price` yields a plain `std::int64_t`, a tick *distance*, which
is a different thing from a price. `Qty` cannot be assigned from a `Price` because both
constructors are `explicit`. Every one of those restrictions turns a class of runtime bug into
a compile error, and the whole apparatus costs nothing: each type is one integer, passed in
one register, and every operation inlines to the same instruction the raw integer would have
produced. This is the strong-type argument from lesson 05 applied where it pays best.

## Multiplication, overflow and rounding

Addition of two `std::int64_t` money values is safe in practice. Multiplication is not:
a price of 10'055 ticks times a tick value of 10'000 micros times a quantity of 1'000'000
already needs 67 bits. Signed overflow is undefined behaviour, which means the compiler is
entitled to assume it cannot happen and optimise on that basis, so you do not merely get a
wrong number — you can get a wrong branch.

Do the multiply in 128 bits and check on the way back down.

```cpp Widening multiply with an explicit range check
[[nodiscard]] inline bool notional_of(Price p, Qty q, std::int64_t tick_value_micros,
                                      Notional& out) noexcept {
    const __int128 v = static_cast<__int128>(p.ticks())
                     * static_cast<__int128>(tick_value_micros)
                     * static_cast<__int128>(q.count());
    constexpr __int128 hi = static_cast<__int128>(INT64_MAX);
    constexpr __int128 lo = static_cast<__int128>(INT64_MIN);
    if (v > hi || v < lo) [[unlikely]] return false;
    out = Notional{static_cast<std::int64_t>(v)};
    return true;
}

// For accumulation, the compiler intrinsics are cheaper: one add and one jo.
[[nodiscard]] inline bool add_checked(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    return !__builtin_add_overflow(a, b, &out);   // GCC and Clang; std::add_sat for saturation
}
```

`__int128` is a compiler extension available in GCC and Clang on 64-bit targets. On x86-64 a
64×64→128 multiply is a single `mul` instruction producing a register pair, so the widening
costs essentially nothing. `__builtin_add_overflow` compiles to the add you were going to do
plus a conditional jump on the overflow flag, which the predictor will get right every time.

Rounding is where correctness meets the rulebook. When you convert a computed price to a
tradeable one, or split a notional across fills, the direction you round must match what the
exchange and the clearing house do, or your reconciliation breaks by one unit at a time until
somebody notices. **Half-even** — ties go to the nearest even multiple — is the common
convention for financial rounding because it does not bias a long series of roundings upward
the way half-up does.

```cpp Round to a multiple of `step`, ties to even
[[nodiscard]] constexpr std::int64_t round_half_even(std::int64_t num, std::int64_t step) noexcept {
    const std::int64_t q  = num / step;
    const std::int64_t r  = num % step;
    const std::int64_t ar = r < 0 ? -r : r;
    if (ar * 2 > step || (ar * 2 == step && (q % 2 != 0)))
        return r < 0 ? q - 1 : q + 1;
    return q;
}

static_assert(round_half_even( 25, 10) ==  2);   // 2.5 -> 2, not 3
static_assert(round_half_even( 35, 10) ==  4);   // 3.5 -> 4
static_assert(round_half_even(-25, 10) == -2);
static_assert(round_half_even( 26, 10) ==  3);
```

Write down the convention next to the type, and test it against a set of cases pulled from the
exchange's own specification. Do not infer it.

The best division is the one you do not do: integer division on x86-64 has a latency of roughly
20-40 cycles and is not pipelined, so it is one of the few individual instructions large
enough to see in a 200 ns budget. Keeping prices in ticks removes most divisions outright. For
the ones that remain with a divisor known at startup, precompute a reciprocal.

```cpp Divide by multiplying, with the exactness bound made explicit
struct Reciprocal {
    std::uint64_t m;        // ceil(2^32 / d)
    std::uint64_t max_x;    // divide() is exact for every x <= max_x

    static constexpr Reciprocal of(std::uint32_t d) noexcept {
        const std::uint64_t two32 = std::uint64_t{1} << 32;
        const std::uint64_t m = two32 / d + 1;
        const std::uint64_t e = m * d - two32;          // 1 <= e <= d
        return Reciprocal{m, two32 / e - 1};
    }
    [[nodiscard]] constexpr std::uint64_t divide(std::uint32_t x) const noexcept {
        return static_cast<std::uint64_t>((static_cast<unsigned __int128>(x) * m) >> 32);
    }
};
```

:::pitfall
A magic-number reciprocal is *not* exact over the whole 32-bit range. The `max_x` field above
is the bound within which `divide(x)` provably equals `x / d`; past it the result can be one
too high. Assert your inputs stay inside the bound, or use the compiler's own strength
reduction by making the divisor a compile-time constant, which is exact by construction.
:::

## Pre-trade risk, in a handful of comparisons

Risk on the hot path is not a risk system. It is a small, fixed, branch-predictable set of
comparisons that sits between the strategy's decision and the encoder, and it must be
impossible to bypass. The real risk system runs elsewhere, reconciles against the drop copy,
and pushes limits into this object between messages.

```cpp risk.hpp
#pragma once
#include <cstdint>

enum class Side : std::uint8_t { Buy, Sell };

enum class Verdict : std::uint8_t {
    Ok, Killed, OrderSize, PriceBand, OrderNotional, GrossNotional,
    Position, SelfTrade, MessageRate
};

struct RiskLimits {
    std::int32_t  max_position;         // absolute, in lots, including working orders
    std::uint32_t max_order_qty;
    std::int64_t  max_order_notional;   // micro-units
    std::int64_t  max_gross_notional;
    std::int64_t  band_ticks;           // fat-finger band around a reference price
    std::uint32_t max_msgs_per_sec;
};

class RateLimiter {                     // integer token bucket
public:
    constexpr RateLimiter(std::uint32_t per_sec, std::uint64_t now_ns) noexcept
        : capacity_(per_sec), tokens_(per_sec),
          refill_ns_(1'000'000'000ull / per_sec), last_ns_(now_ns) {}

    [[nodiscard]] bool allow(std::uint64_t now_ns) noexcept {
        const std::uint64_t elapsed = now_ns - last_ns_;
        if (elapsed >= refill_ns_) {
            const std::uint64_t add = elapsed / refill_ns_;   // divisor fixed at startup
            const std::uint64_t t   = tokens_ + add;
            tokens_   = static_cast<std::uint32_t>(t > capacity_ ? capacity_ : t);
            last_ns_ += add * refill_ns_;
        }
        if (tokens_ == 0) [[unlikely]] return false;
        --tokens_;
        return true;
    }
private:
    std::uint32_t capacity_, tokens_;
    std::uint64_t refill_ns_, last_ns_;
};

class RiskGate {
public:
    RiskGate(const RiskLimits& lim, std::int64_t tick_value_micros, std::uint64_t now_ns) noexcept
        : lim_(lim), tick_value_micros_(tick_value_micros), rate_(lim.max_msgs_per_sec, now_ns) {}

    [[nodiscard]] Verdict check(Side s, Price px, Qty q, Price ref, std::uint64_t now_ns) noexcept {
        if (killed_) [[unlikely]]                          return Verdict::Killed;
        if (q.count() > lim_.max_order_qty)                return Verdict::OrderSize;

        const std::int64_t d = px - ref;
        if ((d < 0 ? -d : d) > lim_.band_ticks)            return Verdict::PriceBand;

        Notional n{};
        if (!notional_of(px, q, tick_value_micros_, n))    return Verdict::OrderNotional;
        if (n.micros() > lim_.max_order_notional)          return Verdict::OrderNotional;

        std::int64_t gross = 0;
        if (!add_checked(gross_micros_, n.micros(), gross)) return Verdict::GrossNotional;
        if (gross > lim_.max_gross_notional)               return Verdict::GrossNotional;

        const std::int32_t sq   = static_cast<std::int32_t>(q.count());
        const std::int32_t proj = exposure_ + (s == Side::Buy ? sq : -sq);
        if (proj > lim_.max_position || proj < -lim_.max_position) return Verdict::Position;

        if (s == Side::Buy  && have_ask_ && px >= our_ask_) return Verdict::SelfTrade;
        if (s == Side::Sell && have_bid_ && px <= our_bid_) return Verdict::SelfTrade;

        if (!rate_.allow(now_ns)) [[unlikely]]             return Verdict::MessageRate;
        return Verdict::Ok;
    }

    void kill() noexcept { killed_ = true; }        // one-way; only a restart clears it
    void set_exposure(std::int32_t lots) noexcept { exposure_ = lots; }
    void set_resting(bool hb, Price bid, bool ha, Price ask) noexcept {
        have_bid_ = hb; our_bid_ = bid; have_ask_ = ha; our_ask_ = ask;
    }
private:
    RiskLimits   lim_;
    std::int64_t tick_value_micros_;
    RateLimiter  rate_;
    std::int64_t gross_micros_{0};
    std::int32_t exposure_{0};
    Price        our_bid_{}, our_ask_{};
    bool         have_bid_{false}, have_ask_{false}, killed_{false};
};
```

That is eight comparisons, one widening multiply and one checked add. The whole object is
around 112 bytes on a typical 64-bit build, so it lives in two cache lines that stay hot, and
every branch is overwhelmingly not-taken in normal operation, which is exactly the shape the
predictor handles for free. Expect single-digit nanoseconds. Nobody has ever regretted paying
that.

:::hft
The **kill switch** is the most important line in the file and the one most likely to be
implemented badly. It must be settable from outside the process without the hot thread
cooperating — a shared memory flag polled once per loop, not a signal handler, not a
condition variable. It must be one-way, so that a panicking human cannot un-kill by accident.
And "killed" must mean *cancel everything and send nothing*, not *stop sending new orders*,
because a system that stops managing its resting orders is more dangerous than one that keeps
trading.
:::

## Testing what you cannot reason about

Five layers, in increasing order of how much sleep they buy you.

1. **Unit tests for the book.** Every operation from lesson 37, plus the edge cases that only
   appear in real feeds: a cancel for an unknown id, an execute larger than the resting
   quantity, an add at a price outside the ladder window, a level emptied and refilled in the
   same packet.
2. **Deterministic replay against golden output.** Capture a real morning of feed to disk, run
   it through the whole system with sending suppressed, and hash the sequence of decisions.
   The hash goes in the repository. Any change that alters it must be explained in the commit
   message. This single test catches more regressions than everything else combined.
3. **Fuzz the decoder.** The decoder from lesson 36 is the only code that reads bytes you did
   not write, and it is the only place a malformed packet can reach memory. `libFuzzer` or
   AFL++ against the message dispatch entry point, with ASan and UBSan on, run for an hour in
   CI and overnight in a longer job.
4. **Property-based tests for invariants.** Generate random but well-formed message streams
   and assert the properties that must hold whatever happened: the best bid is strictly below
   the best ask or the book is one-sided; every level's aggregate quantity equals the sum of
   its orders; every id in the map resolves to an order whose slot points back at a list
   containing it; the free list plus the live orders always total `kMaxOrders`.
5. **Latency regression tests.** Replay a fixed capture, emit the histogram, fail the build if
   p99 or p99.9 moves beyond a threshold against the committed baseline.

```sh The two build configurations, and where each one runs
# CI correctness: every check on, speed irrelevant
$ g++ -std=c++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -D_GLIBCXX_ASSERTIONS replay.cpp -o replay-debug

# Production: no sanitizers, no assertions, and this is the binary you benchmark
$ g++ -std=c++23 -O2 -march=icelake-server -flto -DNDEBUG -g \
      trader.cpp -o trader
```

:::warn
Sanitizers belong in CI and never in production. AddressSanitizer typically costs 2-3x in
run time and around 3x in memory; ThreadSanitizer is worse. More importantly, a sanitized
binary has different code layout, different allocation behaviour and different timing, so any
latency number measured under one is meaningless. Run them on every commit, ship neither.
:::

:::exercise
Write the property-based test for book invariants. Generate a stream of a hundred thousand
random add, cancel, modify and execute messages against a small ladder — deliberately small,
so collisions and empty levels are frequent — and after every message assert all four
invariants listed above. Then seed the generator from the clock, log the seed, and run it in
CI. When it fails at 3am you will have a seed that reproduces it exactly, which is the entire
point of writing it this way rather than with hand-picked cases.
:::

## Takeaways

- Prices are integers. Binary floating point cannot represent decimal prices exactly, breaks
  equality, drifts under accumulation, and blocks reassociation.
- Pick ticks for prices and a scaled integer for money, and put the scale in the type so the
  next reader cannot get it wrong.
- Multiply through `__int128` and add with `__builtin_add_overflow`. Both are nearly free and
  signed overflow is undefined behaviour, not merely a wrong number.
- Match the exchange's rounding convention exactly, and avoid division entirely by staying in
  ticks or precomputing a reciprocal with a proven exactness bound.
- Pre-trade risk is eight predictable comparisons and a one-way kill switch, costing single-
  digit nanoseconds. It is never optional and never bypassable.
- Golden-output replay of a recorded feed is the highest-value test you will write. Sanitizers
  and fuzzing in CI, latency regression on dedicated hardware, neither in production.
