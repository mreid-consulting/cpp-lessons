---
title: Designing a Fast Order Book
part: Part VI - Trading Systems
summary: The limit order book as a data structure problem. Why the obvious std::map design costs you two hundred nanoseconds, and how a price ladder over a slab of handles gets you to twenty.
time: 45 min
level: expert
tags: order-book, price-ladder, slab-allocation, open-addressing, intrusive-lists
---

The order book is the one data structure a trading system cannot avoid. Every message from
the feed touches it, every strategy decision reads it, and on a busy instrument it is
updated a few hundred thousand times a second. It is also the place where a beginner's
instinct — reach for `std::map`, it is sorted — costs an order of magnitude. This lesson
builds the structure that desks actually run.

## What the book has to do

A **limit order book** is the exchange's record of every resting order in one instrument:
the orders that want to buy at some price or better, and the orders that want to sell. It
is sorted by price, and within each price by arrival time, because exchanges match in
price-time priority. Your local copy is rebuilt from the feed messages decoded in lesson 36.

The operations, and roughly how often each occurs on a liquid US equity:

| Operation | Meaning | Share of messages |
|---|---|---|
| Add | a new resting order at a price | 40-50% |
| Cancel | remove a resting order entirely | 40-50% |
| Modify | change quantity, or price via cancel-replace | 5-10% |
| Execute | a resting order is filled, wholly or partly | 1-5% |

And the reads, which are what the strategy actually cares about:

- **Best bid and offer** (the BBO, or *top of book*): highest bid price and its quantity,
  lowest ask price and its quantity.
- **Depth at level N**: the Nth non-empty price on a side, and the quantity there.
- **Lookup by exchange order id**: cancel and execute messages name an order by a 64-bit
  identifier the exchange assigned, not by price. You must find it before you can remove it.

That last requirement is the one that shapes the design. Every cancel is a lookup in a
sparse 64-bit key space, and cancels are half your traffic.

## Why std::map is the wrong shape

Here is the design that a code review will wave through and a profiler will not.

```cpp The design to avoid
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

struct Order { std::uint64_t id; std::uint32_t qty; };

struct Level {
    std::uint64_t    total_qty;
    std::list<Order> orders;          // time priority, one heap node per order
};

std::map<std::int64_t, Level, std::greater<>> bids;   // sorted descending
std::map<std::int64_t, Level>                 asks;
std::unordered_map<std::uint64_t,
                   std::list<Order>::iterator> by_id;
```

It is correct. It is also, on a typical modern server core, 100-200 ns per update, and here
is where that goes. `std::map` is a red-black tree: finding a price at depth 20 is four or
five node visits, each a dependent load into a separately allocated node, each with a real
chance of missing L1 and L2. That is pointer chasing, and the loads cannot overlap because
the address of the next node is the result of the previous load. Inserting a new price
level allocates. `std::list` allocates a node per order. `std::unordered_map` is, in every
mainstream implementation, an array of bucket pointers into singly linked lists of
separately allocated nodes, so a lookup is a hash, a load, and then a chase.

:::key
The cost is not comparisons or hashing. It is that every one of these containers stores its
elements in individually allocated nodes reachable only through a pointer, so the hardware
prefetcher cannot help and the loads serialise. Fixing the algorithm will not fix this.
Fixing the layout will.
:::

## The price ladder

Prices are not arbitrary. An instrument has a **tick size** — the minimum price increment,
say one cent — and every legal price is an integer multiple of it. So represent price as an
integer count of ticks, as this series has since lesson 02, and the price axis becomes a
dense range of small integers. A dense range of small integers is an array index.

```cpp Types and the ladder window
#include <array>
#include <cstddef>
#include <cstdint>

using Price  = std::int64_t;    // ticks
using Qty    = std::uint32_t;
using Handle = std::uint32_t;   // index into the order slab, not a pointer

inline constexpr Handle       kNull   = 0xFFFF'FFFFu;
inline constexpr std::int32_t kLevels = 4096;         // ladder slots per side

struct Level {                  // 16 bytes: four levels per cache line
    std::uint64_t qty;          // aggregate resting quantity at this price
    Handle        head;         // oldest order at this price
    Handle        tail;
};

std::array<Level, kLevels> bid;
std::array<Level, kLevels> ask;

// The whole indexing scheme, and it is O(1).
inline std::int32_t slot_of(Price px, Price base) noexcept {
    return static_cast<std::int32_t>(px - base);
}
```

An add becomes a subtraction, a bounds check, and a read-modify-write of sixteen contiguous
bytes. One cache line is touched. There is no allocation, no rebalancing, no comparison
chain.

The memory arithmetic decides the window. At 16 bytes per level, 4096 slots is 64 KB per
side and 128 KB for both, which fits comfortably in a typical 1-2 MB L2 and leaves the few
lines you actually touch resident in L1. At a one-cent tick that window spans $40.96, which
covers a normal trading day for most names but not a halt-and-reopen. Widening to a full
$1-to-$1000 static range costs 100,000 levels, or 1.6 MB per side — still viable for a
handful of instruments, wasteful for five thousand.

Three ways to handle a price that leaves the window:

1. **Rebase.** When the touch drifts past a guard band, pick a new base, `memmove` the live
   region, and recompute. It is a few microseconds and it happens a handful of times a day,
   so do it off the hot path if you can, and never mid-message.
2. **Circular window.** Index with `px & (kLevels - 1)` and store the level's own price so a
   stale slot is detected on read. No move, but every access pays one extra comparison and
   an aliasing price wraps onto a live slot, so you must still evict.
3. **A static range wide enough that it cannot happen.** The right answer for a future with
   a fixed tick and a known limit-up/limit-down band.

Sparse and wide-tick instruments break the assumption that the ladder is dense. A stock
trading at $3 with a one-cent tick has a few hundred meaningful prices; a bond quoted in
32nds, or an option at $0.05 wide, has a ladder that is almost entirely empty. The hybrid
is the standard answer: a ladder for the ±N ticks around the touch, where all the traffic
is, and a small sorted vector or hash map for the tail, which is read rarely and never on
the critical path.

## Orders in a slab, and finding them by id

Orders live in one preallocated array — a **slab** — and are referred to by a 32-bit
`Handle`, which is just the index. Handles beat pointers here for three reasons: they are
half the size, so a linked-list node is 32 bytes instead of 48; they survive the whole
structure being copied or snapshotted; and a free handle can be reused without the ABA
problems a recycled address invites. The free list threads through the `next` field of the
free entries, so it costs nothing.

The id map is the other half. `std::unordered_map` is the wrong container for the reason
above: chaining means a pointer chase on every lookup, and half your messages are lookups.
An open-addressing table with linear probing puts keys and values in two flat arrays, so a
lookup is a hash, one load, and — on the common case of no collision — you are done. The
probe sequence walks forward through contiguous memory, which is exactly what the hardware
prefetcher is built for.

```cpp Open-addressing id map with backward-shift deletion
class IdMap {
public:
    static constexpr std::size_t kSlots = 1u << 17;   // power of two, >= 2x live orders
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
        keys_[i] = k;
        vals_[i] = v;
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
        for (;;) {                       // Knuth backward shift: no tombstones
            vals_[i] = kNull;
            for (;;) {
                j = (j + 1) & kMask;
                if (vals_[j] == kNull) return;
                if (!inside(mix(keys_[j]) & kMask, i, j)) break;
            }
            keys_[i] = keys_[j];
            vals_[i] = vals_[j];
            i = j;
        }
    }

private:
    // is the home slot cyclically inside (i, j] ?
    static constexpr bool inside(std::size_t home, std::size_t i, std::size_t j) noexcept {
        return (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
    }
    std::array<std::uint64_t, kSlots> keys_{};
    std::array<Handle, kSlots>        vals_{};
};
```

:::pitfall
Linear probing degrades badly above about 70% load, and the usual tombstone-based erase
degrades it further because deleted slots still lengthen probe chains. Backward-shift
deletion, above, restores the table to the state it would have been in had the key never
been inserted. Size the table at twice your worst-case live order count and never resize it
at run time.
:::

## Time priority, top of book, and the strategy's cache line

Within a price level, orders are matched oldest first, so each level owns a doubly linked
list. **Intrusive** means the links live inside the `Order` object rather than in separate
node wrappers, so there is no allocation and unlinking is two stores. Because the links are
handles, an order is 32 bytes and two of them share a cache line.

Best bid and best ask are maintained, never searched. An add that lands at a better price
updates the index with one comparison. A removal that empties the best level walks outward
until it finds a non-empty one, which is one iteration in the overwhelming majority of cases
and bounded by the ladder width in the worst.

The strategy should not walk your book at all. Publish a flat, aligned snapshot:

```cpp One cache line, one load, no chasing
struct alignas(64) TopOfBook {
    std::int64_t  bid_px;    // ticks
    std::int64_t  ask_px;
    std::uint64_t bid_qty;
    std::uint64_t ask_qty;
    std::uint64_t seq;       // sequence number; see the seqlock in lesson 34
};
static_assert(sizeof(TopOfBook) == 64);
```

Forty bytes of payload in a 64-byte object on its own cache line. A strategy reading price
and size touches exactly one line, and the `alignas` guarantees the writer never falsely
shares with a neighbour, which is the failure lesson 21 covers.

## The book, assembled

```cpp book.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hft {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline constexpr std::size_t kMaxOrders = 1u << 16;   // live orders per book

struct Order {                  // 32 bytes: two per cache line
    std::uint64_t exch_id;
    Qty           qty;
    Handle        prev;
    Handle        next;
    std::int32_t  slot;         // ladder index; price is base_ + slot
    Side          side;
};

class Book {                    // ~3.8 MB: allocate once, never on the stack
public:
    explicit Book(Price base) noexcept { reset(base); }

    void reset(Price base) noexcept {
        base_     = base;
        best_bid_ = -1;
        best_ask_ = kLevels;
        bid_.fill(Level{0, kNull, kNull});
        ask_.fill(Level{0, kNull, kNull});
        for (Handle i = 0; i + 1 < kMaxOrders; ++i) slab_[i].next = i + 1;
        slab_[kMaxOrders - 1].next = kNull;
        free_head_ = 0;
        ids_.clear();
    }

    // kNull means the price is outside the window or the slab is full.
    Handle add(std::uint64_t id, Side s, Price px, Qty q) noexcept {
        const std::int32_t slot = static_cast<std::int32_t>(px - base_);
        if (slot < 0 || slot >= kLevels) [[unlikely]] return kNull;
        if (free_head_ == kNull) [[unlikely]] return kNull;

        const Handle h = free_head_;
        Order& o   = slab_[h];
        free_head_ = o.next;

        o.exch_id = id;
        o.qty     = q;
        o.slot    = slot;
        o.side    = s;
        o.next    = kNull;

        Level& lv = level(s, slot);
        o.prev = lv.tail;
        if (lv.tail != kNull) slab_[lv.tail].next = h; else lv.head = h;
        lv.tail  = h;
        lv.qty  += q;

        if (s == Side::Buy) { if (slot > best_bid_) best_bid_ = slot; }
        else                { if (slot < best_ask_) best_ask_ = slot; }

        ids_.insert(id, h);
        return h;
    }

    void cancel(std::uint64_t id) noexcept {
        const Handle h = ids_.find(id);
        if (h == kNull) [[unlikely]] return;
        unlink(h);
        ids_.erase(id);
    }

    void execute(std::uint64_t id, Qty filled) noexcept {
        const Handle h = ids_.find(id);
        if (h == kNull) [[unlikely]] return;
        Order& o    = slab_[h];
        const Qty f = filled < o.qty ? filled : o.qty;
        level(o.side, o.slot).qty -= f;
        o.qty -= f;
        if (o.qty == 0) { unlink(h); ids_.erase(id); }
    }

    // A size reduction keeps time priority. Anything else is a cancel-replace.
    void reduce(std::uint64_t id, Qty new_qty) noexcept {
        const Handle h = ids_.find(id);
        if (h == kNull) [[unlikely]] return;
        Order& o = slab_[h];
        if (new_qty >= o.qty) [[unlikely]] return;
        level(o.side, o.slot).qty -= (o.qty - new_qty);
        o.qty = new_qty;
    }

    void replace(std::uint64_t id, std::uint64_t new_id, Price px, Qty q) noexcept {
        const Handle h = ids_.find(id);
        if (h == kNull) [[unlikely]] return;
        const Side s = slab_[h].side;
        unlink(h);
        ids_.erase(id);
        add(new_id, s, px, q);
    }

    [[nodiscard]] bool  has_bid() const noexcept { return best_bid_ >= 0; }
    [[nodiscard]] bool  has_ask() const noexcept { return best_ask_ < kLevels; }
    [[nodiscard]] Price bid_px()  const noexcept { return base_ + best_bid_; }
    [[nodiscard]] Price ask_px()  const noexcept { return base_ + best_ask_; }
    [[nodiscard]] std::uint64_t bid_qty() const noexcept {
        return bid_[static_cast<std::size_t>(best_bid_)].qty;
    }
    [[nodiscard]] std::uint64_t ask_qty() const noexcept {
        return ask_[static_cast<std::size_t>(best_ask_)].qty;
    }

    [[nodiscard]] TopOfBook top(std::uint64_t seq) const noexcept {
        return TopOfBook{has_bid() ? bid_px()  : 0, has_ask() ? ask_px()  : 0,
                         has_bid() ? bid_qty() : 0, has_ask() ? ask_qty() : 0, seq};
    }

    // Writes the n best non-empty levels on one side; returns how many were found.
    std::size_t depth(Side s, std::size_t n, Price* px, std::uint64_t* qty) const noexcept {
        std::size_t k = 0;
        if (s == Side::Buy) {
            for (std::int32_t i = best_bid_; i >= 0 && k < n; --i)
                if (bid_[static_cast<std::size_t>(i)].qty != 0) {
                    px[k] = base_ + i; qty[k] = bid_[static_cast<std::size_t>(i)].qty; ++k;
                }
        } else {
            for (std::int32_t i = best_ask_; i < kLevels && k < n; ++i)
                if (ask_[static_cast<std::size_t>(i)].qty != 0) {
                    px[k] = base_ + i; qty[k] = ask_[static_cast<std::size_t>(i)].qty; ++k;
                }
        }
        return k;
    }

private:
    Level& level(Side s, std::int32_t slot) noexcept {
        return (s == Side::Buy ? bid_ : ask_)[static_cast<std::size_t>(slot)];
    }

    void unlink(Handle h) noexcept {
        Order& o  = slab_[h];
        Level& lv = level(o.side, o.slot);
        if (o.prev != kNull) slab_[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != kNull) slab_[o.next].prev = o.prev; else lv.tail = o.prev;
        lv.qty -= o.qty;
        if (lv.head == kNull) repair_best(o.side, o.slot);
        o.next     = free_head_;
        free_head_ = h;
    }

    void repair_best(Side s, std::int32_t slot) noexcept {
        if (s == Side::Buy) {
            if (slot != best_bid_) return;
            while (best_bid_ >= 0 && bid_[static_cast<std::size_t>(best_bid_)].head == kNull)
                --best_bid_;
        } else {
            if (slot != best_ask_) return;
            while (best_ask_ < kLevels && ask_[static_cast<std::size_t>(best_ask_)].head == kNull)
                ++best_ask_;
        }
    }

    Price        base_{0};
    std::int32_t best_bid_{-1};
    std::int32_t best_ask_{kLevels};
    Handle       free_head_{kNull};
    std::array<Level, kLevels>    bid_{};
    std::array<Level, kLevels>    ask_{};
    std::array<Order, kMaxOrders> slab_{};
    IdMap                         ids_;
};

}  // namespace hft
```

Correctness is not something you argue about; it is something you diff. Build a reference
book out of the containers from the first section, which is obviously right because it is
obviously simple, run a recorded feed through both, and compare after every message.

```cpp Differential test against a reference book
#include <cassert>
#include <memory>

void replay(const Message* msgs, std::size_t n) {
    auto fast = std::make_unique<hft::Book>(hft::Price{10'000});
    ReferenceBook slow;                      // std::map based, from section two
    for (std::size_t i = 0; i < n; ++i) {
        apply(*fast, msgs[i]);
        apply(slow,  msgs[i]);
        assert(fast->has_bid() == slow.has_bid());
        if (fast->has_bid()) {
            assert(fast->bid_px()  == slow.bid_px());
            assert(fast->bid_qty() == slow.bid_qty());
        }
        // and the same for the ask, plus the top five levels each side
    }
}
```

:::hft
On a typical modern server core, with the working set warm, expect roughly 20-40 ns for an
add or cancel through this structure, of which the id-map lookup is usually the largest
single component. The `std::map` design measures 100-200 ns on the same feed. Neither figure
is a law of nature — measure yours with the methods from lesson 26, on your hardware, with
your feed — but the ratio is stable, and it is the difference between being first to a quote
and being second.
:::

:::exercise
Take the differential test above and drive it with a recorded morning of one liquid symbol.
Then break things deliberately: shrink `kLevels` to 64 and confirm the book refuses
out-of-window adds instead of corrupting memory; shrink `IdMap::kSlots` to just above the
peak live-order count and measure how much the average cancel slows down as load factor
climbs past 70%; and remove the `repair_best` call from `unlink` to see how a stale best bid
manifests — it will not crash, it will quietly quote a price that is no longer there.
:::

## Takeaways

- The book's cost is dominated by pointer chasing and allocation, not by algorithmic
  complexity. Change the layout, not the big-O.
- Integer ticks turn the price axis into an array index. A flat ladder gives O(1) update and
  touches one cache line.
- Size the ladder window deliberately, and decide up front whether you rebase, wrap, or make
  it wide enough to never matter.
- Keep orders in a preallocated slab addressed by 32-bit handles, with intrusive links for
  time priority and a free list threaded through the unused entries.
- Cancels are half your traffic and every one is a lookup by exchange id. An open-addressing
  table with backward-shift deletion is the right container; `std::unordered_map` is not.
- Maintain the BBO incrementally and publish it as one aligned cache line. Prove the whole
  thing with a differential replay against a deliberately naive reference book.
