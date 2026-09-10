---
title: Types, Bits and Representation
part: Part I - Foundations
summary: What the machine actually stores when you write int, double or char, and the conversion rules that quietly destroy correctness before you ever reach the hot path.
time: 30 min
level: beginner
tags: integers, floating-point, undefined-behaviour, bits
---

C++ lets you name a type and mostly forget what it is made of. That works until a price
comes off the wire in the wrong byte order, or a loop counter wraps below zero and runs
for four billion iterations, or a compiler deletes a bounds check you were relying on
because you gave it permission without knowing. Representation is not trivia here. It is
the layer where correctness and latency are decided together.

## The types you get, and the sizes you do not

C++ guarantees far less about its fundamental types than people assume. The standard fixes
a minimum range and an ordering, not a size:

| Type | Guaranteed at least | Typical on x86-64 Linux |
|---|---|---|
| `char` | 1 byte, and `sizeof(char) == 1` always | 1 byte, signed |
| `short` | 16 bits | 2 bytes |
| `int` | 16 bits | 4 bytes |
| `long` | 32 bits | 8 bytes |
| `long long` | 64 bits | 8 bytes |
| `bool` | — | 1 byte |

`long` is 8 bytes on Linux and macOS and 4 bytes on 64-bit Windows. That single fact has
corrupted more shared-memory layouts than any other in the language. So on anything that
crosses a boundary you write the width down:

```cpp types.hpp
#include <cstdint>

using Ticks = std::int64_t;    // price, in integral exchange ticks
using Qty   = std::uint32_t;   // shares or lots, never negative
using SeqNo = std::uint64_t;   // exchange sequence number
```

`<cstdint>` gives you `std::int8_t` through `std::int64_t` and their unsigned partners.
These are exact-width and, on any platform you will deploy to, they exist. There are also
`std::int_fast32_t` (at least 32 bits, whichever is quickest) and `std::int_least32_t`.
Ignore both: on a hot path you care about layout, and layout means exact width.

`sizeof` yields a size in bytes as a `std::size_t`. `alignof` yields the address multiple
the type must sit on. Both are compile-time constants, so you can assert on them:

```cpp
#include <cstdint>
#include <cstddef>

struct Quote {
    std::int64_t  price_ticks;   // offset 0
    std::uint32_t qty;           // offset 8
    std::uint16_t venue_id;      // offset 12
    // 2 bytes of tail padding
};

static_assert(sizeof(Quote) == 16);
static_assert(alignof(Quote) == 8);   // driven by the widest member
```

A struct is aligned to its most demanding member, and its size is rounded up to a multiple
of that alignment so arrays of it stay aligned. Reorder those three members and you can get
24 bytes instead of 16. Lesson 21 makes a discipline out of this; for now just know that
`sizeof` is a fact you can check rather than a fact you assume.

## Signed, unsigned, and the conversions that bite

Every arithmetic operation on a type narrower than `int` first promotes both operands to
`int`. This is the **integral promotion**, it is silent, and it is not always harmless:

```cpp
#include <cstdint>

std::uint32_t notional_bad(std::uint16_t px, std::uint16_t qty) {
    return px * qty;             // both promote to int, multiply is int
}
```

With `px == qty == 60000`, the product is 3,600,000,000, which exceeds the maximum `int`
of 2,147,483,647. That is signed overflow, which is undefined behaviour, in a function
that looks like it only deals in unsigned values. The fix is to widen deliberately:
`return static_cast<std::uint32_t>(px) * qty;`.

When one operand is signed and the other unsigned and both are at least as wide as `int`,
the signed one converts to unsigned. This produces the single most common bug in beginner
C++:

```cpp
#include <cstdint>
#include <vector>

bool valid_level(const std::vector<std::int64_t>& book, int depth) {
    return depth < book.size();          // WRONG
}
```

`book.size()` is a `std::size_t`, which is unsigned 64-bit. Pass `depth == -1` and it
converts to 18,446,744,073,709,551,615, so the comparison is true and you index off the
front of the book. GCC and Clang will tell you with `-Wsign-compare`, which is included in
`-Wall`. Turn warnings into errors and this class of bug stops existing. When you genuinely
need to compare across signedness, C++20 gives you the correct primitives:

```cpp
#include <utility>          // std::cmp_less
#include <vector>

bool valid_level(const std::vector<std::int64_t>& book, int depth) {
    return depth >= 0 && std::cmp_less(depth, book.size());
}
```

:::pitfall
The reverse-iteration loop that never ends:

```cpp
for (std::size_t i = levels.size() - 1; i >= 0; --i) { /* ... */ }
```

`i` is unsigned, so `i >= 0` is always true. When `i` is 0 the decrement wraps it to
`SIZE_MAX` and the loop reads far past the end. If the container is empty, `size() - 1`
is already `SIZE_MAX` on the first line. Iterate with `for (std::size_t i = levels.size();
i-- > 0;)` or use a reverse iterator.
:::

## Overflow: undefined versus wrapping

Unsigned overflow is defined: arithmetic is modulo 2^N, and wraparound is a guarantee you
may rely on. Signed overflow is **undefined behaviour**. The compiler is entitled to assume
it never happens, and it uses that assumption aggressively.

```cpp
bool will_grow(int x)          { return x + 1 > x; }
bool will_grow(unsigned int x) { return x + 1 > x; }
```

```asm g++ -O2, x86-64
will_grow(int):
        mov     eax, 1              ; assumed always true
        ret
will_grow(unsigned int):
        cmp     edi, -1             ; must actually check for wrap
        setne   al
        ret
```

That is not the compiler being clever at your expense; it is the same assumption that lets
it promote a 32-bit loop induction variable to a 64-bit register without emitting a wrap
check on every iteration, which is worth real cycles in a tight loop. The cost is that a
signed overflow anywhere in your program licenses arbitrary behaviour everywhere.

C++20 finally mandated two's complement for signed integers, so `-1` is guaranteed to be
all bits set and `std::int32_t` is guaranteed to span -2,147,483,648 to 2,147,483,647.
Note carefully what did **not** change: signed overflow is still undefined. Representation
was fixed; the arithmetic contract was not. Build your debug and test binaries with
`-fsanitize=undefined` and you will be told at run time the first time you cross the line.

## Floating point, and why a price is an integer

An IEEE-754 `double` is 64 bits: 1 sign, 11 exponent, 52 stored mantissa bits giving 53
bits of precision. It represents a number as a binary fraction scaled by a power of two.
One-tenth is not such a number, any more than one-third is a finite decimal.

```cpp
#include <cstdio>

int main() {
    double a = 0.1, b = 0.2;
    std::printf("%.17g\n", a + b);   // 0.30000000000000004
    std::printf("%d\n", a + b == 0.3);  // 0
}
```

Three separate problems follow from this, and only the first is widely known.

**Representation error.** A price of 100.10 is not 100.10. Accumulate a few thousand fills
and your position value disagrees with the exchange's by a fraction of a tick, which the
reconciliation process will find at 6pm.

**Non-associativity.** `(a + b) + c` is not `a + (b + c)` in floating point, so the
compiler is forbidden from reordering a summation loop. That forbids vectorising it, which
costs you a factor of four or eight on a reduction. `-ffast-math` lifts the restriction by
declaring you do not care about the exact result, which on a risk calculation is a
statement you should be very reluctant to sign.

**Latency.** Floating point division is not pipelined the way multiplication is.

| Operation | Latency (cycles) | Throughput (per cycle) |
|---|---|---|
| 64-bit integer `imul` | 3 | 1 |
| `addsd` / `mulsd` | 4 | 2 |
| `divsd` | 13 to 20, microarchitecture dependent | 1 per 4 to 6 cycles |

Those figures are for recent Intel and AMD desktop and server parts; check Agner Fog's
tables or `llvm-mca` for your exact target rather than trusting the range.

:::hft
Prices are `std::int64_t` counts of exchange ticks, always. Every venue quotes on a
discrete grid, so the tick is the natural unit and the conversion is exact. An `int64_t`
represents every integer up to 9.2 x 10^18; a `double` represents every integer only up to
2^53, about 9.0 x 10^15, and above that it starts skipping. Integer prices compare exactly,
add exactly, hash exactly, and multiply against a quantity in three cycles. They also
survive a memcpy into a shared-memory ring without a rounding step. The only place a
`double` belongs is in a model output that is about to be rounded back to a tick anyway.
:::

## Raw bytes: std::byte, endianness and type punning

`char` is three types wearing a coat: `char`, `signed char` and `unsigned char` are
distinct, and plain `char` has implementation-defined signedness (signed on x86 Linux,
unsigned on ARM Linux). `char` means "a character", not "a small number". For raw storage
use `std::byte`, which is a scoped enumeration over `unsigned char` that supports only
bitwise operations, so you cannot accidentally do arithmetic on it.

x86-64 is little-endian: the least significant byte sits at the lowest address. Most
exchange protocols descended from network byte order are big-endian, NASDAQ ITCH among
them, while CME's Simple Binary Encoding is little-endian. You must know which, and C++23
gives you the swap directly:

```cpp decode.hpp
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Reads a big-endian 64-bit field from an unaligned wire buffer.
std::int64_t load_be64(const std::byte* p) noexcept {
    std::uint64_t raw;
    std::memcpy(&raw, p, sizeof raw);
    if constexpr (std::endian::native == std::endian::little) {
        raw = std::byteswap(raw);          // one BSWAP instruction
    }
    return static_cast<std::int64_t>(raw);
}
```

The `memcpy` is not a copy. At `-O2` both GCC and Clang turn a fixed-size `memcpy` into a
single unaligned load, and `std::byteswap` into a single `bswap`. What the `memcpy` buys
you is legality: the **strict aliasing** rule says you may not read an object through a
pointer to an unrelated type, so `*reinterpret_cast<const std::uint64_t*>(p)` is undefined
behaviour even though it usually appears to work. `memcpy` and `std::bit_cast` are the two
sanctioned escape hatches.

```cpp
#include <bit>
#include <cstdint>

// Same bits, different type. Both operands must be trivially copyable
// and the same size. constexpr-capable, unlike memcpy.
std::uint64_t bits_of(double d) noexcept { return std::bit_cast<std::uint64_t>(d); }
```

:::warn
`reinterpret_cast` between unrelated pointer types compiles, produces no instructions, and
is undefined behaviour in almost every case people reach for it. It does not begin an
object's lifetime and it does not fix alignment. Reach for `std::bit_cast` when the sizes
match and `std::memcpy` when they do not.
:::

## Bit operations that are one instruction

`<bit>` exposes the CPU's bit-manipulation instructions portably, with a correct fallback
where the hardware lacks them. Each of these compiles to one instruction given a suitable
`-march`:

```cpp
#include <bit>
#include <cstdint>

static_assert(std::popcount(0b1011u) == 3);          // POPCNT
static_assert(std::countl_zero(std::uint32_t{1}) == 31);  // LZCNT
static_assert(std::bit_width(std::uint32_t{4096}) == 13); // 1 + floor(log2 x)
static_assert(std::has_single_bit(4096u));                // power of two?
static_assert(std::bit_ceil(4000u) == 4096u);
```

The trading use is immediate. A ring buffer sized to a power of two lets you replace an
index modulo, a 20-plus cycle division, with a single `and` against `capacity - 1`; you use
`std::bit_ceil` to round the requested capacity up and `std::has_single_bit` to assert the
invariant. A bitset of active price levels lets `std::countr_zero` find the best level in
one instruction rather than a loop.

:::exercise
Write a function `std::uint32_t mask_for(std::uint32_t requested)` that returns
`std::bit_ceil(requested) - 1`, with a `static_assert` that the requested capacity is at
least 2 and at most 2^20. Then compile it at `-O2 -march=x86-64-v3` and confirm the whole
body is `lzcnt`, a shift and a decrement, with no branch. Change `std::bit_ceil` to a
`while (n < requested) n *= 2;` loop and diff the assembly.
:::

## Takeaways

- Fundamental type sizes are not fixed by the standard. Use `<cstdint>` exact-width types
  for anything stored, shared or transmitted.
- Mixed signed and unsigned comparison converts the signed operand to unsigned. Compile
  with `-Wall -Werror` and use `std::cmp_less` when you must mix.
- Unsigned overflow wraps and is defined. Signed overflow is undefined and the optimiser
  acts on that assumption; test with `-fsanitize=undefined`.
- Prices are `std::int64_t` ticks. Doubles carry representation error, block vectorisation
  through non-associativity, and divide in 13 to 20 cycles against 3 for an integer multiply.
- Read wire data through `std::memcpy` or `std::bit_cast`, never `reinterpret_cast`, and
  swap byte order explicitly with `std::byteswap`.
- `<bit>` turns population counts, leading-zero counts and power-of-two rounding into
  single instructions.
