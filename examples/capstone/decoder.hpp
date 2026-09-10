// Zero-copy decoder over a receive buffer.
// Reads never assume the buffer is aligned: every field goes through memcpy,
// which the compiler turns into a single load.
#pragma once

#include "types.hpp"

#include <bit>
#include <cstddef>
#include <cstring>
#include <span>

namespace hft {

template <typename T>
[[nodiscard]] inline T load_be(const std::byte* p) noexcept {
    T v{};
    std::memcpy(&v, p, sizeof(T));
    if constexpr (sizeof(T) > 1) {
        if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    }
    return v;
}

struct DecodedHeader {
    MsgType type;
    std::uint16_t length;
    std::uint32_t seq;
};

[[nodiscard]] inline DecodedHeader decode_header(const std::byte* p) noexcept {
    return DecodedHeader{static_cast<MsgType>(load_be<std::uint8_t>(p)),
                         load_be<std::uint16_t>(p + 1),
                         load_be<std::uint32_t>(p + 3)};
}

[[nodiscard]] inline AddOrder decode_add(const std::byte* p) noexcept {
    return AddOrder{load_be<std::uint64_t>(p + 7),
                    Price{load_be<std::int64_t>(p + 15)},
                    load_be<std::uint32_t>(p + 23),
                    load_be<std::uint8_t>(p + 27) == 'B' ? Side::Buy : Side::Sell};
}

[[nodiscard]] inline CancelOrder decode_cancel(const std::byte* p) noexcept {
    return CancelOrder{load_be<std::uint64_t>(p + 7), load_be<std::uint32_t>(p + 15)};
}

[[nodiscard]] inline Execute decode_execute(const std::byte* p) noexcept {
    return Execute{load_be<std::uint64_t>(p + 7), load_be<std::uint32_t>(p + 15)};
}

// Tracks the exchange sequence number so a gap is detected rather than ignored.
class SequenceGuard {
public:
    // Returns false when this message is a duplicate or leaves a gap.
    bool accept(std::uint32_t seq) noexcept {
        if (expected_ == 0) { expected_ = seq + 1; return true; }
        if (seq == expected_) { ++expected_; return true; }
        if (seq < expected_) { ++duplicates_; return false; }
        gaps_ += seq - expected_;
        expected_ = seq + 1;
        return false;
    }

    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
    [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }

private:
    std::uint32_t expected_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t duplicates_ = 0;
};

}  // namespace hft
