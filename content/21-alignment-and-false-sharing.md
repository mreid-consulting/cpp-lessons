---
title: Alignment, Padding and False Sharing
part: Part III - The Machine
summary: How to compute a struct's size by hand, why an object straddling a cache line costs extra, and the two-line fix for the multithreaded slowdown that no profiler points at directly.
time: 30 min
level: advanced
tags: alignment, padding, false-sharing, mesi, interference-size
---

Lesson 20 treated padding as waste to be squeezed out. That is half the story. Alignment is
also a correctness property, and adding padding on purpose is sometimes the highest-leverage
change in a concurrent system. The same 64-byte cache line that made contiguity valuable
becomes, the moment two threads write into it, the most expensive object in your program.

## Natural alignment and how padding appears

Every type has an **alignment requirement**: a power of two that the address of any object
of that type must be a multiple of. `alignof(T)` reports it. On x86-64 with the System V ABI
scalars have **natural alignment**, equal to their size: `char` is 1, `std::uint16_t` is 2,
`std::uint32_t` is 4, and `std::int64_t`, `double` and every pointer are 8. A struct's
alignment is the maximum of its members'.

The compiler lays members out in declaration order and inserts **padding** wherever the next
member's offset would not otherwise be a multiple of its alignment. It also pads the end so
`sizeof` is a multiple of `alignof`, which is what makes `array[i]` land at `i * sizeof(T)`
with every element correctly aligned.

The by-hand rule is a three-line loop you can run in your head:

1. Start at offset 0. For each member in declaration order, round the current offset **up**
   to a multiple of that member's `alignof`, place it there, then advance by its `sizeof`.
2. The struct's `alignof` is the largest member `alignof`.
3. Round the final offset up to a multiple of the struct's `alignof`. That is `sizeof`.

Applied to a tick record written in the obvious order:

```cpp
#include <cstddef>
#include <cstdint>

struct TickNaive {
    std::uint8_t  venue;        // align 1 -> offset  0, size 1;   pad 7
    std::int64_t  price_ticks;  // align 8 -> offset  8, size 8
    std::uint16_t flags;        // align 2 -> offset 16, size 2;   pad 2
    std::uint32_t qty;          // align 4 -> offset 20, size 4
    std::uint64_t recv_ns;      // align 8 -> offset 24, size 8
};                              // end 32, alignof 8 -> sizeof 32

static_assert(alignof(TickNaive) == 8);
static_assert(sizeof(TickNaive) == 32);
static_assert(offsetof(TickNaive, price_ticks) == 8);
static_assert(offsetof(TickNaive, recv_ns) == 24);
```

Nine bytes of padding out of thirty-two. Sorting by descending alignment puts `price_ticks`
and `recv_ns` first, then `qty`, `flags`, `venue`, for 23 bytes of data in a 24-byte struct.
The `static_assert` lines are not paranoia; they are how you find out at compile time that
someone added a `bool` to a struct you had tuned.

:::pitfall
Alignment is a property of the *type*, and the compiler assumes it holds. Produce a
`TickNaive*` that is not 8-byte aligned and reading `price_ticks` is undefined behaviour
even on x86-64, where the hardware would tolerate it. Compilers use the assumption to emit
aligned vector loads, and the failure shows up as a `SIGSEGV` inside a memcpy you did not
write. This is the second obligation attached to `std::start_lifetime_as` in lesson 17.
:::

## alignas and aligned allocation

`alignas(N)` raises a type's or object's alignment to `N`, which must be a power of two.
You cannot lower alignment with it.

```cpp
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

struct alignas(64) RingSlot {       // one whole cache line per slot
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t seq;
};
static_assert(sizeof(RingSlot) == 64);   // padded up to a multiple of alignof
```

Over-alignment inflates `sizeof` to a multiple of the alignment. A 16-byte payload declared
`alignas(64)` occupies 64 bytes, so an array of them has one slot per line: exactly what you
want for a ring buffer and exactly what you do not want for a bulk data table.

Getting over-aligned memory dynamically has been well-defined since C++17. Plain `new`
respects the type's alignment by routing to the aligned `operator new` overload:

```cpp
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

int main() {
    auto* slots = new RingSlot[1024];             // aligned new: honours alignas(64)
    // ... use slots ...
    delete[] slots;

    // Raw buffer, C-style. Size must be a multiple of alignment.
    void* p = std::aligned_alloc(64, 64 * 1024);  // <cstdlib>
    std::free(p);                                 // NOT delete

    // std::vector uses std::allocator, which honours alignof(T) since C++17.
    std::vector<RingSlot> ring(1024);             // each element 64-byte aligned
}
```

The trap is `std::aligned_alloc`: its size must be an integer multiple of the alignment, and
the result is freed with `std::free`, never `delete`.

## Split loads and the 4 KB special case

An object that begins in one cache line and ends in the next causes a **split load**, or
split store: the core issues two line accesses and merges the halves. Within a page that
typically costs a few extra cycles on modern Intel and AMD parts and, more importantly,
consumes two load-buffer entries, so a loop full of them loses throughput as well as
latency. `perf` counts them as `ld_blocks.no_sr` and `mem_inst_retired.split_loads`.

Consider a 24-byte tick in a packed array. Element 0 occupies bytes 0-23, element 1 bytes
24-47, element 2 bytes 48-71, which straddles the boundary at 64. Two of every eight
elements split. Rounding the record to 32 bytes as lesson 20 did costs 33% more memory and
removes every split, usually the better trade for a randomly accessed structure.

The pathological case is a split across a **4 KB page boundary**. The halves are in
different pages, so two TLB lookups, possibly two page walks, and on older Intel
microarchitectures a penalty around 100 cycles. Never acceptable in a hot loop; align any
structure you access whole to at least its own size.

:::key
One rule prevents almost all of this: **make the size of any record you index into a power
of two that divides 64, and align the array to 64.** A `sizeof(T)` of 8, 16, 32 or 64 then
guarantees no element straddles a line, and `static_assert(64 % sizeof(T) == 0)` enforces it
forever.
:::

## False sharing

Now the expensive one. Cache coherence operates on lines, not variables. Under the MESI
protocol a line in a core's L1 is Modified, Exclusive, Shared or Invalid. To write, a core
must own the line in Modified state, which means broadcasting a **request for ownership**
and invalidating every other copy.

So when two threads on two cores write two *different* variables that occupy the *same*
line, the hardware cannot tell they are independent. Each write steals the line back, and it
ping-pongs across the interconnect at the cost of a cross-core transfer every time. This is
**false sharing**. It is invisible in the source, invisible in a flame graph, and produces a
program that gets slower as you add threads.

```cpp
#include <atomic>
#include <cstdint>

struct CountersBad {                                 // both in one 64-byte line
    std::atomic<std::uint64_t> ticks_seen{0};        // written by the feed thread
    std::atomic<std::uint64_t> orders_sent{0};       // written by the strategy thread
};
```

Two threads, each doing ten million `fetch_add(1, std::memory_order_relaxed)` on its own
counter. On a typical dual-socket Xeon with both threads on the same socket, the padded
version below takes roughly 8 to 12 milliseconds per thread, about 1 ns per increment; the
unpadded version above takes roughly 100 to 200 milliseconds, about 15 ns per increment. A
**slowdown of 10 to 15 times** purely from sharing a line, and several times worse again
across sockets. Figures from one machine class, but expect an order of magnitude anywhere.

The fix is to give each contended variable its own line:

```cpp
#include <atomic>
#include <cstdint>
#include <new>

// hardware_destructive_interference_size: the minimum offset between two objects
// to avoid false sharing. 64 on x86-64 in libstdc++ and libc++.
inline constexpr std::size_t kLine = std::hardware_destructive_interference_size;

struct alignas(kLine) PaddedCounter {
    std::atomic<std::uint64_t> v{0};
    char pad[kLine - sizeof(std::atomic<std::uint64_t>)]{};
};
static_assert(sizeof(PaddedCounter) == kLine);

struct CountersGood {
    PaddedCounter ticks_seen;
    PaddedCounter orders_sent;
};
static_assert(sizeof(CountersGood) == 2 * kLine);
```

`std::hardware_constructive_interference_size` is the companion: the maximum size for which
two objects are likely to share a line, so it is the hint for things you *want* together.
Both live in `<new>`.

:::warn
Two caveats. GCC warns under `-Winterference-size` that their value is part of your ABI and
can change between compiler versions, so never use them in a struct crossing a library
boundary without pinning the number. More practically, Intel parts from Sandy Bridge onward
have an L2 **adjacent-line prefetcher** that fetches lines in aligned 128-byte pairs, so two
variables 64 bytes apart can still ping-pong, and Apple silicon uses 128-byte lines outright.
If 64 bytes of padding did not help as much as you expected, try 128.
:::

## Fixing, sharding and detecting

Three patterns cover nearly every real case.

**Pad the producer and consumer indices of a ring buffer.** This is the canonical case,
because it is the one place where two threads write adjacent integers by design. The producer
writes `head` on every push, the consumer writes `tail` on every pop, and each reads the
other. In one line they ping-pong on every operation.

```cpp
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

template <typename T, std::size_t N>       // N a power of two
class SpscRing {
    static_assert((N & (N - 1)) == 0);
    static constexpr std::size_t kLine = std::hardware_destructive_interference_size;

    alignas(kLine) std::atomic<std::size_t> head_{0};   // written by producer only
    alignas(kLine) std::atomic<std::size_t> tail_{0};   // written by consumer only
    alignas(kLine) T buf_[N];                           // and the data on its own lines
public:
    bool push(const T& v) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) return false;
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
};
```

Three `alignas` declarations, and a queue that scales instead of collapsing. Lesson 33 builds
this out properly, including the cached-index trick that removes most of the remaining
cross-core traffic.

**Shard counters per core.** A global `std::atomic<uint64_t> messages_processed` incremented
by eight feed threads is eight cores fighting over one line. Give each thread a padded slot
and sum them when someone asks.

```cpp
#include <array>
#include <atomic>
#include <cstdint>

template <std::size_t Threads>
class ShardedCounter {
    std::array<PaddedCounter, Threads> shards_{};
public:
    // Hot path: one relaxed increment on a line no other thread touches.
    void bump(std::size_t tid) noexcept {
        shards_[tid].v.fetch_add(1, std::memory_order_relaxed);
    }
    // Cold path: called by the stats thread once a second.
    [[nodiscard]] std::uint64_t total() const noexcept {
        std::uint64_t s = 0;
        for (const auto& c : shards_) s += c.v.load(std::memory_order_relaxed);
        return s;
    }
};
```

**Detect it with `perf c2c`.** Cache-to-cache analysis is the only tool that names the
problem directly. It samples loads and stores with their physical addresses and reports the
lines being transferred between cores, down to the offset within the line and the source
line that touched it.

```sh
$ perf c2c record -F 60000 -a -- ./trader --replay capture.pcap
$ perf c2c report --stdio
```

Read the "Shared Data Cache Line Table" and look for lines with a high count of **HITM**
events: a load that hit a line held Modified in another core's cache. A hot line with two
different offsets attributed to two threads is false sharing. The same offset from many
threads is true sharing, a different problem needing a different fix. Lesson 28 covers
`perf` properly.

:::hft
False sharing is the most common reason a trading system fails to scale from four threads to
eight, and it is almost never where people look. The usual story: the feed handler and the
strategy each have a small stats struct, both came out of the same allocation, and 40
nanoseconds of interconnect traffic landed on a path with a 2-microsecond budget. Padding
costs nothing. Do it by default on any variable two threads write.
:::

One near relative looks like false sharing in a profile. **4K aliasing** is a false
dependency between a load and a pending store whose addresses match in their low 12 bits but
are actually 4 KB apart. The core cannot disambiguate quickly and blocks the load, typically
for 5 to 7 cycles, counted by `ld_blocks_partial.address_alias`. It appears in
single-threaded code, most often when copying between two buffers whose sizes are multiples
of 4096. Offsetting one buffer by a cache line removes it, the same fix as the conflict-miss
pathology in lesson 19 and for the same reason: powers of two are hostile to hardware that
indexes with address bits.

:::exercise
Benchmark two threads each incrementing its own `std::uint64_t` ten million times, with the
counters as adjacent members of a struct. Record the wall time, add `alignas(64)` to each,
record it again, and report the ratio. Repeat with `alignas(128)` to see whether it moves
further on your machine. Then run the unpadded version under `perf c2c record` and confirm
the report names your struct.
:::

## Takeaways

- Alignment is a property of the type. Compute `sizeof` by hand with round-up-place-advance, then lock it in with `static_assert` on `sizeof` and `offsetof`.
- `alignas` raises alignment and inflates `sizeof` to a multiple of it. Plain `new` and `std::vector` honour over-alignment since C++17; `std::aligned_alloc` needs a size that is a multiple of the alignment and is freed with `std::free`.
- Keep record sizes to a power of two dividing 64 so no element straddles a line. A split across a 4 KB page boundary can cost around 100 cycles on some parts.
- False sharing is two threads writing distinct variables in one line. It typically costs a factor of 10 or more on a same-socket pair and shows up as a program that will not scale with threads.
- Pad with `std::hardware_destructive_interference_size`, minding the ABI warning and the 128-byte adjacent-line prefetch pair on Intel and Apple parts.
- `perf c2c` names the guilty cache line. High HITM counts at two offsets from two threads means false sharing.
