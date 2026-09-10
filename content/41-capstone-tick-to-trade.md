---
title: Capstone: A Tick-to-Trade Skeleton
part: Part VI - Trading Systems
summary: Seven files, one pinned thread, and a measured path from packet to order. Everything in this series, assembled into something you can compile and run today.
time: 60 min
level: expert
tags: capstone, tick-to-trade, spsc, histogram, benchmarking
---

Forty lessons of techniques are worth less than one system you can build, run, measure and
break. This is that system: a complete tick-to-trade skeleton in about six hundred lines,
with a synthetic feed so it runs on your laptop with no exchange connection. It is not a
production trading system, and the last section is an honest list of what it is missing. But
the shape is right, the discipline is right, and every optimisation you were shown earlier is
either in it or is one of the exercises at the end.

## The shape of the system

One thread does everything on the critical path. It is pinned to an isolated core, it never
allocates after startup, it never makes a syscall in steady state, and it never blocks.

```text
                    +--------------------------------------------------+
  packet  --------> | decode -> book -> strategy -> risk -> encode -> send
                    +--------------------------------------------------+
                                    |  24-byte records
                                    v
                              SPSC ring (lesson 33)
                                    |
                    +---------------v----------------+
                    | cold thread: format, write, io |
                    +--------------------------------+
```

Seven files. `types.hpp` holds the strong types and the wire layouts. `spsc.hpp` is the
queue from lesson 33. `book.hpp` is a shrunken version of the price ladder from lesson 37.
`decoder.hpp` is the message decoder from lesson 36. `strategy.hpp` and `risk.hpp` are
deliberately small. `main.cpp` wires them together, generates the feed, and measures.

## Foundations: types, queue, book

Prices are ticks and quantities are counts, both wrapped so a unit mix-up will not compile.
The wire structs are packed and their sizes are asserted, because a struct that silently
gains four bytes of padding is the failure mode lesson 02 warned about.

```cpp types.hpp
#pragma once
#include <compare>
#include <cstdint>

namespace tt {

using Handle = std::uint32_t;
inline constexpr Handle kNull = 0xFFFF'FFFFu;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

class Price {                                   // exchange ticks; never a double
public:
    constexpr Price() = default;
    constexpr explicit Price(std::int64_t t) noexcept : t_(t) {}
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

// ---- wire format: fixed layout, no padding ------------------------------------
#pragma pack(push, 1)
struct MsgHeader { std::uint16_t length; std::uint8_t type; std::uint32_t seq; };
struct AddMsg     { MsgHeader h; std::uint64_t id;     std::int32_t price; std::uint32_t qty; std::uint8_t side; };
struct CancelMsg  { MsgHeader h; std::uint64_t id; };
struct ExecMsg    { MsgHeader h; std::uint64_t id;     std::uint32_t qty; };
struct ReplaceMsg { MsgHeader h; std::uint64_t old_id; std::uint64_t new_id;
                    std::int32_t price; std::uint32_t qty; };
struct NewOrderMsg { std::uint8_t type; std::uint64_t client_id;
                     std::int32_t price; std::uint32_t qty; std::uint8_t side; };
#pragma pack(pop)

static_assert(sizeof(MsgHeader)   ==  7);
static_assert(sizeof(AddMsg)      == 24);
static_assert(sizeof(CancelMsg)   == 15);
static_assert(sizeof(ExecMsg)     == 19);
static_assert(sizeof(ReplaceMsg)  == 31);
static_assert(sizeof(NewOrderMsg) == 18);

enum : std::uint8_t { kAdd = 'A', kCancel = 'X', kExec = 'E', kReplace = 'U' };

// ---- published views ----------------------------------------------------------
struct alignas(64) TopOfBook {
    std::int64_t  bid_px;
    std::int64_t  ask_px;
    std::uint64_t bid_qty;
    std::uint64_t ask_qty;
    std::uint64_t seq;
};
static_assert(sizeof(TopOfBook) == 64);

struct Signal { bool act; Side side; Price px; Qty qty; };

struct LogRecord {                              // 24 bytes, no strings, no formatting
    std::uint64_t tsc;
    std::uint32_t site;
    std::uint32_t u32;
    std::int64_t  i64;
};
static_assert(sizeof(LogRecord) == 24);

enum : std::uint32_t { kSiteQuote = 0, kSiteReject = 1, kSiteGap = 2, kSiteCount = 3 };

}  // namespace tt
```

```cpp spsc.hpp
#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace tt {

inline constexpr std::size_t kCacheLine = 64;

// Wait-free single-producer/single-consumer queue. Lesson 33.
template <class T, std::size_t N>
class Spsc {
    static_assert(N != 0 && (N & (N - 1)) == 0, "capacity must be a power of two");
public:
    [[nodiscard]] bool try_push(const T& v) noexcept {
        const std::size_t w    = write_.load(std::memory_order_relaxed);
        const std::size_t next = w + 1;
        if (next - read_cache_ > N) {                       // maybe full: refresh once
            read_cache_ = read_.load(std::memory_order_acquire);
            if (next - read_cache_ > N) return false;
        }
        buf_[w & (N - 1)] = v;
        write_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_cache_) {                            // maybe empty: refresh once
            write_cache_ = write_.load(std::memory_order_acquire);
            if (r == write_cache_) return false;
        }
        out = buf_[r & (N - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    alignas(kCacheLine) std::atomic<std::size_t> write_{0};
    std::size_t read_cache_{0};                             // producer-private
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};
    std::size_t write_cache_{0};                            // consumer-private
    alignas(kCacheLine) std::array<T, N> buf_{};
};

}  // namespace tt
```

The book is lesson 37 with the constants turned down so the whole object fits in about a
megabyte: a 1024-slot ladder per side, sixteen thousand order slots, and a 32k-entry
open-addressing id map. Everything else is identical.

```cpp book.hpp
#pragma once
#include "types.hpp"
#include <array>
#include <cstddef>

namespace tt {

inline constexpr std::int32_t kLevels    = 1024;     // ladder slots per side
inline constexpr std::size_t  kMaxOrders = 1u << 14; // live orders

struct Level { std::uint64_t qty; Handle head; Handle tail; };
struct Order { std::uint64_t id; std::uint32_t qty; Handle prev, next;
               std::int32_t slot; Side side; };

class IdMap {                                        // open addressing, linear probing
public:
    static constexpr std::size_t kSlots = 1u << 15;
    static constexpr std::size_t kMask  = kSlots - 1;

    void clear() noexcept { vals_.fill(kNull); }

    static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 33; x *= 0xff51'afd7'ed55'8ccdULL;
        x ^= x >> 33; x *= 0xc4ce'b9fe'1a85'ec53ULL;
        return x ^ (x >> 33);
    }
    void insert(std::uint64_t k, Handle v) noexcept {
        std::size_t i = mix(k) & kMask;
        while (vals_[i] != kNull && keys_[i] != k) i = (i + 1) & kMask;
        keys_[i] = k; vals_[i] = v;
    }
    [[nodiscard]] Handle find(std::uint64_t k) const noexcept {
        std::size_t i = mix(k) & kMask;
        while (vals_[i] != kNull) {
            if (keys_[i] == k) return vals_[i];
            i = (i + 1) & kMask;
        }
        return kNull;
    }
    void erase(std::uint64_t k) noexcept {
        std::size_t i = mix(k) & kMask;
        while (vals_[i] != kNull && keys_[i] != k) i = (i + 1) & kMask;
        if (vals_[i] == kNull) return;
        std::size_t j = i;
        for (;;) {                                   // backward shift, no tombstones
            vals_[i] = kNull;
            for (;;) {
                j = (j + 1) & kMask;
                if (vals_[j] == kNull) return;
                if (!inside(mix(keys_[j]) & kMask, i, j)) break;
            }
            keys_[i] = keys_[j]; vals_[i] = vals_[j]; i = j;
        }
    }
private:
    static constexpr bool inside(std::size_t h, std::size_t i, std::size_t j) noexcept {
        return (i <= j) ? (i < h && h <= j) : (i < h || h <= j);
    }
    std::array<std::uint64_t, kSlots> keys_{};
    std::array<Handle, kSlots>        vals_{};
};

class Book {                                         // heap-allocate: ~1 MB
public:
    explicit Book(std::int64_t base) noexcept { reset(base); }

    void reset(std::int64_t base) noexcept {
        base_ = base; best_bid_ = -1; best_ask_ = kLevels;
        bid_.fill(Level{0, kNull, kNull});
        ask_.fill(Level{0, kNull, kNull});
        for (Handle i = 0; i + 1 < kMaxOrders; ++i) slab_[i].next = i + 1;
        slab_[kMaxOrders - 1].next = kNull;
        free_head_ = 0;
        ids_.clear();
        drops_ = 0;
    }

    void add(std::uint64_t id, Side s, std::int64_t px, std::uint32_t q) noexcept {
        const std::int32_t slot = static_cast<std::int32_t>(px - base_);
        if (slot < 0 || slot >= kLevels || free_head_ == kNull) [[unlikely]] { ++drops_; return; }
        const Handle h = free_head_;
        Order& o = slab_[h];
        free_head_ = o.next;
        o.id = id; o.qty = q; o.slot = slot; o.side = s; o.next = kNull;

        Level& lv = level(s, slot);
        o.prev = lv.tail;
        if (lv.tail != kNull) slab_[lv.tail].next = h; else lv.head = h;
        lv.tail = h; lv.qty += q;

        if (s == Side::Buy) { if (slot > best_bid_) best_bid_ = slot; }
        else                { if (slot < best_ask_) best_ask_ = slot; }
        ids_.insert(id, h);
    }

    void cancel(std::uint64_t id) noexcept {
        const Handle h = ids_.find(id);
        if (h == kNull) [[unlikely]] return;
        unlink(h); ids_.erase(id);
    }

    void execute(std::uint64_t id, std::uint32_t filled) noexcept {
        const Handle h = ids_.find(id);
        if (h == kNull) [[unlikely]] return;
        Order& o = slab_[h];
        const std::uint32_t f = filled < o.qty ? filled : o.qty;
        level(o.side, o.slot).qty -= f;
        o.qty -= f;
        if (o.qty == 0) { unlink(h); ids_.erase(id); }
    }

    void replace(std::uint64_t old_id, std::uint64_t new_id,
                 std::int64_t px, std::uint32_t q) noexcept {
        const Handle h = ids_.find(old_id);
        if (h == kNull) [[unlikely]] return;
        const Side s = slab_[h].side;
        unlink(h); ids_.erase(old_id);
        add(new_id, s, px, q);
    }

    [[nodiscard]] TopOfBook top(std::uint64_t seq) const noexcept {
        const bool hb = best_bid_ >= 0, ha = best_ask_ < kLevels;
        return TopOfBook{
            hb ? base_ + best_bid_ : 0, ha ? base_ + best_ask_ : 0,
            hb ? bid_[static_cast<std::size_t>(best_bid_)].qty : 0,
            ha ? ask_[static_cast<std::size_t>(best_ask_)].qty : 0, seq};
    }
    [[nodiscard]] std::uint64_t drops() const noexcept { return drops_; }

private:
    Level& level(Side s, std::int32_t slot) noexcept {
        return (s == Side::Buy ? bid_ : ask_)[static_cast<std::size_t>(slot)];
    }
    void unlink(Handle h) noexcept {
        Order& o = slab_[h];
        Level& lv = level(o.side, o.slot);
        if (o.prev != kNull) slab_[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != kNull) slab_[o.next].prev = o.prev; else lv.tail = o.prev;
        lv.qty -= o.qty;
        if (lv.head == kNull) repair(o.side, o.slot);
        o.next = free_head_; free_head_ = h;
    }
    void repair(Side s, std::int32_t slot) noexcept {
        if (s == Side::Buy) {
            if (slot != best_bid_) return;
            while (best_bid_ >= 0 && bid_[static_cast<std::size_t>(best_bid_)].head == kNull) --best_bid_;
        } else {
            if (slot != best_ask_) return;
            while (best_ask_ < kLevels && ask_[static_cast<std::size_t>(best_ask_)].head == kNull) ++best_ask_;
        }
    }

    std::int64_t  base_{0};
    std::int32_t  best_bid_{-1}, best_ask_{kLevels};
    Handle        free_head_{kNull};
    std::uint64_t drops_{0};
    std::array<Level, kLevels>    bid_{}, ask_{};
    std::array<Order, kMaxOrders> slab_{};
    IdMap                         ids_;
};

}  // namespace tt
```

## Decoder, strategy, risk

The decoder takes the handler as a template parameter rather than through a virtual
interface, so at `-O2` the callbacks inline straight into the dispatch loop and there is no
indirect call anywhere on the path. It copies each message out with `memcpy` rather than
casting a pointer, which is both correct under the aliasing rules and free — the compiler
turns a fixed-size `memcpy` into the same loads a cast would have produced.

```cpp decoder.hpp
#pragma once
#include "types.hpp"
#include <cstddef>
#include <cstring>

namespace tt {

// Decodes a packet of concatenated length-prefixed messages. Lesson 36.
// Handler must provide on_add / on_cancel / on_exec / on_replace / on_gap.
template <class Handler>
class Decoder {
public:
    explicit Decoder(Handler& h) noexcept : h_(h) {}

    void on_packet(const std::byte* p, std::size_t n) noexcept {
        std::size_t off = 0;
        while (off + sizeof(MsgHeader) <= n) {
            MsgHeader hd;
            std::memcpy(&hd, p + off, sizeof(hd));          // no aliasing, no unaligned load
            if (hd.length < sizeof(MsgHeader) || off + hd.length > n) [[unlikely]] return;

            if (hd.seq != expected_) [[unlikely]] {
                if (expected_ != 0) h_.on_gap(expected_, hd.seq);
                expected_ = hd.seq;
            }
            ++expected_;

            switch (hd.type) {
            case kAdd: {
                AddMsg m; std::memcpy(&m, p + off, sizeof(m));
                h_.on_add(m.id, m.side == 0 ? Side::Buy : Side::Sell, m.price, m.qty);
                break;
            }
            case kCancel: {
                CancelMsg m; std::memcpy(&m, p + off, sizeof(m));
                h_.on_cancel(m.id);
                break;
            }
            case kExec: {
                ExecMsg m; std::memcpy(&m, p + off, sizeof(m));
                h_.on_exec(m.id, m.qty);
                break;
            }
            case kReplace: {
                ReplaceMsg m; std::memcpy(&m, p + off, sizeof(m));
                h_.on_replace(m.old_id, m.new_id, m.price, m.qty);
                break;
            }
            default: break;                                  // unknown type: skip by length
            }
            off += hd.length;
        }
    }

    [[nodiscard]] std::uint32_t expected() const noexcept { return expected_; }

private:
    Handler&      h_;
    std::uint32_t expected_{0};
};

}  // namespace tt
```

```cpp strategy.hpp
#pragma once
#include "types.hpp"

namespace tt {

struct StrategyConfig {
    std::int64_t  min_spread_ticks;   // quote only when the market is at least this wide
    std::uint32_t quote_qty;
    std::int32_t  max_position;       // lots, absolute
    std::uint64_t min_top_qty;        // ignore a top of book thinner than this
};

// Quote inside a wide spread, subject to a position limit. Deliberately trivial:
// the point is the shape of the call, not the alpha.
class Strategy {
public:
    constexpr explicit Strategy(const StrategyConfig& c) noexcept : c_(c) {}

    [[nodiscard]] Signal on_top(const TopOfBook& t) noexcept {
        if (t.bid_qty < c_.min_top_qty || t.ask_qty < c_.min_top_qty) [[unlikely]]
            return Signal{false, Side::Buy, Price{}, Qty{}};

        const std::int64_t spread = t.ask_px - t.bid_px;
        if (spread < c_.min_spread_ticks) return Signal{false, Side::Buy, Price{}, Qty{}};

        // Lean against the position: buy while short of the limit, sell while long.
        if (position_ < c_.max_position && t.bid_qty <= t.ask_qty)
            return Signal{true, Side::Buy, Price{t.bid_px + 1}, Qty{c_.quote_qty}};
        if (position_ > -c_.max_position)
            return Signal{true, Side::Sell, Price{t.ask_px - 1}, Qty{c_.quote_qty}};

        return Signal{false, Side::Buy, Price{}, Qty{}};
    }

    void on_fill(Side s, std::uint32_t q) noexcept {
        position_ += (s == Side::Buy) ? static_cast<std::int32_t>(q)
                                      : -static_cast<std::int32_t>(q);
    }
    [[nodiscard]] std::int32_t position() const noexcept { return position_; }
    void reset() noexcept { position_ = 0; }

private:
    StrategyConfig c_;
    std::int32_t   position_{0};
};

}  // namespace tt
```

```cpp risk.hpp
#pragma once
#include "types.hpp"
#include <atomic>

namespace tt {

enum class Verdict : std::uint8_t {
    Ok, Killed, OrderSize, PriceBand, OrderNotional, Position, SelfTrade, MessageRate
};

struct RiskLimits {
    std::uint32_t max_order_qty;
    std::int64_t  max_order_notional;   // micro-units
    std::int32_t  max_position;
    std::int64_t  band_ticks;
    std::uint32_t max_msgs_per_sec;
};

[[nodiscard]] inline bool notional_of(Price p, Qty q, std::int64_t tick_value_micros,
                                      std::int64_t& out) noexcept {
    const __int128 v = static_cast<__int128>(p.ticks())
                     * static_cast<__int128>(tick_value_micros)
                     * static_cast<__int128>(q.count());
    if (v > static_cast<__int128>(INT64_MAX) || v < static_cast<__int128>(INT64_MIN))
        [[unlikely]] return false;
    out = static_cast<std::int64_t>(v);
    return true;
}

class RiskGate {
public:
    // `clock_hz` is the frequency of whatever counter `now` is measured in, so the
    // rate limiter can work directly in TSC ticks with no conversion on the hot path.
    RiskGate(const RiskLimits& l, std::int64_t tick_value_micros,
             std::uint64_t clock_hz, std::uint64_t now) noexcept
        : lim_(l), tick_value_micros_(tick_value_micros),
          tokens_(l.max_msgs_per_sec), refill_(clock_hz / l.max_msgs_per_sec), last_(now) {}

    [[nodiscard]] Verdict check(const Signal& s, Price ref, std::int32_t position,
                                std::uint64_t now) noexcept {
        if (kill_.load(std::memory_order_relaxed)) [[unlikely]] return Verdict::Killed;
        if (s.qty.count() > lim_.max_order_qty)                 return Verdict::OrderSize;

        const std::int64_t d = s.px - ref;
        if ((d < 0 ? -d : d) > lim_.band_ticks)                 return Verdict::PriceBand;

        std::int64_t n = 0;
        if (!notional_of(s.px, s.qty, tick_value_micros_, n))   return Verdict::OrderNotional;
        if (n > lim_.max_order_notional)                        return Verdict::OrderNotional;

        const std::int32_t sq   = static_cast<std::int32_t>(s.qty.count());
        const std::int32_t proj = position + (s.side == Side::Buy ? sq : -sq);
        if (proj > lim_.max_position || proj < -lim_.max_position) return Verdict::Position;

        if (s.side == Side::Buy  && have_ask_ && s.px >= our_ask_) return Verdict::SelfTrade;
        if (s.side == Side::Sell && have_bid_ && s.px <= our_bid_) return Verdict::SelfTrade;

        if (!allow(now)) [[unlikely]]                           return Verdict::MessageRate;
        return Verdict::Ok;
    }

    void kill() noexcept { kill_.store(true, std::memory_order_relaxed); }   // one-way
    void set_resting(bool hb, Price b, bool ha, Price a) noexcept {
        have_bid_ = hb; our_bid_ = b; have_ask_ = ha; our_ask_ = a;
    }

private:
    [[nodiscard]] bool allow(std::uint64_t now) noexcept {
        const std::uint64_t elapsed = now - last_;
        if (elapsed >= refill_) {
            const std::uint64_t add = elapsed / refill_;
            const std::uint64_t t   = tokens_ + add;
            tokens_ = static_cast<std::uint32_t>(t > lim_.max_msgs_per_sec
                                                  ? lim_.max_msgs_per_sec : t);
            last_ += add * refill_;
        }
        if (tokens_ == 0) [[unlikely]] return false;
        --tokens_;
        return true;
    }

    RiskLimits        lim_;
    std::int64_t      tick_value_micros_;
    std::uint32_t     tokens_;
    std::uint64_t     refill_, last_;
    Price             our_bid_{}, our_ask_{};
    bool              have_bid_{false}, have_ask_{false};
    std::atomic<bool> kill_{false};             // set from any thread, polled here
};

}  // namespace tt
```

## The loop

`main.cpp` contains the clock, the histogram, the synthetic feed, the engine, and the loop.
The engine is both the decoder's handler and the owner of the path, so one packet is one
call. The measurement brackets exactly the work you would do between the NIC handing you a
buffer and the order leaving.

```cpp main.cpp
#include "book.hpp"
#include "decoder.hpp"
#include "risk.hpp"
#include "spsc.hpp"
#include "strategy.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace tt {

// ---------------------------------------------------------------- clock ------
// The cheapest monotonic counter this machine has. On x86-64 that is the
// invariant TSC: ~20 cycles to read and a tick of roughly a third of a
// nanosecond. On aarch64 it is cntvct_el0, whose frequency you must read rather
// than assume -- 24 MHz on Apple silicon, so about 42 ns of resolution.
// The portable fallback resolves only microseconds and cannot see this path.
[[nodiscard]] inline std::uint64_t now_ticks() noexcept {
#if defined(__x86_64__)
    std::uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    std::uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

[[nodiscard]] inline std::uint64_t measure_clock_hz() noexcept {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = now_ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t c1 = now_ticks();
    const auto t1 = clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<std::uint64_t>(static_cast<double>(c1 - c0) / secs);
}

inline void pin_to_core(int core) noexcept {
#if defined(__linux__)
    ::cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
#else
    (void)core;                       // no portable equivalent; see lesson 35
#endif
}

// ------------------------------------------------------------ histogram ------
class Histogram {
public:
    static constexpr std::size_t  kBuckets = 2048;
    static constexpr std::uint64_t kNsPer  = 5;

    void record(std::uint64_t ns) noexcept {
        const std::size_t b = static_cast<std::size_t>(ns / kNsPer);
        ++c_[b < kBuckets ? b : kBuckets - 1];
        ++n_;
        sum_ += ns;
        if (ns > max_) max_ = ns;
    }
    [[nodiscard]] std::uint64_t pct(double p) const noexcept {
        if (n_ == 0) return 0;
        const std::uint64_t want = static_cast<std::uint64_t>(static_cast<double>(n_) * p);
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            acc += c_[i];
            if (acc >= want) return (i + 1) * kNsPer;
        }
        return kBuckets * kNsPer;
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return n_; }
    [[nodiscard]] std::uint64_t max()   const noexcept { return max_; }
    [[nodiscard]] double        mean()  const noexcept {
        return n_ ? static_cast<double>(sum_) / static_cast<double>(n_) : 0.0;
    }

private:
    std::array<std::uint64_t, kBuckets> c_{};
    std::uint64_t n_{0}, sum_{0}, max_{0};
};

// --------------------------------------------------------- synthetic feed ----
// Builds packets of 1-6 messages around a fixed mid. Buys are strictly below the
// mid and sells strictly above, so the book can never cross. Deterministic given
// the seed, which is what makes a replay reproducible.
class FeedGen {
public:
    FeedGen(std::uint64_t seed, std::int64_t base) noexcept
        : s_(seed | 1u), mid_(base + kLevels / 2) {}

    std::size_t next_packet(std::byte* out, std::size_t cap) noexcept {
        std::size_t off = 0;
        const int n = 1 + static_cast<int>(rnd() % 6);
        for (int i = 0; i < n; ++i) {
            if (off + sizeof(ReplaceMsg) > cap) break;
            const std::uint64_t r = rnd() % 100;
            // Keep the live population inside a band so the slab never fills.
            const bool add = (n_ < kLowWater) || (n_ < kHighWater && r < 50);
            if (add)          off += emit_add(out + off);
            else if (r < 70)  off += emit_cancel(out + off);
            else              off += emit_exec(out + off);
        }
        return off;
    }

private:
    std::uint64_t rnd() noexcept {                    // xorshift64*
        s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
        return s_ * 0x2545'F491'4F6C'DD1DULL;
    }

    std::size_t emit_add(std::byte* p) noexcept {
        const bool buy = (rnd() & 1) != 0;
        const std::int64_t depth = static_cast<std::int64_t>(rnd() % 12);
        AddMsg m{};
        m.h.length = sizeof(AddMsg);
        m.h.type   = kAdd;
        m.h.seq    = seq_++;
        m.id       = next_id_++;
        m.side     = buy ? 0 : 1;
        m.price    = static_cast<std::int32_t>(buy ? mid_ - 1 - depth : mid_ + 1 + depth);
        m.qty      = 1 + static_cast<std::uint32_t>(rnd() % 500);
        std::memcpy(p, &m, sizeof(m));
        live_[n_++] = Live{m.id, m.qty};
        return sizeof(m);
    }

    std::size_t emit_cancel(std::byte* p) noexcept {
        const std::size_t i = pick();
        CancelMsg m{};
        m.h.length = sizeof(CancelMsg);
        m.h.type   = kCancel;
        m.h.seq    = seq_++;
        m.id       = live_[i].id;
        std::memcpy(p, &m, sizeof(m));
        live_[i] = live_[--n_];
        return sizeof(m);
    }

    std::size_t emit_exec(std::byte* p) noexcept {
        const std::size_t i = pick();
        const std::uint32_t have = live_[i].qty;
        const std::uint32_t fill = 1 + static_cast<std::uint32_t>(rnd() % have);
        ExecMsg m{};
        m.h.length = sizeof(ExecMsg);
        m.h.type   = kExec;
        m.h.seq    = seq_++;
        m.id       = live_[i].id;
        m.qty      = fill;
        std::memcpy(p, &m, sizeof(m));
        if (fill == have) live_[i] = live_[--n_]; else live_[i].qty = have - fill;
        return sizeof(m);
    }

    [[nodiscard]] std::size_t pick() noexcept { return static_cast<std::size_t>(rnd() % n_); }

    struct Live { std::uint64_t id; std::uint32_t qty; };
    static constexpr std::size_t kCapacity  = 8192;
    static constexpr std::size_t kLowWater  = 2048;
    static constexpr std::size_t kHighWater = 6144;

    std::uint64_t s_;
    std::int64_t  mid_;
    std::uint32_t seq_{0};
    std::uint64_t next_id_{1};
    std::array<Live, kCapacity> live_{};
    std::size_t   n_{0};
};

// -------------------------------------------------------------- engine -------
using LogQueue = Spsc<LogRecord, 1u << 16>;

class Engine {
public:
    Engine(std::int64_t base, std::uint64_t clock_hz, LogQueue& log) noexcept
        : book_(base), dec_(*this), strat_(kStrategy),
          risk_(kLimits, kTickValueMicros, clock_hz, now_ticks()), log_(log) {}

    // ---- decoder callbacks: templates, so these inline into the dispatch loop ----
    void on_add(std::uint64_t id, Side s, std::int32_t px, std::uint32_t q) noexcept {
        book_.add(id, s, px, q);
    }
    void on_cancel(std::uint64_t id) noexcept { book_.cancel(id); }
    void on_exec(std::uint64_t id, std::uint32_t q) noexcept { book_.execute(id, q); }
    void on_replace(std::uint64_t o, std::uint64_t n, std::int32_t px, std::uint32_t q) noexcept {
        book_.replace(o, n, px, q);
    }
    void on_gap(std::uint32_t expected, std::uint32_t got) noexcept {
        ++gaps_;
        push_log(LogRecord{now_ticks(), kSiteGap, expected, static_cast<std::int64_t>(got)});
    }

    // ---- one packet, wire to wire ----
    void on_packet(const std::byte* p, std::size_t n, std::uint64_t t0) noexcept {
        dec_.on_packet(p, n);

        const TopOfBook t = book_.top(++seq_);
        settle_quote(t);
        if (t.bid_px == 0 || t.ask_px == 0) [[unlikely]] return;

        const Signal sig = strat_.on_top(t);
        if (!sig.act) return;

        const Price ref{(t.bid_px + t.ask_px) / 2};
        const Verdict v = risk_.check(sig, ref, strat_.position(), t0);
        if (v != Verdict::Ok) [[unlikely]] {
            ++rejects_[static_cast<std::size_t>(v)];
            push_log(LogRecord{t0, kSiteReject, static_cast<std::uint32_t>(v), sig.px.ticks()});
            return;
        }

        const std::size_t bytes = encode(sig);
        send(bytes);
        quote_ = sig;
        have_quote_ = true;
        risk_.set_resting(sig.side == Side::Buy, sig.px, sig.side == Side::Sell, sig.px);
        push_log(LogRecord{t0, kSiteQuote, sig.qty.count(), sig.px.ticks()});
    }

    void suppress(bool on) noexcept { suppressed_ = on; }
    void reset_state() noexcept {
        strat_.reset();
        have_quote_ = false;
        sent_ = fills_ = gaps_ = 0;
        rejects_.fill(0);
    }

    [[nodiscard]] std::uint64_t sent()      const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t fills()     const noexcept { return fills_; }
    [[nodiscard]] std::uint64_t gaps()      const noexcept { return gaps_; }
    [[nodiscard]] std::uint64_t drops()     const noexcept { return book_.drops(); }
    [[nodiscard]] std::uint64_t log_drops() const noexcept { return log_drops_; }
    [[nodiscard]] std::int32_t  position()  const noexcept { return strat_.position(); }
    [[nodiscard]] const std::array<std::uint64_t, 8>& rejects() const noexcept { return rejects_; }

private:
    // A passive quote is deemed filled once the market trades through its price.
    // Crude and deterministic, but enough to exercise the position limit.
    void settle_quote(const TopOfBook& t) noexcept {
        if (!have_quote_) return;
        const bool hit = (quote_.side == Side::Buy)
                       ? (t.bid_px != 0 && t.bid_px <  quote_.px.ticks())
                       : (t.ask_px != 0 && t.ask_px >  quote_.px.ticks());
        if (!hit) return;
        strat_.on_fill(quote_.side, quote_.qty.count());
        risk_.set_resting(false, Price{}, false, Price{});   // nothing of ours is resting
        have_quote_ = false;
        ++fills_;
    }

    std::size_t encode(const Signal& s) noexcept {
        NewOrderMsg m{};
        m.type      = 'N';
        m.client_id = ++client_id_;
        m.price     = static_cast<std::int32_t>(s.px.ticks());
        m.qty       = s.qty.count();
        m.side      = (s.side == Side::Buy) ? 0 : 1;
        std::memcpy(out_.data(), &m, sizeof(m));
        return sizeof(m);
    }

    void send(std::size_t n) noexcept {
        if (suppressed_) [[unlikely]] return;
        sent_ += n;
        // Stands in for the order-entry socket, and stops the encoder being elided.
        __asm__ __volatile__("" :: "r"(out_.data()) : "memory");
    }

    void push_log(const LogRecord& r) noexcept {
        if (suppressed_) [[unlikely]] return;     // warming must not leak into the log
        if (!log_.try_push(r)) ++log_drops_;      // drop a log line, never delay an order
    }

    static constexpr std::int64_t kTickValueMicros = 10'000;   // 1 tick = 0.01 unit
    static constexpr StrategyConfig kStrategy{
        .min_spread_ticks = 2, .quote_qty = 100, .max_position = 2000, .min_top_qty = 1};
    static constexpr RiskLimits kLimits{
        .max_order_qty = 1000, .max_order_notional = 50'000'000'000LL,
        .max_position = 2500, .band_ticks = 40, .max_msgs_per_sec = 2'000'000};

    Book                 book_;
    Decoder<Engine>      dec_;
    Strategy             strat_;
    RiskGate             risk_;
    LogQueue&            log_;
    std::array<std::byte, 64> out_{};
    Signal        quote_{false, Side::Buy, Price{}, Qty{}};
    std::uint64_t seq_{0}, client_id_{0};
    std::uint64_t sent_{0}, fills_{0}, gaps_{0}, log_drops_{0};
    std::array<std::uint64_t, 8> rejects_{};
    bool have_quote_{false}, suppressed_{false};
};

}  // namespace tt

// ---------------------------------------------------------------- main -------
int main() {
    using namespace tt;

    pin_to_core(2);
    const std::uint64_t hz = measure_clock_hz();
    const double ns_per_tick = 1e9 / static_cast<double>(hz);

    auto log    = std::make_unique<LogQueue>();          // every allocation happens
    auto engine = std::make_unique<Engine>(10'000, hz, *log);   // here, before the loop
    auto hist   = std::make_unique<Histogram>();

    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> logged{0};
    std::thread cold([&] {                               // cold path, its own core
        pin_to_core(3);
        static constexpr const char* kFmt[kSiteCount] = {
            "quote qty=%u px=%lld", "reject verdict=%u px=%lld", "gap expected=%u got=%lld"};
        char scratch[128];
        LogRecord r{};
        std::uint64_t n = 0;
        for (;;) {
            if (log->try_pop(r)) {
                if (r.site < kSiteCount)                 // real formatting, off the hot path
                    (void)std::snprintf(scratch, sizeof(scratch), kFmt[r.site],
                                        r.u32, static_cast<long long>(r.i64));
                ++n;
                continue;
            }
            if (!running.load(std::memory_order_acquire)) break;
            std::this_thread::yield();
        }
        logged.store(n, std::memory_order_relaxed);
    });

    std::array<std::byte, 2048> packet{};
    FeedGen warm_gen(0xC0FFEE, 10'000);

    engine->suppress(true);            // warm i-cache, d-cache, TLB and the predictor
    for (int i = 0; i < 200'000; ++i) {
        const std::size_t n = warm_gen.next_packet(packet.data(), packet.size());
        engine->on_packet(packet.data(), n, now_ticks());
    }
    engine->suppress(false);
    engine->reset_state();

    FeedGen gen(0x5EED, 10'000);
    std::uint64_t anomalies = 0;
    constexpr int kPackets = 1'000'000;
    for (int i = 0; i < kPackets; ++i) {
        const std::size_t n = gen.next_packet(packet.data(), packet.size());
        const std::uint64_t t0 = now_ticks();      // stands in for the NIC timestamp
        engine->on_packet(packet.data(), n, t0);
        const std::uint64_t t1 = now_ticks();
        if (t1 < t0) [[unlikely]] { ++anomalies; continue; }   // never trust a backwards clock
        hist->record(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * ns_per_tick));
    }

    running.store(false, std::memory_order_relaxed);
    cold.join();

    std::printf("clock            %.3f MHz (%.1f ns per tick)\n",
                static_cast<double>(hz) / 1e6, ns_per_tick);
    std::printf("clock anomalies  %llu\n", static_cast<unsigned long long>(anomalies));
    std::printf("packets          %llu\n", static_cast<unsigned long long>(hist->count()));
    std::printf("orders sent      %llu bytes\n", static_cast<unsigned long long>(engine->sent()));
    std::printf("fills            %llu\n", static_cast<unsigned long long>(engine->fills()));
    std::printf("position         %d\n", engine->position());
    std::printf("book drops       %llu\n", static_cast<unsigned long long>(engine->drops()));
    std::printf("log dropped      %llu\n",
                static_cast<unsigned long long>(engine->log_drops()));
    std::printf("log records      %llu\n",
                static_cast<unsigned long long>(logged.load(std::memory_order_relaxed)));
    static constexpr const char* kVerdict[8] = {
        "ok", "killed", "size", "band", "notional", "position", "self-trade", "rate"};
    for (std::size_t i = 1; i < 8; ++i)
        if (engine->rejects()[i])
            std::printf("reject %-11s %llu\n", kVerdict[i],
                        static_cast<unsigned long long>(engine->rejects()[i]));
    std::printf("tick-to-trade ns  mean %.0f  p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n",
                hist->mean(),
                static_cast<unsigned long long>(hist->pct(0.50)),
                static_cast<unsigned long long>(hist->pct(0.90)),
                static_cast<unsigned long long>(hist->pct(0.99)),
                static_cast<unsigned long long>(hist->pct(0.999)),
                static_cast<unsigned long long>(hist->max()));
    return 0;
}
```

## Build it and run it

:::hft A version you can run right now
The seven files below are the design. A working variant of the same system ships with this
series in `examples/capstone/`, complete with a synthetic feed generator so it needs no
exchange connection:

```sh
$ cd examples/capstone && make run
```

It reports two numbers, and the difference between them is the lesson. An uninstrumented
pass measures the real cost of decode through send, around 25 ns per message on an
unpinned laptop. An instrumented pass reports the per-message distribution, whose median
of about 40 ns is mostly the two clock reads the instrumentation itself added. The maximum,
in the hundreds of microseconds, is the scheduler on an untuned machine.

Read those three numbers together before you optimise anything.
:::


Two configurations and no others. The production one is the only binary whose timings mean
anything; the debug one is the only binary whose correctness claims mean anything.

```text Makefile
CXX      ?= g++
STD       = -std=c++23
WARN      = -Wall -Wextra -Wshadow -Wconversion
ARCH     ?= -march=native          # pin to the deployment target in production

# The binary you ship and the only one whose latency numbers mean anything.
PROD      = $(STD) -O3 $(ARCH) -flto -DNDEBUG -g -pthread

# The binary CI runs for correctness. Never benchmark this one.
DEBUG     = $(STD) -O1 -g -fno-omit-frame-pointer -pthread \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -D_GLIBCXX_ASSERTIONS

HDRS      = types.hpp spsc.hpp book.hpp decoder.hpp strategy.hpp risk.hpp

all: trader trader-debug

trader: main.cpp $(HDRS)
	$(CXX) $(PROD) $(WARN) main.cpp -o $@

trader-debug: main.cpp $(HDRS)
	$(CXX) $(DEBUG) $(WARN) main.cpp -o $@

run: trader
	./trader

check: trader-debug
	./trader-debug

# Run occasionally, not in the default build: -Wpadded fires on every aggregate,
# but it is the only way to catch a message struct that silently grew.
layout:
	$(CXX) $(STD) -O2 -Wpadded -fsyntax-only main.cpp

clean:
	rm -f trader trader-debug

.PHONY: all run check layout clean
```

```text One real run. Your numbers will differ; the shape should not.
$ make run
clock            24.000 MHz (41.7 ns per tick)
clock anomalies  0
packets          1000000
orders sent      14658750 bytes
fills            814374
position         2000
book drops       0
log dropped      0
log records      814461
reject self-trade  85
tick-to-trade ns  mean 120  p50 130  p90 210  p99 295  p99.9 3670  max 294877
```

Read that output the way you would read a production dashboard. `book drops 0` means the
ladder never overflowed and the slab never filled, so the book is a faithful copy of the
feed. `log dropped 0` means the cold thread kept up. `position 2000` is the strategy sitting
against its own limit, which is the limit working. The 85 self-trade rejects are the risk
gate stopping the strategy from quoting through its own resting order.

:::warn
That run was on an Apple M-series laptop, where the only user-readable counter ticks at
24 MHz — about 42 ns of resolution — and where nothing is pinned or isolated. Both facts show
up in the numbers: the percentiles are quantised to multiples of 42, and the p99.9 of 3.6 µs
and 295 µs maximum are the operating system, not the code. On an isolated, pinned core on a
tuned x86-64 server with `rdtsc`, expect a far tighter tail and roughly a third of a
nanosecond of clock resolution. This is exactly the measurement discipline lesson 26 is
about: know what your clock can see and what your machine is doing before you believe a
percentile.
:::

:::hft
The single number to watch is not the mean, it is the gap between p50 and p99.9. Here it is
roughly 28x, which for an unpinned laptop is unremarkable and for a production system would
be a fire. Every technique in Part IV exists to close that gap: isolate the core, disable the
frequency governor, pre-touch and lock the memory, warm the path, and stop anything else from
running on that CPU. Get the ratio under 3x before you optimise a single line of the mean.
:::

## Guided optimisation, and what production adds

:::exercise Twelve changes, each pointing back at a lesson
1. Build with `-O0`, then `-O2`, then `-O3 -march=native -flto`, and record all five
   percentiles for each. Explain the difference before reading on (lesson 01).
2. Replace the price ladder in `book.hpp` with `std::map<std::int64_t, Level>` and
   `std::unordered_map` for the ids. Measure. This is the whole argument of lesson 37 in one
   diff (lessons 10, 37).
3. Delete the 200,000-packet warm-up loop and compare the first thousand samples against the
   steady state. Then put it back and confirm the difference disappears (lesson 39).
4. Make `Decoder`'s handler a virtual base class instead of a template parameter. Look at
   what happens to inlining and to p50 (lesson 23).
5. Replace the binary `LogRecord` push with a direct `std::format` call on the hot path.
   Measure the damage, then revert (lessons 24, 39).
6. Make the `Book` a stack local rather than heap allocated, and watch it fail. Then use
   `mmap` with `MADV_HUGEPAGE` and pre-touch it, and measure the first-message latency
   (lessons 19, 24, 39).
7. Sort the `Level` struct so that a strategy reading five levels of depth touches one cache
   line instead of five. Measure with `perf stat -e L1-dcache-load-misses` (lessons 19, 20, 28).
8. Add `[[likely]]` and `[[unlikely]]` to the branches in `on_packet` based on the actual
   message mix, then check whether the compiler had already worked it out (lesson 22).
9. Pin the hot thread to an isolated core with `isolcpus` and `nohz_full`, pin the cold
   thread elsewhere, and re-measure the tail (lessons 30, 35).
10. Publish `TopOfBook` through a seqlock to a second reader thread and confirm the reader
    never observes a torn price (lesson 34).
11. Turn `min_spread_ticks` and `quote_qty` into template parameters on `Strategy` and
    compare the generated code against the runtime-configured version (lessons 12, 13, 39).
12. Add a `--replay <file>` mode that reads a captured packet stream instead of generating
    one, hash the sequence of orders sent, and make the hash a CI assertion (lessons 26, 40).
:::

What a real system adds, none of which is in these six hundred lines:

- **Gap recovery.** This decoder notices a sequence gap and carries on with a book it knows
  is wrong. A real one arbitrates the A and B feeds, requests retransmits, and re-snapshots
  from the refresh feed, all without stopping (lesson 38).
- **Multi-symbol sharding.** One book, one symbol. Real feeds carry thousands, partitioned
  across handler threads by symbol so each core owns a disjoint set and never shares a line.
- **Session and sequence management.** Order entry needs a logon, heartbeats, sequence number
  negotiation, resend requests and a clean logout, all of which must survive a reconnect
  mid-day without duplicating an order.
- **Drop copy reconciliation.** Exchanges send an independent copy of your fills. Your
  position must be checked against it continuously, because your own view can be wrong and a
  wrong position means every subsequent risk check is wrong.
- **Persistence.** Every order and fill written durably before the next one is sent, so a
  crash does not lose track of live orders.
- **Failover.** A warm standby with the same book state, an arbitration protocol so both
  never quote at once, and a tested procedure for switching.
- **Compliance.** Timestamps at every hop to regulatory precision, audit trails, kill-switch
  records, and the reports that go with them.

Each of those is larger than everything above. That is the honest ratio: the fast part is
small, and the part that keeps you in business is not.

## Takeaways

- A complete tick-to-trade path is a few hundred lines. The hard part is what surrounds it,
  not the loop itself.
- One pinned thread, no allocation, no locks, no syscalls, and every buffer preallocated and
  warmed before the first real message.
- Templates rather than virtual interfaces keep the decode-to-send path free of indirect
  calls and let the whole thing inline into one function.
- Instrument with a raw counter and a fixed-bucket histogram, report percentiles, and know
  your clock's resolution before you trust them.
- Push everything that is not the decision off the path: 24-byte binary records into an SPSC
  queue and a cold thread that does the formatting.
- Measure before and after every one of the twelve exercises. The ones that surprise you are
  the ones worth remembering.
