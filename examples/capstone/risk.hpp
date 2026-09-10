// Pre-trade risk. Every check is a comparison on data already in registers or
// L1, so the whole gate costs a few nanoseconds and runs on every outbound order.
#pragma once

#include "types.hpp"

#include <cstdint>

namespace hft {

struct RiskLimits {
    Qty max_order_qty = 1000;
    std::int64_t max_position = 5000;
    Ticks band_ticks = 500;          // fat-finger guard around the touch
    std::uint32_t max_orders_per_sec = 100000;
};

enum class RiskResult : std::uint8_t {
    Accept = 0, QtyTooLarge, PositionLimit, PriceOutOfBand, RateLimit, KillSwitch
};

class RiskGate {
public:
    explicit RiskGate(RiskLimits limits) noexcept : limits_(limits) {}

    // Every check is a comparison on data already in registers or L1.
    [[nodiscard]] RiskResult check(const OutboundOrder& o, std::int64_t position,
                                   Price reference, std::uint64_t now_ns) noexcept {
        if (killed_) [[unlikely]] return RiskResult::KillSwitch;
        if (o.qty > limits_.max_order_qty) [[unlikely]] return RiskResult::QtyTooLarge;

        const std::int64_t signed_qty =
            (o.side == Side::Buy ? 1 : -1) * static_cast<std::int64_t>(o.qty);
        const std::int64_t projected = position + signed_qty;
        if (projected > limits_.max_position || projected < -limits_.max_position)
            [[unlikely]] return RiskResult::PositionLimit;

        const Ticks delta = o.price.v - reference.v;
        if (delta > limits_.band_ticks || delta < -limits_.band_ticks)
            [[unlikely]] return RiskResult::PriceOutOfBand;

        if (now_ns - window_start_ns_ >= 1'000'000'000ULL) {
            window_start_ns_ = now_ns;
            in_window_ = 0;
        }
        if (++in_window_ > limits_.max_orders_per_sec) [[unlikely]] return RiskResult::RateLimit;

        return RiskResult::Accept;
    }

    void kill() noexcept { killed_ = true; }
    [[nodiscard]] bool killed() const noexcept { return killed_; }

private:
    RiskLimits limits_;
    std::uint64_t window_start_ns_ = 0;
    std::uint32_t in_window_ = 0;
    bool killed_ = false;
};

}  // namespace hft
