---
title: span, string_view and mdspan
part: Part II - Modern C++23
summary: A pointer and a length, given a type. How to write a decoder that copies nothing, why a static extent unrolls your loop, and how one layout choice on a price matrix costs you five cache lines.
time: 25 min
level: intermediate
tags: span, string_view, mdspan, views, lifetime, cache
---

Before C++17 the way to pass a buffer was a pointer and a separate `size_t`, and every
function had to trust that the caller kept them in sync. The view types package the pair,
give it a type, and add bounds-aware slicing. They own nothing, allocate nothing, and copy
nothing. That last property is the reason they belong in a feed handler, and the reason they
are dangerous: a view is a lifetime bug waiting for you to store it somewhere.

## A view is a pointer and a length

Every type in this lesson is the same idea specialised: a handle to memory somebody else
owns, plus enough shape information to index it safely.

```cpp
#include <cstddef>
#include <span>
#include <string_view>

static_assert(sizeof(std::span<int>)        == 2 * sizeof(void*));  // pointer + size
static_assert(sizeof(std::span<int, 8>)     == sizeof(void*));      // size is in the type
static_assert(sizeof(std::string_view)      == 2 * sizeof(void*));
```

Look at the second line. `std::span<int, 8>` has a **static extent**: the length is a template
argument, so the object stores only the pointer, and the size is a compile-time constant
everywhere it is used.

```cpp
#include <cstdint>
#include <span>

std::int64_t sum_dynamic(std::span<const std::int64_t> v) noexcept {
    std::int64_t s = 0;
    for (std::int64_t x : v) s += x;
    return s;
}

std::int64_t sum_ten(std::span<const std::int64_t, 10> v) noexcept {
    std::int64_t s = 0;
    for (std::int64_t x : v) s += x;
    return s;
}
```

`sum_dynamic` compiles to a loop with a trip-count test, a scalar prologue and a vectorised
body, because the compiler cannot know whether the length is 0, 3 or a million.
`sum_ten` compiles to straight-line code: at `-O2` on x86-64 you get a handful of SIMD adds
and no branch at all. Dump both with `-S -masm=intel` and compare; the difference is not
subtle.

:::key
Use a static extent whenever the size is fixed by a protocol: a 10-level book snapshot, an
8-byte sequence number, a 48-byte fixed message. You are handing the optimiser a fact it
cannot otherwise have, and paying 8 fewer bytes per view.
:::

## Slicing, and looking at raw bytes

A span slices without copying. The member functions come in two flavours: a runtime one
returning a dynamic-extent span, and a template one returning a static extent.

```cpp
#include <cstddef>
#include <span>

void split(std::span<const std::byte> pkt) noexcept {
    auto hdr  = pkt.first<8>();     // std::span<const std::byte, 8>, size known
    auto body = pkt.subspan(8);     // std::span<const std::byte>, dynamic
    auto tail = pkt.last(4);        // dynamic: the trailing checksum
    (void)hdr; (void)body; (void)tail;
}
```

`subspan(offset)` takes everything from `offset`; `subspan(offset, count)` takes `count`
elements; `subspan<Off, Cnt>()` does the same with compile-time values and keeps the static
extent. None of them check bounds: an out-of-range offset is undefined behaviour, exactly as
with pointer arithmetic. Check the length once at the top of your decode function and slice
freely below it.

`std::as_bytes` and `std::as_writable_bytes` reinterpret any span as a span of `std::byte`.
This is the standard-blessed way to look at an object's representation, and it is what a
transport layer should take.

```cpp
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

struct Order { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; std::uint32_t pad; };

std::span<const std::byte> wire_image(const std::array<Order, 4>& batch) noexcept {
    return std::as_bytes(std::span<const Order, 4>{batch});
}
```

## A decoder that copies nothing

Here is the API shape the rest of this series assumes. The decoder takes bytes it does not
own, returns a decoded value or an error, and never touches the allocator.

```cpp feed.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

enum class DecodeError : std::uint8_t { Truncated, BadType };

struct PacketHeader {
    std::uint16_t len;
    std::uint8_t  msg_type;
    std::uint8_t  flags;
    std::uint32_t seq;
};

struct DecodedPacket {
    PacketHeader header;
    std::span<const std::byte> body;      // borrows from the caller's buffer
};

inline std::expected<DecodedPacket, DecodeError>
decode(std::span<const std::byte> pkt) noexcept {
    if (pkt.size() < sizeof(PacketHeader))
        return std::unexpected(DecodeError::Truncated);

    PacketHeader h{};
    std::memcpy(&h, pkt.data(), sizeof h);     // see lesson 36 on object lifetime and casts

    if (pkt.size() < h.len)
        return std::unexpected(DecodeError::Truncated);

    return DecodedPacket{h, pkt.subspan(sizeof(PacketHeader), h.len - sizeof(PacketHeader))};
}
```

Because the parameter is `std::span<const std::byte>`, one function serves a stack array, a
`std::vector<std::byte>`, a slice of a ring buffer, and a region mapped from a kernel-bypass
NIC. None of them copies. Any contiguous range converts implicitly.

:::hft
This is the single most valuable habit in this lesson. A decoder that takes
`const std::vector<std::byte>&` forces the receive path to build a vector, which means an
allocation per packet or a copy into a reusable one. A decoder taking
`std::span<const std::byte>` reads directly out of the NIC's receive ring. On a
two-million-message-per-second feed, removing one 1,500-byte copy per packet is the
difference between comfortably keeping up and dropping under burst.
:::

## string_view, and the trap

`std::string_view` is `std::span<const char>` with string operations and one extra hazard:
the standard library is full of functions returning `std::string` by value, and binding a view
to one of those leaves you pointing at a destroyed buffer.

```cpp
#include <string>
#include <string_view>

std::string_view bad() {
    std::string s = "XNAS";
    return s;                         // dangles: s dies at the closing brace
}

void also_bad() {
    std::string_view sv = std::string("XNAS") + "-EQ";   // temporary dies at the semicolon
    (void)sv;                                            // sv is already invalid here
}
```

Both compile without a warning by default. Turn on `-Wdangling-reference` on GCC 13 and later
and `-Wreturn-stack-address` everywhere, and treat a `string_view` member as something you
justify in code review.

The other surprise: a `string_view` is **not null-terminated**. `sv.data()` points at the
first character, and there is no guarantee of a `\0` after the last one, so passing it to a
C API that expects `const char*` reads past the end.

```cpp
#include <cstdint>
#include <string_view>

// Fixed-width, space-padded symbol field straight off the wire.
constexpr std::string_view trim_symbol(std::string_view field) noexcept {
    while (!field.empty() && field.back() == ' ') field.remove_suffix(1);
    return field;
}

static_assert(trim_symbol("ESZ5    ") == "ESZ5");
```

`remove_prefix` and `remove_suffix` adjust the view in place, which is how you parse a
fixed-width record with no allocation at all.

## mdspan, and the layout that costs you cache lines

`std::mdspan` (C++23) is a span with more than one index. It separates three things: a data
handle, an **extents** object saying how big each dimension is, and a **layout** saying how
index tuples map to offsets. Storage stays a flat, contiguous array that you own.

Library support lags the standard here. If `<mdspan>` is missing, use the Kokkos reference
implementation, which provides the same interface in `namespace std::experimental`.

```cpp
#include <cstddef>
#include <cstdint>
#include <mdspan>
#include <vector>

constexpr std::size_t kSymbols = 4096;
constexpr std::size_t kDepth   = 10;

using BookView = std::mdspan<const std::int64_t,
                             std::extents<std::size_t, kSymbols, kDepth>>;

std::int64_t total_at_top(const std::vector<std::int64_t>& storage) noexcept {
    BookView book{storage.data()};
    std::int64_t s = 0;
    for (std::size_t sym = 0; sym < kSymbols; ++sym)
        s += book[sym, 0];                  // C++23 multidimensional subscript
    return s;
}
```

The default layout is `std::layout_right`, which is row-major: the **last** index varies
fastest, so all ten price levels of one symbol are adjacent in memory. `std::layout_left` is
column-major: the **first** index varies fastest, so all 4,096 symbols at level 0 are adjacent
and the ten levels of one symbol are 4,096 elements apart.

That choice is not stylistic. With 8-byte prices and 64-byte cache lines:

| Access pattern | `layout_right` | `layout_left` |
|---|---|---|
| Ten levels of one symbol | 80 bytes, 2 cache lines | 10 lines, 32 KB apart |
| Level 0 of every symbol | 4,096 lines, 8 bytes used each | 512 lines, fully used |

Pick the layout that matches your dominant loop. A book you walk depth-first per symbol wants
`layout_right`; a risk engine summing the top of book across the universe wants `layout_left`.
Getting it backwards costs you a factor of eight in bytes fetched, and on the second row a
hardware prefetcher that cannot help you.

:::perf
`std::layout_stride` lets you describe a matrix with padding between rows, which is how you
stop two hot rows from mapping to the same cache set. Pad a 4,096-by-8 matrix to a row stride
of 9 and the conflict misses disappear. Lesson 21 covers the aliasing arithmetic; `mdspan`
just gives you the vocabulary to express it.
:::

Slicing an `mdspan` is `submdspan`, which takes a slice specifier per dimension and returns a
view of the selected region. It was adopted for C++26 rather than C++23, so today you reach for
it via the reference implementation or write the arithmetic yourself. When it lands,
`submdspan(book, sym, std::full_extent)` is the ten-level slice for one symbol, and it keeps
the layout information rather than degrading to a raw pointer.

Finally, the accessor. The third template parameter of `mdspan`, defaulting to
`std::default_accessor<T>`, decides how a reference is produced from the data handle and a
computed offset. Replace it and every read through the view changes behaviour: a
`checked_accessor` that asserts in debug builds, an accessor issuing non-temporal loads for a
matrix you stream once and never revisit, or one returning `std::atomic_ref<T>` so a shared
matrix is read without a lock. This is the extension point that makes `mdspan` more than
syntax sugar over index arithmetic.

:::warn
Views must never outlive their owner. That makes them **parameter types and local variables**,
not members. If a class needs to keep the data, it should keep a container; if it genuinely
must keep a view, the owner's lifetime has to be a documented invariant of the class, checked
in review, because no compiler will check it for you.
:::

:::exercise
Allocate a flat `std::vector<std::int64_t>` of 4,096 by 10 and wrap it twice, once as
`layout_right` and once as `layout_left`. Write two loops: one summing all ten levels for each
symbol in turn, one summing level 0 across all symbols. Time all four combinations and record
nanoseconds per element. Then run each under `perf stat -e L1-dcache-load-misses` and check
that the miss counts explain the timings you measured.
:::

## Takeaways

- A view is a pointer plus a length. It owns nothing and copies nothing, which is exactly why
  it belongs in parameters and not in members.
- `std::span<T, N>` with a static extent stores only the pointer and hands the optimiser a
  compile-time trip count, so fixed-size loops unroll and lose their branch.
- Take `std::span<const std::byte>` in every decoder entry point and the same function works
  on a stack array, a vector, a ring buffer and a NIC receive region with no copies.
- `std::string_view` binds happily to a temporary `std::string` and dangles silently, and its
  `data()` is not null-terminated. Enable `-Wdangling-reference`.
- `std::mdspan` splits data, extents and layout. `layout_right` makes the last index
  contiguous, `layout_left` the first, and choosing wrongly can multiply the bytes you fetch
  by eight.
- The accessor parameter turns `mdspan` into an extension point: bounds checks, non-temporal
  loads, or atomic reads, all without changing the indexing code.
