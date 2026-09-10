// A deliberately simple market-making rule, and the pre-trade risk gate it
// must pass. Both are branch-light and allocation free.
#pragma once

#include "types.hpp"
#include "book.hpp"

namespace hft {

struct StrategyConfig {
    Ticks min_spread = 2;      // quote only when the book is at least this wide
    Ticks join_offset = 1;     // improve the touch by this many ticks
    Qty   order_qty = 100;
    std::int64_t max_position = 1000;
};

class Strategy {
public:
    explicit Strategy(StrategyConfig cfg) noexcept : cfg_(cfg) {}

    // Called on every book update. Returns an order, or an invalid one.
    // Requoting only when the target actually moves is what keeps the outbound
    // message rate proportional to real market activity rather than to feed volume.
    [[nodiscard]] OutboundOrder on_book(const Book& book) noexcept {
        if (!book.has_both_sides()) [[unlikely]] return {};
        if (book.spread() < cfg_.min_spread) return {};

        // Lean against inventory: quote the side that reduces position.
        const Side side = position_ > 0 ? Side::Sell : Side::Buy;
        if (side == Side::Buy && position_ >= cfg_.max_position) return {};
        if (side == Side::Sell && position_ <= -cfg_.max_position) return {};

        // Micro-price: the touch weighted by the opposite side's size. A book
        // that is heavy on the bid implies the next trade prints nearer the ask.
        // One division per update is a deliberate simplification; lesson 40
        // covers replacing it with a reciprocal multiply.
        const std::int64_t bid = book.best_bid().v;
        const std::int64_t ask = book.best_ask().v;
        const std::int64_t bq = book.bid_qty();
        const std::int64_t aq = book.ask_qty();
        const std::int64_t total = bq + aq;
        const std::int64_t micro = total > 0 ? (bid * aq + ask * bq) / total : (bid + ask) / 2;

        Price px{side == Side::Buy ? micro - cfg_.join_offset : micro + cfg_.join_offset};
        if (px.v <= bid) px.v = bid + 1;          // stay inside the touch
        if (px.v >= ask) px.v = ask - 1;

        if (px == last_px_ && side == last_side_) return {};
        last_px_ = px;
        last_side_ = side;
        return OutboundOrder{px, cfg_.order_qty, side, true};
    }

    void on_fill(Side side, Qty qty) noexcept {
        position_ += (side == Side::Buy ? 1 : -1) * static_cast<std::int64_t>(qty);
    }

    [[nodiscard]] std::int64_t position() const noexcept { return position_; }

private:
    StrategyConfig cfg_;
    std::int64_t position_ = 0;
    Price last_px_{-1};
    Side last_side_ = Side::Buy;
};

}  // namespace hft
