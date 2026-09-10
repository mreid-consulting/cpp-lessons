// Price-ladder order book.
//
// Levels are a flat array indexed by price relative to a fixed base, so an
// update is one bounds check and one add. Orders live in a preallocated slab
// addressed by 32-bit handles, and the exchange order id maps to a handle
// through an open-addressing table sized to a power of two.
#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hft {

inline constexpr std::size_t kLevels = 4096;      // ticks either side of base
inline constexpr std::size_t kMaxOrders = 1u << 16;
inline constexpr std::size_t kIdTableSize = 1u << 17;   // load factor 0.5

class Book {
public:
    explicit Book(Price base) noexcept : base_(base) { clear(); }

    void clear() noexcept {
        bid_qty_.fill(0);
        ask_qty_.fill(0);
        for (auto& e : id_table_) e = Entry{};
        free_head_ = 0;
        for (Handle h = 0; h + 1 < kMaxOrders; ++h) orders_[h].next_free = h + 1;
        orders_[kMaxOrders - 1].next_free = kNoHandle;
        best_bid_ = kNoIndex;
        best_ask_ = kNoIndex;
        live_ = 0;
    }

    void on_add(const AddOrder& a) noexcept {
        const std::size_t idx = index_of(a.price);
        if (idx == kNoIndex) [[unlikely]] return;      // outside our window

        const Handle h = alloc();
        if (h == kNoHandle) [[unlikely]] return;       // slab exhausted

        orders_[h] = Order{a.id, static_cast<std::uint32_t>(idx), a.qty, a.side, kNoHandle};
        insert_id(a.id, h);

        if (a.side == Side::Buy) {
            bid_qty_[idx] += a.qty;
            if (best_bid_ == kNoIndex || idx > best_bid_) best_bid_ = idx;
        } else {
            ask_qty_[idx] += a.qty;
            if (best_ask_ == kNoIndex || idx < best_ask_) best_ask_ = idx;
        }
    }

    void on_reduce(OrderId id, Qty qty) noexcept {
        const Handle h = find_id(id);
        if (h == kNoHandle) [[unlikely]] return;

        Order& o = orders_[h];
        const Qty take = (qty == 0 || qty > o.qty) ? o.qty : qty;
        o.qty -= take;

        if (o.side == Side::Buy) {
            bid_qty_[o.level] -= take;
            if (o.level == best_bid_ && bid_qty_[o.level] == 0) rescan_bid();
        } else {
            ask_qty_[o.level] -= take;
            if (o.level == best_ask_ && ask_qty_[o.level] == 0) rescan_ask();
        }

        if (o.qty == 0) {
            erase_id(id);
            release(h);
        }
    }

    [[nodiscard]] bool has_both_sides() const noexcept {
        return best_bid_ != kNoIndex && best_ask_ != kNoIndex;
    }
    [[nodiscard]] Price best_bid() const noexcept { return price_of(best_bid_); }
    [[nodiscard]] Price best_ask() const noexcept { return price_of(best_ask_); }
    [[nodiscard]] Qty bid_qty() const noexcept { return bid_qty_[best_bid_]; }
    [[nodiscard]] Qty ask_qty() const noexcept { return ask_qty_[best_ask_]; }
    [[nodiscard]] Ticks spread() const noexcept {
        return static_cast<Ticks>(best_ask_) - static_cast<Ticks>(best_bid_);
    }
    [[nodiscard]] std::uint32_t live_orders() const noexcept { return live_; }

private:
    static constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

    struct Order {
        OrderId id = 0;
        std::uint32_t level = 0;
        Qty qty = 0;
        Side side = Side::Buy;
        Handle next_free = kNoHandle;
    };

    struct Entry {
        OrderId id = 0;
        Handle handle = kNoHandle;
    };

    [[nodiscard]] std::size_t index_of(Price p) const noexcept {
        const Ticks d = p.v - base_.v + static_cast<Ticks>(kLevels / 2);
        if (d < 0 || d >= static_cast<Ticks>(kLevels)) return kNoIndex;
        return static_cast<std::size_t>(d);
    }

    [[nodiscard]] Price price_of(std::size_t idx) const noexcept {
        return Price{base_.v + static_cast<Ticks>(idx) - static_cast<Ticks>(kLevels / 2)};
    }

    Handle alloc() noexcept {
        const Handle h = free_head_;
        if (h == kNoHandle) return kNoHandle;
        free_head_ = orders_[h].next_free;
        ++live_;
        return h;
    }

    void release(Handle h) noexcept {
        orders_[h].next_free = free_head_;
        free_head_ = h;
        --live_;
    }

    // Multiplicative hash; the table is a power of two so the mask is one AND.
    static constexpr std::size_t hash(OrderId id) noexcept {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> 40) & (kIdTableSize - 1);
    }

    void insert_id(OrderId id, Handle h) noexcept {
        std::size_t i = hash(id);
        while (id_table_[i].handle != kNoHandle) i = (i + 1) & (kIdTableSize - 1);
        id_table_[i] = Entry{id, h};
    }

    [[nodiscard]] Handle find_id(OrderId id) const noexcept {
        std::size_t i = hash(id);
        for (std::size_t probes = 0; probes < kIdTableSize; ++probes) {
            const Entry& e = id_table_[i];
            if (e.handle == kNoHandle) return kNoHandle;
            if (e.id == id) return e.handle;
            i = (i + 1) & (kIdTableSize - 1);
        }
        return kNoHandle;
    }

    // Backward-shift deletion keeps the probe chains intact.
    void erase_id(OrderId id) noexcept {
        std::size_t i = hash(id);
        while (id_table_[i].handle != kNoHandle && id_table_[i].id != id)
            i = (i + 1) & (kIdTableSize - 1);
        if (id_table_[i].handle == kNoHandle) return;

        std::size_t gap = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & (kIdTableSize - 1);
            if (id_table_[j].handle == kNoHandle) break;
            const std::size_t home = hash(id_table_[j].id);
            const bool movable = (gap <= j) ? !(home > gap && home <= j)
                                            : !(home > gap || home <= j);
            if (movable) {
                id_table_[gap] = id_table_[j];
                gap = j;
            }
        }
        id_table_[gap] = Entry{};
    }

    void rescan_bid() noexcept {
        while (best_bid_ != kNoIndex && bid_qty_[best_bid_] == 0) {
            if (best_bid_ == 0) { best_bid_ = kNoIndex; return; }
            --best_bid_;
        }
    }

    void rescan_ask() noexcept {
        while (best_ask_ != kNoIndex && ask_qty_[best_ask_] == 0) {
            if (best_ask_ + 1 >= kLevels) { best_ask_ = kNoIndex; return; }
            ++best_ask_;
        }
    }

    Price base_;
    std::size_t best_bid_ = kNoIndex;
    std::size_t best_ask_ = kNoIndex;
    std::uint32_t live_ = 0;
    Handle free_head_ = 0;

    std::array<Qty, kLevels> bid_qty_{};
    std::array<Qty, kLevels> ask_qty_{};
    std::array<Order, kMaxOrders> orders_{};
    std::array<Entry, kIdTableSize> id_table_{};
};

}  // namespace hft
