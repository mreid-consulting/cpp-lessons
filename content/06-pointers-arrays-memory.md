---
title: Pointers, Arrays and the Memory Model
part: Part I - Foundations
summary: Addresses, the three storage durations and what each costs, why an array forgets its own size the moment you pass it, and how to read a network buffer without invoking undefined behaviour.
time: 30 min
level: beginner
tags: pointers, arrays, lifetime, aliasing, cache
---

C++ hands you the address space directly. That is the reason the language is still in the
hot path of every exchange-connected system, and it is the reason the failure modes are
silent rather than exceptional. A pointer is a number with a type attached; whether that
number still refers to a live object is a question the language will never ask on your
behalf.

## Addresses and pointers

Memory is a flat array of bytes, and every byte has an address. A pointer holds one.

```cpp
#include <cstdint>
#include <print>

int main() {
    std::int64_t price_ticks = 10'025;
    std::int64_t* p = &price_ticks;      // & takes the address

    *p = 10'026;                          // * dereferences: read or write the pointee
    std::print("{} at {}\n", price_ticks, static_cast<const void*>(p));

    std::int64_t* q = nullptr;            // points at nothing. Never dereference.
    if (q != nullptr) { *q = 1; }
}
```

On x86-64 a pointer is 8 bytes regardless of what it points at, so `sizeof(void*)`,
`sizeof(std::int64_t*)` and `sizeof(Order*)` are all 8. Use `nullptr`, not `NULL` and not
`0`; `nullptr` has its own type and cannot be mistaken for an integer during overload
resolution.

Dereferencing a null pointer is undefined behaviour, not a guaranteed crash. On Linux it
usually faults because page zero is unmapped, but the compiler is also entitled to assume
you never do it and delete the branch that checks. That is not hypothetical: if you
dereference a pointer and then test it for null, the optimiser may remove the test, because
the dereference already proved it non-null.

## Where objects live, and for how long

Every object has a **storage duration** that decides when it is created and destroyed.

| Duration | Where | Created | Destroyed | Cost to allocate |
|---|---|---|---|---|
| Automatic | stack | at its declaration | at end of scope | one register add, effectively free |
| Dynamic | heap | at `new` / `malloc` | at `delete` / `free` | ~20 to 200 ns, unbounded tail |
| Static | data segment | before `main` (or on first use) | after `main` | none |
| Thread-local | per-thread block | on thread start / first use | on thread exit | none per access |

```cpp
#include <cstdint>
#include <vector>

std::int64_t g_last_trade_ticks = 0;          // static duration, whole program

void on_tick(std::int64_t px) {
    std::int64_t local = px;                  // automatic: this scope only
    static std::uint64_t call_count = 0;      // static duration, function scope
    ++call_count;

    std::vector<std::int64_t> scratch(1024);  // vector object is automatic,
                                              // its buffer is dynamic
    g_last_trade_ticks = local;
}                                             // scratch destroyed here, buffer freed
```

Stack allocation is a subtraction from the stack pointer performed once for the whole frame,
so a hundred locals cost the same as one. Heap allocation runs an allocator: glibc's
`malloc` is typically tens of nanoseconds for a small block from a thread cache, but it can
take a lock, touch a new page, or call `mmap`, and the tail of that distribution is what
kills a latency budget. Lesson 24 is about removing it from the hot path entirely; lesson 8
is about owning it safely when you cannot.

Stacks are small. The Linux main thread default is 8 MiB (`ulimit -s`), and threads created
by `std::thread` inherit a comparable default. A `std::int64_t prices[2'000'000];` local is
16 MB and overflows the stack, which manifests as a segmentation fault with no allocation
error anywhere. Large fixed-size buffers belong in static storage, in a pool, or in a
container that heap-allocates.

## Dangling: the failure mode that matters

An object's **lifetime** ends at a defined point. A pointer that outlives it is dangling,
and using it is undefined behaviour that frequently appears to work, because the memory is
still mapped and still holds the old bytes until something reuses it.

```cpp
#include <cstdint>
#include <vector>

std::int64_t* worst_bug() {
    std::int64_t px = 10'025;
    return &px;                 // px dies here. -Wreturn-local-addr catches this one.
}

void invalidation(std::vector<std::int64_t>& book) {
    std::int64_t* best = &book.front();
    book.push_back(9'999);      // may reallocate: the whole buffer moves
    *best = 1;                  // dangling. Nothing warns.
}
```

The second one is the shape you will actually meet. Any operation that can grow a
`std::vector` invalidates every pointer, reference and iterator into it. The same is true of
`std::unordered_map` on rehash. Cache a pointer across a mutation and you have a bug that
survives testing and fails under production message rates, because reallocation only happens
at particular sizes.

:::warn
Build your test binaries with `-fsanitize=address,undefined`. AddressSanitizer detects
use-after-free, use-after-return and heap overflow at roughly 2x slowdown and finds nearly
all of this class. Run it in CI on every commit. It is the single highest-yield tool
available to a C++ team and it costs one flag.
:::

## Pointers, references, and arrays that decay

A reference must bind to something at construction, can never be rebound, and has no null
state. A pointer can be null, can be reassigned, and supports arithmetic. Choose on that
basis: a reference when the thing always exists and you are aliasing it, a pointer when
absence is a legitimate state or when you need to walk over memory.

Pointer arithmetic is scaled by the pointee's size. `p + 1` advances by `sizeof(*p)` bytes,
not one byte:

```cpp
#include <cstdint>

std::int64_t sum_levels(const std::int64_t* first, const std::int64_t* last) noexcept {
    std::int64_t total = 0;
    for (const std::int64_t* p = first; p != last; ++p) {   // ++p adds 8 bytes
        total += *p;
    }
    return total;
}
```

Subtracting two pointers into the same array gives the element count, as a `std::ptrdiff_t`.
Arithmetic on pointers into *different* arrays is undefined even if you never dereference
the result.

A C array is a block of elements, and it forgets its own size the instant you pass it
anywhere. This is **array-to-pointer decay**:

```cpp
#include <cstdint>
#include <iterator>

void takes_array(std::int64_t prices[10]) {
    // The [10] is a comment. The parameter is std::int64_t*.
    static_assert(sizeof(prices) == sizeof(std::int64_t*));
}

void caller() {
    std::int64_t prices[10]{};
    static_assert(sizeof(prices) == 80);      // here it still knows
    static_assert(std::size(prices) == 10);   // C++17, works only before decay
    takes_array(prices);                      // size information lost at the call
}
```

The idiom `sizeof(a) / sizeof(a[0])` is correct in the scope where the array is declared and
silently wrong, giving 1 on x86-64 for an array of 8-byte elements, anywhere the array has
decayed. `std::array` keeps the size in the type and never decays:

```cpp
#include <array>
#include <cstdint>

std::int64_t total(const std::array<std::int64_t, 10>& prices) noexcept {
    std::int64_t t = 0;
    for (std::int64_t px : prices) { t += px; }   // size known, range-for works
    return t;
}
```

`std::array` is an aggregate with no indirection: `sizeof(std::array<std::int64_t, 10>)` is
80, exactly the C array, and it compiles to the same loop. The only cost is that the size is
baked into the type, so a function taking one works for that size alone. The general answer
is `std::span`, a pointer-and-length pair that accepts C arrays, `std::array` and
`std::vector` alike, and that is lesson 16.

## Two dimensions, row-major, and the cache line

A 2D array in C++ is an array of arrays, laid out **row-major**: the whole of row 0, then
the whole of row 1. `book[i][j]` is at offset `(i * cols + j) * sizeof(element)`.

```cpp
#include <cstddef>
#include <cstdint>

constexpr std::size_t kRows = 1024, kCols = 1024;
using Grid = std::int64_t[kRows][kCols];

std::int64_t sum_row_major(const Grid& g) noexcept {   // adjacent addresses
    std::int64_t t = 0;
    for (std::size_t i = 0; i < kRows; ++i)
        for (std::size_t j = 0; j < kCols; ++j)
            t += g[i][j];
    return t;
}

std::int64_t sum_column_major(const Grid& g) noexcept { // 8 KiB stride
    std::int64_t t = 0;
    for (std::size_t j = 0; j < kCols; ++j)
        for (std::size_t i = 0; i < kRows; ++i)
            t += g[i][j];
    return t;
}
```

Same arithmetic, same number of loads, and on a typical Xeon the second is commonly 5 to 20
times slower. Measure it on your own machine rather than trusting that range, because it
depends on cache size and page size, but the reason is fixed. The CPU does not fetch bytes;
it fetches 64-byte **cache lines**. Walking a row consumes all eight `int64_t` in each line
before moving on, and the hardware prefetcher recognises the constant stride and fetches
ahead. Walking a column touches one 8-byte element per 64-byte line, so seven eighths of
every fetch is discarded, the working set is eight times larger than it needs to be, and at
an 8 KiB stride each access also lands on a different 4 KiB page, exhausting the TLB.
Lesson 19 measures all of this properly and lesson 20 turns it into a design rule.

## Alignment, aliasing, and reading a network buffer

Every type has an alignment requirement: `alignof(std::int64_t)` is 8, so an `std::int64_t`
object must live at an address that is a multiple of 8. x86-64 tolerates misaligned scalar
loads at a small cost, larger when the access straddles two cache lines, but the *language*
does not: creating an object at a misaligned address is undefined behaviour, and on other
architectures it faults. You can over-align deliberately:

```cpp
#include <cstdint>
#include <new>

struct alignas(64) ProducerCursor {          // own cache line, no false sharing
    std::uint64_t seq = 0;
};
static_assert(alignof(ProducerCursor) == 64);
static_assert(sizeof(ProducerCursor) == 64);  // padded up to the alignment
```

Lesson 21 explains why the cursor gets a line to itself. `void*` is the type-erased pointer:
it can hold any object address, it carries no size or type, and you cannot dereference it or
do arithmetic on it in standard C++. It appears in C interfaces such as `memcpy`, in
`malloc`, and in callback registration where a `void* user_data` is threaded through. In new
code it is almost always the wrong tool.

Which brings us to the last rule and the one that trips up every feed handler. **Strict
aliasing** says you may access an object only through a glvalue of its own type, a
signed/unsigned variant of it, or a character type (`char`, `unsigned char`, `std::byte`).
So this, which every C programmer writes, is undefined behaviour:

```cpp
#include <cstddef>
#include <cstdint>

struct MdIncrement {
    std::uint64_t seq;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t flags;
};

// UB twice over: no MdIncrement object exists at buf, and buf may be misaligned.
const MdIncrement* wrong(const std::byte* buf) {
    return reinterpret_cast<const MdIncrement*>(buf);
}
```

The sanctioned form copies the bytes into a real object:

```cpp
#include <cstddef>
#include <cstdint>
#include <cstring>

MdIncrement decode(const std::byte* buf) noexcept {
    MdIncrement m;
    std::memcpy(&m, buf, sizeof m);      // legal, aligned, and free at -O2
    return m;
}
```

At `-O2` a fixed-size `memcpy` is not a function call. GCC and Clang lower a 24-byte copy to
three unaligned 8-byte moves, or a 16-byte SSE move plus an 8-byte move, which is exactly
what the illegal `reinterpret_cast` version would have generated had the pointer been
aligned. You pay nothing for correctness here. C++23 adds `std::start_lifetime_as` for the
in-place case, but library support is still thin; use `memcpy` today, and `std::bit_cast`
when the source and destination are the same size and both trivially copyable.

:::hft
This is why market data structs are declared with fixed-width types, explicit padding and a
`static_assert` on `sizeof`. The decoder then becomes a `memcpy` into a stack object plus a
byte swap on the fields that need it, and the whole thing inlines into a handful of loads.
The moment somebody adds a `std::string`, a `std::vector` or a virtual function to that
struct, the `memcpy` becomes undefined behaviour and the register-passing goes away, and
nothing in the build tells you. Assert triviality at the type, as lesson 4 shows, and the
build tells you.
:::

:::exercise
Write both `sum_row_major` and `sum_column_major` above into one file, time each over 100
iterations with `std::chrono::steady_clock`, and record the ratio. Then run
`perf stat -e cache-misses,LLC-load-misses,dTLB-load-misses ./a.out` on each in isolation.
Predict which counter separates them most before you look. Finally shrink the grid to 64 by
64, which fits in L1, and confirm the ratio collapses to roughly 1.
:::

## Takeaways

- A pointer is an address plus a type. Pointer arithmetic scales by `sizeof` the pointee,
  and only within a single array.
- Automatic storage is free, static storage is free, dynamic storage costs tens to hundreds
  of nanoseconds with an unbounded tail. Stacks are 8 MiB; big buffers go elsewhere.
- Growing a `std::vector` invalidates every pointer and reference into it. Build tests with
  `-fsanitize=address,undefined` and run them in CI.
- C arrays decay to pointers and lose their length. Use `std::array` when the size is fixed
  and `std::span` when it is not.
- Row-major layout means column-first traversal wastes seven eighths of every cache line and
  is commonly 5 to 20 times slower for the same arithmetic.
- Never `reinterpret_cast` a byte buffer to a struct. `std::memcpy` into a real object is
  legal, aligned, and compiles to the same instructions.
