---
title: optional, variant and expected
part: Part II - Modern C++23
summary: Three types that put "maybe", "one of these" and "or an error" into the type system, what each one costs in bytes and cycles, and when a hand-written union still wins.
time: 30 min
level: intermediate
tags: optional, variant, expected, error-handling, unions
---

A sentinel price of `-1`, an out-parameter `bool ok`, a global `errno`: every one of those is
a fact about the program that the type system does not know, and so cannot enforce. The
vocabulary types encode "there may be no value", "it is exactly one of these" and "a value or
a reason there is not" directly into the return type. None of them allocates. All of them
cost something, and the point of this lesson is to say exactly what.

## optional: a value that might not be there

`std::optional<T>` is a `T` and a `bool`, laid out adjacently. No allocation, no indirection.

```cpp
#include <cstdint>
#include <optional>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

static_assert(sizeof(Level) == 16);                       // 8 + 4 + 4 padding
static_assert(sizeof(std::optional<Level>) == 24);        // 16 + bool + 7 padding
static_assert(sizeof(std::optional<std::int64_t>) == 16); // 8 + bool + 7 padding
```

That padding is the price. An `optional<int64_t>` is twice the size of the `int64_t`, which
matters when you put a million of them in a vector, and not at all when one is returned in
registers. There is no "niche" optimisation in the standard: `std::optional<T*>` is 16 bytes
even though a null pointer could have encoded the empty state.

The natural use is a lookup that may find nothing.

```cpp book.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

class Book {
public:
    std::optional<Level> best_bid() const noexcept {
        if (n_bids_ == 0) return std::nullopt;
        return bids_[0];
    }

    std::optional<Level> level_at(std::size_t depth) const noexcept {
        if (depth >= n_bids_) return std::nullopt;
        return bids_[depth];
    }

private:
    std::array<Level, 32> bids_{};
    std::size_t n_bids_ = 0;
};
```

Three ways to get the value out, and they differ in exactly one respect:

- `*opt` and `opt->qty` do **not** check. Dereferencing an empty optional is undefined
  behaviour. This is the one you want on the hot path, after you have tested `if (opt)`.
- `opt.value()` checks and throws `std::bad_optional_access` if empty.
- `opt.value_or(fallback)` returns a copy of the value or of the fallback, and never throws.
  Note it evaluates `fallback` unconditionally, so do not put work in it.

C++23 also adds monadic operations, which chain lookups without a pyramid of `if`s:
`and_then` (returns another optional), `transform` (wraps the result back up), and `or_else`
(supplies a replacement optional).

```cpp
#include "book.hpp"

std::int64_t mid_or_last(const Book& b, std::int64_t last) noexcept {
    if (auto bid = b.best_bid()) return bid->price_ticks;   // no check inside the branch
    return last;
}

std::int64_t notional_at_top(const Book& b) noexcept {
    return b.best_bid()
            .transform([](const Level& l) noexcept {
                return l.price_ticks * static_cast<std::int64_t>(l.qty);
            })
            .value_or(0);
}
```

## variant: a checked tagged union

`std::variant<Ts...>` holds exactly one of its alternatives and remembers which. It is a
union plus an index, and the size is the largest alternative rounded up to satisfy alignment,
plus room for the index.

```cpp msgs.hpp
#pragma once
#include <cstdint>
#include <type_traits>
#include <variant>

struct AddOrder { std::uint64_t id; std::int64_t price_ticks;
                  std::uint32_t qty; std::uint32_t symbol_id; };   // 24 bytes
struct Cancel   { std::uint64_t id; };                             //  8 bytes
struct Trade    { std::uint64_t id; std::uint32_t qty; };          // 16 bytes

using Msg = std::variant<AddOrder, Cancel, Trade>;

static_assert(sizeof(AddOrder) == 24);
static_assert(sizeof(Msg) == 32);      // 24 payload + 1 index + 7 padding

// Nothrow-movable alternatives mean valueless_by_exception is unreachable.
static_assert(std::is_trivially_copyable_v<Msg>);
```

The index is one byte for any variant with fewer than 256 alternatives, and the padding to
the alternatives' alignment usually swallows it. So a variant is typically free relative to a
raw union: you get the tag for nothing.

**`valueless_by_exception`** is the one wart. If assigning a new alternative throws part way
through, the variant has destroyed the old value and not constructed the new one. It enters a
special empty state, `index()` returns `std::variant_npos`, and `std::visit` throws
`std::bad_variant_access`. It cannot happen if every alternative is nothrow-move-constructible,
which is automatic for the trivially copyable message structs above. Assert it, as the header
does on its last line, and forget it.

## Getting the value out, and what visit costs

`std::visit` calls a callable with whichever alternative is active. The idiomatic spelling
uses an aggregate that inherits several `operator()`s:

```cpp
#include "msgs.hpp"
#include <cstdint>

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

std::int64_t notional(const Msg& m) noexcept {
    return std::visit(overloaded{
        [](const AddOrder& a) noexcept {
            return a.price_ticks * static_cast<std::int64_t>(a.qty);
        },
        [](const Cancel&) noexcept { return std::int64_t{0}; },
        [](const Trade& t) noexcept { return static_cast<std::int64_t>(t.qty); },
    }, m);
}
```

That is clean, exhaustive by construction (omit an alternative and it does not compile), and
it has a cost. The classic implementation builds a static table of function pointers indexed
by `index()`, then does an indirect call through it. An indirect call cannot be inlined, so
the bodies of those three lambdas stay as separate functions, and the compiler cannot
propagate anything across the call. Implementations have improved: recent libstdc++ emits a
`switch` for small variants, and both libraries special-case a single variant argument. But
this varies by library and version, so look at the assembly before assuming.

`std::get_if` is the escape hatch. It takes a pointer, returns a pointer to the alternative or
`nullptr`, and never throws.

```cpp
#include "msgs.hpp"

bool is_aggressive_add(const Msg& m, std::int64_t touch) noexcept {
    if (const auto* a = std::get_if<AddOrder>(&m))
        return a->price_ticks >= touch;
    return false;
}
```

`std::get<T>(m)` also works and throws `std::bad_variant_access` on the wrong alternative.
Prefer `get_if` on any path where you care about cycles.

## expected: a value or a reason, in the return type

`std::expected<T, E>` (C++23) is the return type for an operation that can fail for a reason
you want to report. It holds either a `T` or an `E`, never both, with the same union layout as
`variant<T, E>`. No allocation, no unwinding, no global state.

```cpp decode.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

struct Trade { std::uint64_t id; std::uint32_t qty; };

enum class DecodeError : std::uint8_t { Truncated, BadChecksum, UnknownType };

std::expected<Trade, DecodeError> decode_trade(std::span<const std::byte> b) noexcept {
    if (b.size() < sizeof(Trade))
        return std::unexpected(DecodeError::Truncated);

    Trade t{};
    std::memcpy(&t, b.data(), sizeof t);       // see lesson 36 on why not reinterpret_cast
    if (t.qty == 0)
        return std::unexpected(DecodeError::BadChecksum);
    return t;
}

static_assert(sizeof(std::expected<Trade, DecodeError>) == 24);
```

The caller checks with `if (r)`, reads the value with `*r` or `r->qty`, and reads the error
with `r.error()`. `r.value()` throws `std::bad_expected_access<E>`; on the hot path, do not
use it.

The monadic interface is the reason `expected` reads better than an error code. `and_then`
chains another fallible step and short-circuits on error, `transform` maps a successful value,
`or_else` handles the error and may recover, `transform_error` converts the error type.

```cpp
#include "decode.hpp"

std::expected<std::uint64_t, DecodeError>
trade_id_if_large(std::span<const std::byte> b) noexcept {
    return decode_trade(b)
        .and_then([](const Trade& t) -> std::expected<Trade, DecodeError> {
            if (t.qty < 100) return std::unexpected(DecodeError::UnknownType);
            return t;
        })
        .transform([](const Trade& t) noexcept { return t.id; });
}
```

Each step is a predicted-not-taken branch when nothing goes wrong. The error path is the same
branch, taken. There is no unwinder, no allocation, and no work proportional to stack depth.

:::hft
A feed handler decoding two million messages a second sees malformed packets rarely but not
never: a gap, a truncated datagram after a retransmit, an unknown message type after an
exchange release. `expected` puts that on the same code path as success, so a bad packet
costs one mispredicted branch, roughly 15 to 20 cycles. An exception costs the unwinder,
which on Linux libstdc++ takes a process-wide lock in `_Unwind_RaiseException` and lands in
the microseconds. Under a burst of bad packets, that lock serialises every thread in the
process. This is the concrete reason trading systems compile with `-fno-exceptions`, covered
as a policy in lesson 18.
:::

| Channel | Cost when nothing goes wrong | Cost when it does | Clarity |
|---|---|---|---|
| Exceptions | zero: no branch, table-driven | microseconds; a global lock in libgcc | best; cannot be ignored |
| Error code return | one predictable branch per call | same branch, taken | poor; silently ignorable |
| `errno` / out-param | a store plus a branch | same | worst; invisible in the signature |
| `std::optional<T>` | one branch; `sizeof(T)` rounded up | same branch | good, but carries no reason |
| `std::expected<T,E>` | one branch; `max(T,E)` plus tag | same branch | good, and carries the reason |

Read the first row carefully. Exceptions really are free on the success path, which is why
they are right for a config parser or an order-entry gateway's start-up. They are wrong when
the failure is frequent, or when the tail latency of the failure is what you are being paid
to control.

## When a hand-written union still wins

Be honest about the decoder hot path. `std::visit` on a `variant` re-derives which alternative
is active from the variant's own index, but the wire already told you: there is a message-type
byte in the header, you have already read it, and you are about to branch on it anyway.
Storing it a second time inside a variant and then dispatching on that copy is one indirect
call you did not need.

```cpp The shape a decoder actually wants
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

struct AddOrder { std::uint64_t id; std::int64_t price_ticks;
                  std::uint32_t qty; std::uint32_t symbol_id; };
struct Cancel   { std::uint64_t id; };
struct Trade    { std::uint64_t id; std::uint32_t qty; };

template <class Handler>
void dispatch(char msg_type, std::span<const std::byte> body, Handler& h) noexcept {
    switch (msg_type) {
        case 'A': { AddOrder a{}; std::memcpy(&a, body.data(), sizeof a); h.on_add(a);    break; }
        case 'X': { Cancel   c{}; std::memcpy(&c, body.data(), sizeof c); h.on_cancel(c); break; }
        case 'E': { Trade    t{}; std::memcpy(&t, body.data(), sizeof t); h.on_trade(t);  break; }
        default:  break;
    }
}
```

The compiler turns that into one bounds-checked jump table, and because `h.on_add` is a
concrete type's member function it inlines into the arm. There is no function-pointer table
and no second dispatch. On a decoder running at two million messages a second, that is worth
measuring; on a control-plane path handling one message a second, `variant` and `visit` are
clearer and the difference is noise.

The rule is not "avoid `variant`". It is that a variant is the right tool when the tag is
genuinely part of the value's identity and travels with it, and the wrong tool when the tag
already exists somewhere else and you are copying it.

:::exercise
Build a `std::variant<AddOrder, Cancel, Trade>` version of `dispatch` using `std::visit`, and
the `switch` version above. Feed both ten million messages with a randomly chosen type and
report nanoseconds per message. Then compile both at `-O2 -S` and count call instructions in
each. Finally, repeat with the message types in a fixed repeating pattern instead of random
order, and explain the change in the gap.
:::

## Takeaways

- `std::optional<T>` is `T` plus a `bool` plus padding, with no allocation and no niche
  optimisation. `operator*` does not check; `value()` throws.
- `std::variant` is a union plus a one-byte index, and the index usually costs nothing after
  alignment padding. Make every alternative nothrow-movable and `valueless_by_exception`
  becomes unreachable.
- `std::visit` is exhaustive and readable but classically dispatches through a function-pointer
  table, which blocks inlining. Use `std::get_if` where cycles matter, and read the assembly.
- `std::expected<T, E>` returns a value or a reason with one branch and no unwinding, and its
  `and_then` / `transform` / `or_else` chain keeps the error path off the page.
- Exceptions cost nothing on the success path and microseconds plus a global lock on the
  failure path. That trade is right for start-up and wrong for a feed handler.
- A `switch` on a message-type byte you have already read beats `std::visit` on a decoder,
  because the variant's tag duplicates information the wire format already gave you.
