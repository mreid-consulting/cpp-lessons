// Vocabulary for the tick-to-trade skeleton.
// Prices are integer ticks. Nothing here allocates, throws, or is polymorphic.
#pragma once

#include <cstdint>
#include <compare>

namespace hft {

using Ticks   = std::int64_t;   // price, in whole ticks
using Qty     = std::uint32_t;
using OrderId = std::uint64_t;  // exchange-assigned
using Handle  = std::uint32_t;  // index into our own slab

inline constexpr Handle kNoHandle = 0xFFFFFFFFu;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Strong wrapper so a price cannot be passed where a quantity is expected.
struct Price {
    Ticks v = 0;
    friend constexpr auto operator<=>(Price, Price) = default;
    constexpr Price operator+(Ticks d) const noexcept { return Price{v + d}; }
    constexpr Price operator-(Ticks d) const noexcept { return Price{v - d}; }
};

// ---------------------------------------------------------------- wire format

// Fixed-layout, big-endian on the wire, one byte of type then a fixed body.
enum class MsgType : std::uint8_t { Add = 'A', Cancel = 'X', Execute = 'E' };

#pragma pack(push, 1)
struct WireHeader {
    std::uint8_t  type;
    std::uint16_t length;      // total bytes including this header
    std::uint32_t seq;
};

struct WireAdd {
    WireHeader    hdr;
    std::uint64_t order_id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint8_t  side;        // 'B' or 'S'
};

struct WireCancel {
    WireHeader    hdr;
    std::uint64_t order_id;
    std::uint32_t qty;         // 0 means the whole order
};

struct WireExecute {
    WireHeader    hdr;
    std::uint64_t order_id;
    std::uint32_t qty;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == 7);
static_assert(sizeof(WireAdd) == 28);

// ------------------------------------------------------------ decoded events

struct AddOrder {
    OrderId id;
    Price   price;
    Qty     qty;
    Side    side;
};

struct CancelOrder {
    OrderId id;
    Qty     qty;
};

struct Execute {
    OrderId id;
    Qty     qty;
};

// -------------------------------------------------------------- our own order

struct OutboundOrder {
    Price price;
    Qty   qty;
    Side  side;
    bool  valid = false;
};

}  // namespace hft
