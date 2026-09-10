// Synthetic feed generator, so the skeleton runs without an exchange connection.
// Encodes the same big-endian wire format the decoder reads.
#pragma once

#include "types.hpp"

#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace hft {

template <typename T>
inline void store_be(std::byte* p, T v) noexcept {
    if constexpr (sizeof(T) > 1) {
        if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    }
    std::memcpy(p, &v, sizeof(T));
}

class FeedGenerator {
public:
    FeedGenerator(Price base, std::uint64_t seed) noexcept
        : base_(base), mid_(base), s_(seed) {}

    // Fills `out` with encoded messages and returns the number of bytes written.
    // Called once at startup: the hot loop reads the buffer, it does not build it.
    std::size_t generate(std::span<std::byte> out, std::size_t message_count) noexcept {
        std::size_t off = 0;
        live_.reserve(kTargetLive * 2);
        for (std::size_t i = 0; i < message_count; ++i) {

            // Bias towards removal once the resting book reaches its target size,
            // so the number of live orders stays bounded.
            const bool crowded = live_.size() >= kTargetLive;
            const std::uint32_t roll = static_cast<std::uint32_t>(next() % 100);
            const bool add = live_.empty() || (crowded ? roll < 40 : roll < 60);

            if (add) {
                if (off + sizeof(WireAdd) > out.size()) break;
                off += encode_add(out.data() + off);
            } else if (roll < 85) {
                if (off + sizeof(WireCancel) > out.size()) break;
                off += encode_cancel(out.data() + off);
            } else {
                if (off + sizeof(WireExecute) > out.size()) break;
                off += encode_execute(out.data() + off);
            }
        }
        return off;
    }

private:
    std::uint64_t next() noexcept {
        s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17;
        return s_;
    }

    std::size_t encode_add(std::byte* p) noexcept {
        const bool buy = (next() & 1) != 0;
        // Depth is exponential-ish: most orders sit near the touch, few deep.
        const std::uint64_t r = next() % 64;
        const Ticks offset = static_cast<Ticks>(r < 32 ? 1 + (r % 4) : 1 + (r % 10));
        const Ticks px = mid_.v + (buy ? -offset : offset);
        const OrderId id = ++next_id_;

        store_be<std::uint8_t>(p, static_cast<std::uint8_t>(MsgType::Add));
        store_be<std::uint16_t>(p + 1, sizeof(WireAdd));
        store_be<std::uint32_t>(p + 3, seq_++);
        store_be<std::uint64_t>(p + 7, id);
        store_be<std::int64_t>(p + 15, px);
        store_be<std::uint32_t>(p + 23, static_cast<std::uint32_t>(next() % 500) + 1);
        store_be<std::uint8_t>(p + 27, buy ? 'B' : 'S');

        live_.push_back(id);
        return sizeof(WireAdd);
    }

    std::size_t encode_cancel(std::byte* p) noexcept {
        const OrderId id = take_live();
        store_be<std::uint8_t>(p, static_cast<std::uint8_t>(MsgType::Cancel));
        store_be<std::uint16_t>(p + 1, sizeof(WireCancel));
        store_be<std::uint32_t>(p + 3, seq_++);
        store_be<std::uint64_t>(p + 7, id);
        store_be<std::uint32_t>(p + 15, 0);
        return sizeof(WireCancel);
    }

    std::size_t encode_execute(std::byte* p) noexcept {
        const OrderId id = take_live();
        store_be<std::uint8_t>(p, static_cast<std::uint8_t>(MsgType::Execute));
        store_be<std::uint16_t>(p + 1, sizeof(WireExecute));
        store_be<std::uint32_t>(p + 3, seq_++);
        store_be<std::uint64_t>(p + 7, id);
        store_be<std::uint32_t>(p + 15, 10);   // partial fill
        // A partial fill leaves the order resting, so it stays cancellable.
        // Dropping it here would leak a level in the book that nothing removes.
        live_.push_back(id);
        return sizeof(WireExecute);
    }

    OrderId take_live() noexcept {
        const std::size_t i = static_cast<std::size_t>(next() % live_.size());
        const OrderId id = live_[i];
        live_[i] = live_.back();
        live_.pop_back();
        return id;
    }

    // A single-symbol book holds hundreds of resting orders, not tens of
    // thousands. Keeping it small is what lets price levels empty and refill,
    // which is what makes the touch move and the spread vary.
    //
    // The reference price is fixed rather than random-walking. Resting orders
    // outlive the walk, so a drifting mid leaves stale bids above stale asks and
    // the book crosses. A real feed resolves that with executions against the
    // crossing side; a generator this small resolves it by not creating the
    // situation.
    static constexpr std::size_t kTargetLive = 24;

    Price base_;
    Price mid_;
    std::uint64_t s_;
    std::uint64_t next_id_ = 0;
    std::uint32_t seq_ = 1;
    std::vector<OrderId> live_;   // startup only; never touched by the hot loop
};

}  // namespace hft
