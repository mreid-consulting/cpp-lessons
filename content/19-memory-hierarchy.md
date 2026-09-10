---
title: The Memory Hierarchy
part: Part III - The Machine
summary: Registers to DRAM in cycles and nanoseconds, why the cache line is the unit of everything, and how to stop counting instructions and start counting cache lines.
time: 35 min
level: intermediate
tags: cache, cache-line, prefetch, tlb, locality
---

Everything up to here has been about the language. From here on it is about the machine,
and the machine has one dominant characteristic: the processor is roughly two orders of
magnitude faster than the memory it reads from, and every trick in a modern CPU exists to
hide that gap. Internalise the numbers below and most performance questions answer themselves.

## The numbers

A typical modern dual-socket x86-64 server core, Ice Lake or Sapphire Rapids generation at
around 3 GHz. These are load-to-use latencies for a dependent load, from a pointer-chasing
benchmark. They vary by part and by clock; measure your own.

| Level | Typical latency (cycles) | Typical latency | Typical capacity |
|---|---|---|---|
| Register | 0 (part of the instruction) | ~0.3 ns | 16 general purpose, 32 vector |
| L1 data cache | 4-5 | ~1.4 ns | 32-48 KB per core |
| L2 | 14-20 | ~5 ns | 1.25-2 MB per core |
| L3 (shared) | 40-70 | ~15-25 ns | 1.5-3 MB per core, tens of MB total |
| Local DRAM | 200-300 | ~75-100 ns | hundreds of GB |
| Remote DRAM (other socket) | 400-600 | ~130-200 ns | hundreds of GB |

Read the ratio, not the absolute values. **An L1 hit and a DRAM miss differ by about a
factor of 60.** In the ~90 nanoseconds a DRAM access takes, a 3 GHz core can retire on the
order of a thousand instructions. That is why a function with twice as many instructions
but half as many cache misses is usually the faster one.

:::key
The core does not stall on every miss. Out-of-order execution runs past a pending load, and
a modern core sustains a dozen or so L1 misses at once. It stalls only when something
*depends* on the missing value. So: **independent** misses overlap and cost roughly one
miss; **dependent** misses serialise and cost the full latency each.
:::

## The cache line is the unit of everything

Memory moves between levels in **cache lines**, 64 bytes on every x86-64 part you will meet
and on ARM server cores such as Graviton and Neoverse. Apple silicon uses 128. A line is
aligned to its own size, so the line containing address `a` starts at `a & ~63`.

Three consequences follow, and they are the whole lesson.

**Reading one byte costs the same as reading 64.** Load a single `std::uint8_t` flag from
DRAM and the memory controller still transfers the whole line. Whether those other 63 bytes
were waste or a free gift depends on whether you are about to use them.

**Spatial locality is free performance.** If the next access is in the same line it is an
L1 hit at ~1.4 ns instead of a ~90 ns miss. Data you use together should live together.

**Temporal locality is what caches are for.** A working set that fits in L2 behaves
completely differently from one that is 10% larger than L2.

```cpp
#include <cstdint>

struct Tick {                       // 16 bytes: four per cache line
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t seq;
};
static_assert(sizeof(Tick) == 16);
static_assert(64 % sizeof(Tick) == 0);   // no Tick ever straddles a line
```

That `static_assert` pair is a design constraint, not decoration. A 24-byte `Tick` would put
some objects across a line boundary, turning one load into two (lesson 21).

## Contiguous versus linked

A contiguous array is the fastest general-purpose data structure ever devised, for the
reason in the previous section. Consider summing the quantities of ten million ticks.

```cpp
#include <cstdint>
#include <span>

std::uint64_t total_qty(std::span<const Tick> ticks) noexcept {
    std::uint64_t sum = 0;
    for (const Tick& t : ticks) sum += t.qty;   // stride 16 bytes: 4 per line
    return sum;
}
```

Ten million `Tick` is 160 MB, far larger than any L3, so every byte comes from DRAM. But one
miss delivers four ticks, and while the core works on those four the **hardware prefetcher**
has already spotted the ascending stride and requested the next several lines. The misses
overlap. The loop runs at DRAM streaming bandwidth, around 10 to 15 GB/s for a single core
on a typical server, which is roughly 1.2 nanoseconds per tick.

Now the linked version:

```cpp
#include <cstdint>

struct TickNode {                   // 24 bytes, allocated one at a time
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t seq;
    TickNode*     next;
};

std::uint64_t total_qty(const TickNode* head) noexcept {
    std::uint64_t sum = 0;
    for (const TickNode* p = head; p != nullptr; p = p->next)
        sum += p->qty;              // the address of the next load lives in this line
    return sum;
}
```

Every iteration is a **dependent load**: the core cannot compute the address of node *n+1*
until node *n* has arrived. The misses cannot overlap, and no prefetcher can predict a
pointer value. Allocate the nodes in order and never touch them and the allocator's locality
saves you. Insert, erase and reinsert for an hour, as any real order book does, and they are
scattered across hundreds of megabytes.

| Layout | Typical time per element | Why |
|---|---|---|
| `std::vector<Tick>`, 10M elements | ~1.2 ns | 4 per line, prefetched, misses overlap |
| Linked list, 10M nodes, allocation order | ~4 ns | 2-3 per line, but no stride to predict |
| Linked list, 10M nodes, shuffled | ~90 ns | one dependent DRAM miss per element |

Typical figures for one core of a modern server-class x86 part, working set beyond L3. The
75x between the first and last rows is not a micro-optimisation. It is the difference
between a design that works and one that does not.

:::hft
This is why order books are arrays indexed by price rather than `std::map<Price, Level>`,
and why order pools are slot arrays rather than `new`-ed nodes. It is also why
`std::unordered_map` disappoints: the standard mandates bucket iteration semantics that
force a node-based design, so every colliding lookup is a dependent pointer chase. Lesson 37
builds the array-indexed book; lesson 20 builds the slot pool.
:::

## Sets, ways, and the power-of-two trap

A cache cannot check every line on every access, so it is **set-associative**. The address
splits into an offset within the line, a set index, and a tag. An address may live in only
one set, and within it in any of *W* ways. A 32 KB, 8-way L1 with 64-byte lines has
32768 / 64 / 8 = **64 sets**, indexed by bits 6 through 11 of the address.

So two addresses land in the same set whenever they agree in bits 6-11, that is, whenever
they are a multiple of 4096 bytes apart. The cache holds 512 lines, but if every access is
4096 bytes from the last you can hold **8**, because they all compete for one set. This is a
**conflict miss**: a miss in a cache that is nowhere near full.

```cpp
#include <cstdint>
#include <vector>

// 512 symbols x 512 levels of int64: row stride is exactly 4096 bytes.
constexpr std::size_t kSyms = 512, kLevels = 512;

std::int64_t column_sum_bad(const std::vector<std::int64_t>& grid, std::size_t lvl) {
    std::int64_t s = 0;
    for (std::size_t sym = 0; sym < kSyms; ++sym)
        s += grid[sym * kLevels + lvl];      // every access 4096 bytes apart
    return s;
}
```

All 512 accesses map to the same L1 set. With 8 ways the ninth evicts the first, and by the
time you come back nothing is resident. Break the power of two by padding the row:

```cpp
constexpr std::size_t kPitch = kLevels + 8;    // 4096 + 64 bytes: rows walk the sets

std::int64_t column_sum_good(const std::vector<std::int64_t>& grid, std::size_t lvl) {
    std::int64_t s = 0;
    for (std::size_t sym = 0; sym < kSyms; ++sym)
        s += grid[sym * kPitch + lvl];         // consecutive rows land in adjacent sets
    return s;
}
```

Eight extra `std::int64_t` per row, 0.2% more memory, and the column walk becomes ordinary
misses rather than a self-inflicted thrash. On a typical server this is worth 3 to 10 times
on a column-major traversal of a power-of-two-pitched array. Whenever a benchmark is
inexplicably slow and a dimension is a power of two, check this first.

## Prefetchers, stores, and the TLB

Keeping the **hardware prefetchers** fed is most of what "cache-friendly" means:

- **Next-line**, in L1 and L2: fetches line *n+1* when line *n* is touched.
- **Stride (IP-based)**: watches the addresses one particular load instruction issues,
  detects a constant stride, and runs ahead. This is what makes `for (const Tick& t : v)`
  fast at any element size, not just 64 bytes.
- **Streaming (L2 to LLC)**: detects longer ascending or descending runs inside a 4 KB
  region and pulls whole streams.

What defeats them, in order of how often you will meet it:

1. **Indirection.** `sum += levels[index[i]].qty` has an unpredictable second address. The
   prefetcher runs ahead on `index` but not on `levels`.
2. **Pointer chasing.** No stride exists to detect.
3. **Randomness.** Hash probes and shuffled access patterns.
4. **Page boundaries.** The L2 streamer will not cross a 4 KB page boundary, because the
   next page may be unmapped and its physical address is unrelated. A long scan therefore
   takes a compulsory miss every 64 lines. Huge pages remove this; lesson 30 covers them.

Stores have their own machinery. Writing to a line that is not cached triggers a
**write-allocate**: the line is read from memory first, then modified, because the cache
must hold all 64 bytes. So writing fresh data costs a *read* you did not want. The **store
buffer** meanwhile lets the core retire a store before the line arrives, which is why stores
feel free until the buffer fills.

**Non-temporal stores** (`_mm_stream_si64` and friends, in `<immintrin.h>`) bypass the cache
and write through a write-combining buffer, skipping the write-allocate read and not
evicting anything useful. They are right in one situation only: writing a large block you
will not read again soon, such as dumping a capture buffer. Elsewhere they are slower,
because the next read of that data is a full DRAM trip.

Finally, every access uses a **virtual** address that must be translated. The **TLB**
(translation lookaside buffer) caches translations: typically 64 entries in the L1 dTLB and
1500 to 2000 in the shared L2 TLB, which with 4 KB pages covers only about 8 MB. Miss it and
the hardware runs a **page walk** through four levels of page tables, each level itself a
memory access that may miss, costing on the order of 50 to 150 cycles. A 200 MB order book
on 4 KB pages TLB-misses constantly no matter how good its cache locality is. That is the
argument for huge pages, and lesson 30 makes it.

:::exercise
Allocate a 64 MB array of `std::int64_t` and time three traversals: sequential, strided by 8
elements (one per line), and strided by 512 elements (4096 bytes). Report nanoseconds per
element, then rerun under `perf stat -e cycles,L1-dcache-load-misses,LLC-load-misses,
dTLB-load-misses`. Predict all four counters for each case before you look, and account for
every prediction you got wrong.
:::

## Counting cache lines

Here is the mental model to leave with. You were taught, implicitly, to estimate cost by
counting instructions or big-O operations. Neither predicts latency on modern hardware.
Instead: **for each operation your system performs, count the distinct cache lines it
touches and note which of those touches depend on each other.**

Applied to a feed handler processing one trade message:

- The 16-byte inbound message: 1 line, already in L1 because the NIC just DMA-ed it.
- The book level for that price: 1 line for an array indexed by price, 3 to 4 dependent
  lines for a `std::map`.
- The order-id lookup: 1 line for an open-addressed table with the payload inline, 2
  dependent lines if the table stores pointers.
- The strategy state: 1 line if the hot fields were grouped (lesson 20), 3 if they are
  scattered across an 88-byte struct.

Four independent lines, mostly resident, is a few tens of nanoseconds. Twelve lines, half
dependent and half missing, is over a microsecond. Same algorithm, same instruction count,
same big-O. The rest of Part III is techniques for getting that count down: layout in
lesson 20, alignment in lesson 21, removing the heap in lesson 24.

## Takeaways

- On a typical modern server core an L1 hit is about 1.4 ns and a DRAM access about 90 ns. Independent misses overlap; dependent misses do not.
- The 64-byte cache line is the unit of all transfer, so reading one byte costs the same as reading sixty-four. Group what you use together.
- A contiguous array beats a shuffled linked list by roughly 75x on traversal, from prefetching and overlapping misses rather than instruction count.
- Power-of-two strides collide in one cache set. Padding a row pitch by one line often buys 3 to 10 times on a column traversal.
- Indirection, pointer chasing, randomness and 4 KB page boundaries all defeat the prefetchers. The TLB covers only about 8 MB with 4 KB pages.
- Estimate cost by counting cache lines touched per operation and marking the dependent ones, not by counting instructions.
