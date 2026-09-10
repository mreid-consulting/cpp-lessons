---
title: Data-Oriented Design: AoS vs SoA
part: Part III - The Machine
summary: Laying out an order pool so the hot loop touches one cache line per order - member ordering, hot/cold splitting, 32-bit handles and slot arrays, with the byte arithmetic at every step.
time: 40 min
level: advanced
tags: soa, layout, padding, handles, slot-array
---

Lesson 19 ended with an instruction: count cache lines touched per operation. This lesson
is what you do about the answer. The technique has a name, **data-oriented design**, and
one idea behind it: design the data layout for the access pattern of the hot loop, then
write the code that falls out of it. Object-oriented design asks what an order *is*.
Data-oriented design asks what the reprice sweep *reads*, and puts exactly that
contiguously in memory.

## Two layouts for the same order pool

Here is a resting-order record as it usually gets written, one struct per order, stored in
an array. This is **array of structs**, AoS.

```cpp aos.hpp
#include <cstdint>
#include <vector>

struct Order {                      // AoS: everything about one order, together
    std::int64_t  price_ticks;
    std::uint64_t order_id;
    std::uint32_t qty;
    std::uint32_t filled_qty;
    std::uint8_t  venue;
    bool          is_buy;
};
static_assert(sizeof(Order) == 32);

std::vector<Order> orders;          // orders[i] is one whole order
```

Here is the same information as **struct of arrays**, SoA: one array per field, index `i`
meaning the same order in every array.

```cpp soa.hpp
#include <cstdint>
#include <vector>

struct OrderPoolSoA {               // SoA: one field, all orders, contiguous
    std::vector<std::int64_t>  price_ticks;
    std::vector<std::uint64_t> order_id;
    std::vector<std::uint32_t> qty;
    std::vector<std::uint32_t> filled_qty;
    std::vector<std::uint8_t>  venue;
    std::vector<std::uint8_t>  is_buy;      // vector<bool> is a bitset; never use it here
};
```

Neither is correct in general. Which one wins is decided by a calculation, not a taste.

## Bytes touched per operation

Take a real operation: a sweep over one million resting orders that recomputes remaining
quantity, reading `qty` and `filled_qty` and nothing else.

```cpp
#include <cstdint>
#include <span>
#include <vector>

std::uint64_t open_qty_aos(std::span<const Order> v) noexcept {
    std::uint64_t s = 0;
    for (const Order& o : v) s += o.qty - o.filled_qty;
    return s;
}

std::uint64_t open_qty_soa(std::span<const std::uint32_t> qty,
                           std::span<const std::uint32_t> filled) noexcept {
    std::uint64_t s = 0;
    for (std::size_t i = 0; i < qty.size(); ++i) s += qty[i] - filled[i];
    return s;
}
```

The arithmetic, for one million orders:

| Layout | Useful bytes | Bytes transferred | Cache lines | Time at ~12 GB/s |
|---|---|---|---|---|
| AoS, 32-byte `Order` | 8 MB | 32 MB | 500 000 | ~2.7 ms |
| AoS, 88-byte `Order` (naive, below) | 8 MB | 88 MB | 1 375 000 | ~7.3 ms |
| SoA, two `uint32` arrays | 8 MB | 8 MB | 125 000 | ~0.7 ms |

The useful column never changes: eight bytes per order is what the computation needs. What
changes is how much the machine has to move to deliver it. SoA transfers exactly the useful
bytes; the 88-byte AoS transfers eleven times that. Those times are estimates from single-core
DRAM streaming bandwidth on a typical modern server, with the working set well beyond L3;
your machine will differ, but the ratios will not.

Now the opposite operation. Cancel one order, chosen at random by handle, touching every
field to write an audit record:

| Layout | Cache lines touched |
|---|---|
| AoS, 32-byte `Order` | 1 |
| SoA, six arrays | 6 (six independent random misses) |

:::key
The rule is not "SoA is faster". It is: **contiguity should follow the access pattern.**
Loops that sweep many objects and read few fields want SoA. Operations that touch one
object and read all of its fields want AoS. A system that does both, which every trading
system does, splits the difference deliberately rather than by accident.
:::

## Padding, ordering, and a sizeof audit

Before choosing a layout, find out what the current one actually costs. Members are laid
out in declaration order, and the compiler inserts padding so each member sits at an offset
that is a multiple of its alignment. Here is a realistic `Order` as it grows organically
over two years of feature requests:

```cpp
#include <cstdint>

struct OrderNaive {
    bool          is_buy;           // offset  0, size  1   + 7 padding
    std::int64_t  price_ticks;      // offset  8, size  8
    std::uint32_t qty;              // offset 16, size  4   + 4 padding
    std::uint64_t order_id;         // offset 24, size  8
    std::uint8_t  venue;            // offset 32, size  1   + 7 padding
    std::int64_t  created_ns;       // offset 40, size  8
    std::uint32_t filled_qty;       // offset 48, size  4   + 4 padding
    std::uint64_t client_id;        // offset 56, size  8
    char          tag[16];          // offset 64, size 16
    std::int64_t  last_update_ns;   // offset 80, size  8
};
static_assert(sizeof(OrderNaive) == 88);
static_assert(alignof(OrderNaive) == 8);
```

Sixty-six bytes of data in eighty-eight bytes of struct. **Twenty-two bytes, 25% of the
memory and 25% of the bandwidth, is padding you are paying to move around.** Reordering by
descending alignment fixes it with no semantic change whatever:

```cpp
#include <cstdint>

struct OrderOrdered {
    std::int64_t  price_ticks;      // offset  0
    std::uint64_t order_id;         // offset  8
    std::uint64_t client_id;        // offset 16
    std::int64_t  created_ns;       // offset 24
    std::int64_t  last_update_ns;   // offset 32
    char          tag[16];          // offset 40
    std::uint32_t qty;              // offset 56
    std::uint32_t filled_qty;       // offset 60
    std::uint8_t  venue;            // offset 64
    bool          is_buy;           // offset 65   + 6 trailing padding
};
static_assert(sizeof(OrderOrdered) == 72);
```

Eighty-eight to seventy-two, for free, by sorting the declarations. Lesson 21 gives the
by-hand rule for computing this and covers `alignas` for the cases where you want to go the
other way.

:::pitfall
Do not reach for `#pragma pack(1)` to remove the last six bytes. Packing produces members
that are not naturally aligned, and an unaligned load of an `std::int64_t` that straddles a
cache line costs several extra cycles, or on some ARM parts a trap. Packing is for wire
formats you memcpy out of, never for structures you compute on.
:::

## Splitting hot from cold

Seventy-two bytes is still more than one cache line's worth of what the matching engine
reads. Ask which fields the hot loop actually touches: price, quantity, filled quantity,
venue, side, and the id. The client id, the timestamps and the free-text tag are read by
the audit and reporting path, which runs off the hot thread and does not care about
nanoseconds.

So split them, keyed by the same index:

```cpp hot_cold.hpp
#include <cstdint>
#include <vector>

struct OrderHot {                   // exactly 32 bytes: two orders per cache line
    std::int64_t  price_ticks;      // offset  0
    std::uint64_t order_id;         // offset  8
    std::uint32_t qty;              // offset 16
    std::uint32_t filled_qty;       // offset 20
    std::uint8_t  venue;            // offset 24
    bool          is_buy;           // offset 25   + 6 padding
};
static_assert(sizeof(OrderHot) == 32);
static_assert(64 % sizeof(OrderHot) == 0);   // no OrderHot straddles a cache line

struct OrderCold {                  // 40 bytes, touched by audit only
    std::uint64_t client_id;
    std::int64_t  created_ns;
    std::int64_t  last_update_ns;
    char          tag[16];
};
static_assert(sizeof(OrderCold) == 40);

std::vector<OrderHot>  hot;         // hot[i] and cold[i] are the same order
std::vector<OrderCold> cold;
```

The working set the hot thread streams has gone from 88 MB to 32 MB for a million orders,
and a random single-order access is now guaranteed to touch exactly one cache line rather
than the two an 88-byte struct straddles most of the time.

:::hft
Hot/cold splitting is the highest-value layout change on most trading codebases, because
the cold fields are usually the ones nobody wanted to delete. Compliance needs the client
id. Support needs the tag. Post-trade needs the timestamps. None of them need those bytes
in L1 during a market open burst. Keep the parallel arrays and the compliance team keeps
its fields, at zero cost to the tick-to-trade path.
:::

## Handles, slots and free lists

Once records live in an array, refer to them by **index**, not by pointer.

```cpp
#include <cstdint>
using OrderHandle = std::uint32_t;
inline constexpr OrderHandle kNullOrder = 0xFFFF'FFFFu;
```

Four advantages, all of them concrete:

- **Half the memory.** A million handles is 4 MB; a million pointers is 8 MB. In an index
  structure that is 4 MB fewer bytes to stream and half as many cache lines.
- **Stable across reallocation.** Grow the `std::vector` and every pointer into it dangles.
  Every index stays correct.
- **Trivially validatable.** `h < capacity` is one comparison. A corrupt pointer is a
  segfault at best and silent corruption at worst.
- **Serialisable.** An index means the same thing in a snapshot file, in a shared memory
  segment mapped at a different address, and in a log record. A pointer means nothing
  outside the process that produced it.

The container is a **slot array**: a fixed-capacity array of records plus a free list of
unused slots. The free list is **intrusive**, meaning it stores its links inside the slots
themselves rather than in a side structure, so it costs no extra memory at all.

```cpp order_pool.hpp
#include <cassert>
#include <cstdint>
#include <vector>

class OrderPool {
    std::vector<OrderHot>  hot_;
    std::vector<OrderCold> cold_;
    OrderHandle            free_head_ = kNullOrder;

public:
    explicit OrderPool(std::uint32_t capacity)
        : hot_(capacity), cold_(capacity) {
        assert(capacity > 0 && capacity < kNullOrder);
        // Thread every slot onto the free list, reusing order_id as the link.
        for (std::uint32_t i = 0; i + 1 < capacity; ++i)
            hot_[i].order_id = i + 1;
        hot_[capacity - 1].order_id = kNullOrder;
        free_head_ = 0;
    }

    // No allocation, no syscall, no branch beyond the exhaustion check.
    [[nodiscard]] OrderHandle acquire() noexcept {
        const OrderHandle h = free_head_;
        if (h == kNullOrder) [[unlikely]] return kNullOrder;
        free_head_ = static_cast<OrderHandle>(hot_[h].order_id);
        return h;
    }

    void release(OrderHandle h) noexcept {
        assert(h < hot_.size());
        hot_[h].order_id = free_head_;       // the slot itself holds the next link
        free_head_ = h;
    }

    [[nodiscard]] OrderHot&  hot(OrderHandle h)  noexcept { assert(h < hot_.size());  return hot_[h]; }
    [[nodiscard]] OrderCold& cold(OrderHandle h) noexcept { assert(h < cold_.size()); return cold_[h]; }
};
```

`acquire` and `release` are a handful of instructions each and touch one cache line. Compare
with `new Order` / `delete`, which on a typical glibc build is 20 to 100 nanoseconds, takes
a lock in a multithreaded process, and scatters your records across the heap so that the
traversal figures from lesson 19 collapse to the shuffled-list row. Lesson 24 makes
allocation-free programming a discipline in its own right.

The free-list walk is itself a pointer chase, which is a real cost if you free thousands of
slots and then allocate thousands. If that matters, keep a free **stack** as a separate
`std::vector<OrderHandle>` and pop from the back: contiguous, prefetchable, at the price of
four bytes per slot.

## A worked redesign

Put the whole sequence together for a pool of one million resting orders, and watch the
working set that the hot thread has to stream.

**Step 0, the original.** `std::unordered_map<std::uint64_t, Order*>` from order id to a
heap-allocated 88-byte `Order`. Working set: 88 MB of orders, scattered; plus 1 million
node allocations of roughly 32 bytes each in the map, also scattered. A lookup is a hash, a
bucket load, a dependent node load, and a dependent `Order` load: three to four dependent
misses, on the order of 250 to 350 nanoseconds when cold.

**Step 1, reorder the members.** 88 to 72 bytes. Working set 72 MB. No code changes, no
semantic change. Verified by a `static_assert` on `sizeof`.

**Step 2, split hot from cold.** 32 MB hot, 40 MB cold. The hot thread now streams 32 MB
instead of 72 MB, and a single-order access touches one line instead of two.

**Step 3, slot array plus 32-bit handles.** The map's value type becomes `OrderHandle`, not
`Order*`: the index structure shrinks by 4 MB and, more importantly, resolving a handle is
`hot_[h]`, one *independent* load into a contiguous array, rather than a dependent pointer
chase. The lookup drops from three or four dependent misses to two.

**Step 4, SoA for the sweep.** The end-of-interval sweep that recomputes open quantity now
reads two `uint32` arrays, 8 MB total, at streaming bandwidth: about 0.7 ms against roughly
7.3 ms for the step-0 layout, a ratio of about ten.

Step 4 also unlocks vectorisation, because a loop over separate arrays of scalars is the
shape the compiler's auto-vectoriser recognises. Interleaved AoS fields are not: the
compiler would have to gather every 32nd byte, and it will decline.

```cpp
#include <cstdint>
#include <cstddef>

// SoA transform. At -O2 -march=x86-64-v3 GCC and Clang emit an AVX2 loop
// processing eight orders per iteration. Check with -fopt-info-vec.
void open_quantities(const std::uint32_t* __restrict qty,
                     const std::uint32_t* __restrict filled,
                     std::uint32_t* __restrict out,
                     std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        out[i] = qty[i] - filled[i];
}
```

`__restrict` is the promise that these three arrays do not overlap. Without it the compiler
must assume a store to `out[i]` might alias `qty[i+1]`, which forces it to keep the loop
scalar. It is a compiler extension, universally supported, and lesson 25 covers when it is
and is not needed.

:::exercise
Take the `OrderNaive` struct above and write a program that prints `offsetof` for every
member and `sizeof` for the struct, then does the same for a version you have reordered by
hand. Confirm 88 and 72. Then build both a 32-byte `OrderHot` sweep and a 72-byte
`OrderOrdered` sweep over ten million records, time them, and check the measured ratio
against the bandwidth prediction of 72/32 = 2.25. Report where your measurement disagrees
with the model and why.
:::

## Takeaways

- Choose layout from the access pattern. Sweeps over many objects reading few fields want SoA; random access touching a whole object wants AoS.
- Compute bytes transferred, not bytes used. A 32-byte struct read for 8 useful bytes moves four times the data it needs.
- Ordering members by descending alignment removed 22 bytes of padding from a realistic 88-byte `Order` with no other change. `static_assert` on `sizeof` so it stays fixed.
- Hot/cold splitting keeps the fields the hot loop reads inside one cache line and exiles the audit fields to a parallel array.
- Prefer 32-bit handles into a slot array over 64-bit pointers: half the memory, stable across reallocation, cheap to validate, and meaningful outside the process.
- SoA is also what makes a transform loop auto-vectorisable. Interleaved AoS fields are not a shape the vectoriser will accept.
