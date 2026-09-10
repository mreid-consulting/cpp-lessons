---
title: Decoding Binary Market Data
part: Part VI - Trading Systems
summary: ITCH, SBE and FIX on the wire. Framing, byte order, reading structs out of a buffer without undefined behaviour, dispatch that predicts, and a decode budget under 100 ns.
time: 40 min
level: advanced
tags: itch, sbe, fix, parsing, swar, framing
---

Decoding is the first thing that happens after a packet lands and the last place anyone
looks for latency. It is also the stage where correctness and speed are least in tension:
the fast way to read a fixed-layout binary message is also the simple way, provided you know
which three C++ constructs are legal and which one everybody reaches for by reflex is
undefined behaviour. This lesson builds a decoder for an ITCH-shaped feed and puts a number
on every part of it.

## The protocol landscape

| Family | Shape | Cost to decode |
|---|---|---|
| Nasdaq ITCH | Fixed-layout binary, big-endian, length-prefixed, framed by MoldUDP64 | Field offsets are compile-time constants |
| Nasdaq OUCH / SoupBinTCP | Fixed-layout binary order entry over TCP | Same, plus stream reassembly |
| CME MDP3, Eurex EOBI | SBE and similar: fixed-position fields, little-endian | Compile-time offsets, no byte swap on x86 |
| FIX 4.x / 5.x | ASCII `tag=value` separated by `0x01` | Scan every byte, branch per delimiter |
| FAST | Presence bitmap plus stop-bit varints, stateful | Sequential, stateful, cannot skip fields |

The dividing line that matters is **fixed layout versus not**. In ITCH, SBE, EOBI and OUCH
the price field of an add-order message is always at the same byte offset, so reading it is
one load at a constant displacement. In FIX and FAST it is wherever the preceding fields
happened to end, so finding it means walking the message. That is tens of nanoseconds against
hundreds, and it is why every latency-sensitive venue offers a binary feed even when it also
publishes FIX. Everything below targets the fixed-layout case; the one FIX-shaped problem
worth solving fast, ASCII integer parsing, gets its own section.

### Byte order

ITCH is big-endian; SBE and the German venues are little-endian; your CPU is little-endian.
C++23 finally gives you the swap directly.

```cpp endian.hpp
#pragma once
#include <bit>
#include <concepts>
#include <cstdint>

template <std::integral T>
[[nodiscard]] constexpr T from_big(T v) noexcept {
    if constexpr (std::endian::native == std::endian::little) return std::byteswap(v);
    else return v;
}

template <std::integral T>
[[nodiscard]] constexpr T from_little(T v) noexcept {
    if constexpr (std::endian::native == std::endian::big) return std::byteswap(v);
    else return v;
}
```

`std::byteswap` compiles to a single `bswap` at `-O2`, or fuses into a `movbe` load on cores
that have it. Cost is one cycle; do not build a byte-order abstraction layer more elaborate
than the above.

### Framing

Over **UDP multicast**, which is how every real-time feed is delivered, a datagram is
atomic: you receive a whole packet or nothing. Nasdaq wraps a MoldUDP64 header, a 10-byte
session identifier, an 8-byte sequence number of the first message and a 2-byte count,
around a run of length-prefixed messages. A message never spans two datagrams, so parsing is
a loop with no state carried between packets. What you *do* get is loss and reordering,
which is what the sequence number is for.

Over **TCP**, used for order entry and snapshot recovery, you have a byte stream and no
message boundaries at all. A `recv` can return half a header.

```cpp Stream reassembly: the only correct shape
#include <cstddef>
#include <cstdint>
#include <cstring>

class StreamFramer {
public:
    // Append n bytes, dispatch every complete message, keep the remainder.
    template <typename OnMessage>
    void feed(const std::byte* data, std::size_t n, OnMessage&& on_message) {
        std::memcpy(buf_ + used_, data, n);          // caller guarantees it fits
        used_ += n;
        std::size_t off = 0;
        for (;;) {
            if (used_ - off < 2) break;              // not even a length
            std::uint16_t len_be;
            std::memcpy(&len_be, buf_ + off, 2);
            const std::size_t len = from_big(len_be);
            if (used_ - off < 2 + len) break;        // body incomplete
            on_message(buf_ + off + 2, len);
            off += 2 + len;
        }
        if (off != 0) {                              // compact the tail
            used_ -= off;
            std::memmove(buf_, buf_ + off, used_);
        }
    }
private:
    static constexpr std::size_t kCap = 1 << 20;
    alignas(64) std::byte buf_[kCap];
    std::size_t used_ = 0;
};
```

Note the shape: never allocate, never copy a message body, and compact only when something
was consumed. The `memmove` of a partial tail is at most a few hundred bytes.

## Reading a struct out of a buffer

Here is the line everyone writes:

```cpp
const auto* h = reinterpret_cast<const MsgHeader*>(buf + off);   // undefined behaviour
```

It is undefined behaviour twice over. No object of type `MsgHeader` has ever been created at
that address, so dereferencing the pointer violates the object model. And `buf + off` need
not satisfy `alignof(MsgHeader)`, which is undefined regardless of what the hardware does.

x86-64 executes unaligned loads happily, at a typical penalty of zero cycles inside one cache
line and a few cycles when the access straddles two, so the code appears to work. What bites
you is the compiler, not the CPU: told the pointer is a `MsgHeader*`, it may assume the
alignment and emit an aligned SIMD load, or delete a later check on the strength of that
assumption. This is a real class of bug, not a lawyer's objection.

Three legal forms, in the order you should reach for them:

```cpp Reading fields legally
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>       // std::start_lifetime_as (C++23)

// 1. memcpy. Works at any offset and any alignment. At -O2 this is one `mov`.
inline std::uint32_t load_u32_be(const std::byte* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return from_big(v);
}

// 2. bit_cast. Compile-time size match, trivially copyable both sides, no
//    alignment requirement because it copies. Nice for whole small structs.
struct RawPrice { std::uint32_t ticks_be; };
inline RawPrice load_price(const std::byte* p) noexcept {
    std::byte tmp[sizeof(RawPrice)];
    std::memcpy(tmp, p, sizeof tmp);
    return std::bit_cast<RawPrice>(tmp);
}

// 3. start_lifetime_as. C++23. Creates the object in the existing bytes with
//    no copy at all -- but the storage must ALREADY be correctly aligned.
inline const MsgHeader* view_header(const std::byte* p) noexcept {
    return std::start_lifetime_as<const MsgHeader>(p);
}
```

`std::start_lifetime_as` is the construct that finally makes zero-copy overlay well-defined,
and it is the right answer when you control alignment. It does **not** waive the alignment
requirement. The clean way to get both is to give every wire struct alignment 1 by packing
it, after which any byte offset is legal. Check your library first: it landed in libstdc++ 14
and is still missing elsewhere, so feature-test `__cpp_lib_start_lifetime_as` and fall back
to `memcpy`.

```cpp Packed wire layout
#pragma pack(push, 1)
struct MsgHeader {
    std::uint16_t length_be;      // body length, not including these two bytes
    char          type;           // 'A' add, 'X' cancel, 'P' trade
};
static_assert(sizeof(MsgHeader) == 3);
static_assert(alignof(MsgHeader) == 1);
#pragma pack(pop)
```

What packing costs you: you may not bind a reference or take an ordinary pointer to a member
(GCC and Clang will tell you so under `-Waddress-of-packed-member`, and you should promote
that to an error); on strict-alignment targets the compiler falls back to byte-by-byte
loads; and you cannot use the struct as a SIMD operand. On x86-64 with fixed-layout market
data, packing costs essentially nothing and buys you the alignment guarantee. Use it for
wire types only, never for anything you keep in memory.

## A worked ITCH-like decoder

```cpp itch_decoder.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "endian.hpp"

#pragma pack(push, 1)
struct AddOrder {
    std::uint64_t timestamp_be;
    std::uint64_t order_ref_be;
    char          side;            // 'B' or 'S'
    std::uint32_t shares_be;
    char          symbol[8];
    std::uint32_t price_be;        // in 1/10000 of a currency unit
};
struct CancelOrder {
    std::uint64_t timestamp_be;
    std::uint64_t order_ref_be;
    std::uint32_t shares_be;       // shares removed, not remaining
};
struct Trade {
    std::uint64_t timestamp_be;
    std::uint64_t match_id_be;
    std::uint32_t shares_be;
    char          symbol[8];
    std::uint32_t price_be;
};
#pragma pack(pop)

static_assert(sizeof(AddOrder) == 33);
static_assert(sizeof(CancelOrder) == 20);
static_assert(sizeof(Trade) == 32);

// Decoded, host-order, cache-friendly. Prices are integer ticks (lesson 40).
struct AddEvent {
    std::uint64_t exchange_ns;
    std::uint64_t order_ref;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t symbol_id;
    bool          is_bid;
};

struct CancelEvent {
    std::uint64_t exchange_ns;
    std::uint64_t order_ref;
    std::uint32_t qty_removed;
};

struct TradeEvent {
    std::uint64_t exchange_ns;
    std::uint64_t match_id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t symbol_id;
};
```

```cpp itch_decoder.hpp, continued
template <typename Handler>
class ItchDecoder {
public:
    explicit ItchDecoder(Handler& h) noexcept : h_(h) {}

    // Parse one whole datagram in place. Returns messages decoded.
    std::size_t decode_packet(const std::byte* p, std::size_t n) noexcept {
        std::size_t off = 0, count = 0;
        while (off + sizeof(MsgHeader) <= n) {
            std::uint16_t len_be;
            std::memcpy(&len_be, p + off, 2);
            const std::size_t len = from_big(len_be);   // type byte + body
            if (len == 0 || off + 2 + len > n) [[unlikely]] { h_.on_truncated(); break; }
            dispatch(static_cast<char>(p[off + 2]), p + off + 3, len - 1);
            off += 2 + len;
            ++count;
        }
        return count;
    }

private:
    void dispatch(char type, const std::byte* body, std::size_t len) noexcept {
        switch (type) {
        case 'A':
            if (len < sizeof(AddOrder)) [[unlikely]] { h_.on_malformed('A'); return; }
            h_.on_add(decode_add(body));
            return;
        case 'X':
            if (len < sizeof(CancelOrder)) [[unlikely]] { h_.on_malformed('X'); return; }
            h_.on_cancel(decode_cancel(body));
            return;
        case 'P':
            if (len < sizeof(Trade)) [[unlikely]] { h_.on_malformed('P'); return; }
            h_.on_trade(decode_trade(body));
            return;
        default:
            h_.on_ignored(type);       // system events, MWCB, LULD: not on the hot path
            return;
        }
    }

    static AddEvent decode_add(const std::byte* b) noexcept {
        AddOrder raw;
        std::memcpy(&raw, b, sizeof raw);          // one 33-byte copy, no allocation
        return AddEvent{
            .exchange_ns = from_big(raw.timestamp_be),
            .order_ref   = from_big(raw.order_ref_be),
            .price_ticks = static_cast<std::int64_t>(from_big(raw.price_be)),
            .qty         = from_big(raw.shares_be),
            .symbol_id   = intern(raw.symbol),
            .is_bid      = (raw.side == 'B'),
        };
    }
    static CancelEvent decode_cancel(const std::byte* b) noexcept;
    static TradeEvent  decode_trade(const std::byte* b) noexcept;
    static std::uint32_t intern(const char (&symbol)[8]) noexcept;   // perfect hash

    Handler& h_;
};
```

Two things to notice. The 33-byte `memcpy` into a local looks like a copy but at `-O2`
becomes a handful of register loads, because every subsequent use is a field read; the struct
never reaches memory. And `AddEvent` is a different type from the wire struct, in host order,
with the symbol already interned. Doing that translation once, here, is what lets the order
book in lesson 37 be a pure integer program.

:::pitfall
`intern` is where naive decoders lose all the time they saved. An 8-character symbol looked
up in a `std::unordered_map<std::string, uint32_t>` costs a hash, an allocation-free but
pointer-chasing bucket walk, and a string compare: typically 30 to 80 nanoseconds, more than
the entire rest of the decode. Build a perfect hash over the day's symbol universe at
startup, or have the venue's instrument id in the message and index an array directly.
:::

## Dispatch, and validation that predicts

Three ways to get from a type byte to a handler.

**A `switch`.** GCC and Clang will build a jump table when the case values are dense enough,
which for scattered ASCII letters they often are not; you may get a chain of compares
instead. Read the assembly. If it is a chain, either order the cases by observed frequency,
or remap the type byte through a 256-entry `constexpr` array into a dense 0..N index and
switch on that. The dense form is one bounded load, one indirect jump, and the branch
predictor learns the message mix.

**A function-pointer table** indexed by the type byte. One load and an indirect call. It
predicts about as well as the switch's indirect jump, but it is opaque: nothing inlines and
the compiler cannot propagate constants into the handler. In microbenchmarks it typically
lands a few nanoseconds per message behind a dense switch. Its advantage is that the table is
data, so handlers can be swapped at runtime.

**Templates.** Make the handler a template parameter, as above, so every `h_.on_add(...)` is
a direct call the compiler can inline, and the whole decode-and-apply path sits in one
function to optimise. The cost is code size, and code size is instruction cache, which lesson
29 says is the thing you are actually short of. Do not push the message types into a
`std::variant` and visit it; that buys back the indirect dispatch you just removed.

The recommendation: dense `switch`, templated handler, cases ordered by frequency, and
`perf stat -e branch-misses` to confirm.

For validation the goal is not fewer checks but *predictable* ones. Every bounds check above
is `[[unlikely]]` and compares against a length already in a register, so in a healthy
session it is false every time and the predictor costs you nothing beyond the compare. Avoid
validation whose outcome varies with the data: a range check on the price mispredicts a few
percent of the time at a typical 15 to 20 cycles each. Compute those as arithmetic into a
flag instead of branching.

## Text protocols: SWAR integer parsing

When you do have to read ASCII, do not read it one byte at a time. Treat eight bytes as one
64-bit word and use ordinary arithmetic to do eight things at once. This is SWAR, SIMD
within a register.

```cpp swar.hpp
#pragma once
#include <cstdint>
#include <cstring>

// True if the eight bytes at p are all ASCII digits.
inline bool is_eight_digits(const char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return ((v & 0xF0F0F0F0F0F0F0F0ULL) |
            (((v + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) >> 4))
           == 0x3333333333333333ULL;
}

// Parse exactly eight ASCII digits, most significant first. Little-endian hosts.
inline std::uint32_t parse_eight_digits(const char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    v -= 0x3030303030303030ULL;                                   // subtract '0'
    const std::uint64_t tens  = ((v * (1 + (0xAULL << 8))) >> 8) & 0x00FF00FF00FF00FFULL;
    const std::uint64_t hunds = ((tens * (1 + (0x64ULL << 16))) >> 16) & 0x0000FFFF0000FFFFULL;
    return static_cast<std::uint32_t>((hunds * (1 + (10000ULL << 32))) >> 32);
}
```

Three multiplies and some masking replace eight iterations of `n = n * 10 + (c - '0')`, each
with a branch. Typical figures put the SWAR form at roughly 1 to 2 nanoseconds for eight
digits against 8 to 15 for the naive loop, and it is branchless, so it has no tail. The same
trick finds a delimiter: XOR the word against a broadcast of the separator and apply the
classic `haszero` bit-twiddle, or just call `memchr`, which is already vectorised.

## Sequence numbers, gaps and A/B feeds

Every exchange feed carries a monotonically increasing sequence number, and every exchange
publishes the same data twice on two multicast groups, the A feed and the B feed, over
physically separate infrastructure. Your handler joins both, takes whichever copy of a given
sequence number arrives first, and discards the second. Typical inter-feed skew is a few
microseconds, so in normal conditions this halves your exposure to a single dropped packet.

```cpp arbitrator.hpp
#pragma once
#include <cstdint>

class FeedArbitrator {
public:
    enum class Action { Process, Duplicate, Gap };

    Action classify(std::uint64_t seq) noexcept {
        if (seq == expected_) [[likely]] { ++expected_; return Action::Process; }
        if (seq < expected_) return Action::Duplicate;   // the other line got here first
        gap_lo_ = expected_;                             // seq > expected_: we lost some
        gap_hi_ = seq;
        expected_ = seq + 1;
        return Action::Gap;
    }

    std::uint64_t gap_lo() const noexcept { return gap_lo_; }
    std::uint64_t gap_hi() const noexcept { return gap_hi_; }

private:
    std::uint64_t expected_ = 1;
    std::uint64_t gap_lo_ = 0, gap_hi_ = 0;
};
```

`Gap` is the interesting branch and the one never exercised in testing. When it fires you
have three jobs at once: keep draining the socket so you do not compound the loss, buffer the
live stream from `seq` onward, and request the missing range from the venue's retransmit
service or rebuild from a snapshot. Whether the book is usable meanwhile is a business
decision belonging in the strategy; the decoder's job is to report the gap immediately and
accurately.

:::hft
A decoder that is correct at 200,000 messages per second and correct during a gap are two
different products. Opening auctions and macro releases produce bursts an order of magnitude
above the daily mean, and that is exactly when the recovery path runs for the first time.
Replay a captured pcap of a real busy open through your handler in CI, including one with
deliberately dropped packets, and assert both the book state and the gap reports. Every desk
that skipped this has a story about the day the book was silently wrong for four minutes.
:::

## The latency budget

For a fixed-layout binary message, decode should be a small fraction of tick-to-trade.
Typical per-message figures on a modern x86 core with everything L1-resident:

| Stage | Typical |
|---|---|
| Length and type read, bounds check | 1 to 3 ns |
| Dispatch (predicted) | 1 to 3 ns |
| Field copy and byte swaps | 3 to 8 ns |
| Symbol to integer id (perfect hash or direct index) | 1 to 5 ns |
| Book update, lesson 37 | 20 to 60 ns |
| **Decode total, excluding book update** | **10 to 25 ns** |

The target to hold yourself to is comfortably under 100 nanoseconds per message for decode.
A 1500-byte datagram carrying 40 ITCH messages should therefore be fully decoded in one to
two microseconds, which is the number to compare against your kernel-bypass receive path in
lesson 38. If you are above it, the usual culprits in order are: the symbol lookup, an
allocation hiding in a handler, a mispredicting `switch`, and a `std::function` somewhere in
the dispatch chain.

:::exercise
Implement the decoder above for the three message types and drive it with a generated buffer
of one million messages in a realistic mix, roughly 60% add, 30% cancel, 10% trade. Measure
nanoseconds per message. Then produce three variants: replace the `switch` with a
256-entry function-pointer table, replace the templated `Handler` with `std::function`
callbacks, and replace the symbol interning with `std::unordered_map<std::string, uint32_t>`.
Report the cost of each change separately, and check `perf stat -e branch-misses,L1-icache-load-misses`
for the table and template variants.
:::

## Takeaways

- Fixed-layout binary protocols put every field at a compile-time offset; text protocols do
  not, and that difference is worth an order of magnitude in decode time.
- `reinterpret_cast` onto a receive buffer is undefined behaviour even on x86. Use `memcpy`,
  `std::bit_cast`, or C++23 `std::start_lifetime_as` over storage you know is aligned.
- Pack your wire structs so their alignment is 1. Then any offset is legal, and the packing
  costs nothing on x86.
- UDP multicast gives you whole messages that can be lost; TCP gives you a byte stream that
  must be reassembled. Write the framing for the transport you actually have.
- Dense `switch` plus a templated handler beats a function-pointer table by a few nanoseconds
  and beats `std::function` by far more. Mark the validation branches `[[unlikely]]`.
- Sequence gaps and A/B arbitration are part of the decoder's contract. Test them with a
  replayed pcap, not with a unit test that never drops a packet.
