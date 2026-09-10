---
title: Programming Without the Heap
part: Part III - The Machine
summary: Why malloc is banned on the hot path, and the full toolkit that replaces it: StaticVector, arenas, handle pools, pmr, pre-faulting and an operator new that aborts.
time: 45 min
level: advanced
tags: allocation, arena, pool, pmr, static-vector
---

There is one rule that every low-latency trading desk enforces and that no other kind of
C++ shop cares about: the hot path does not allocate. Not "allocates rarely", not "allocates
efficiently". Zero calls to `operator new`, `malloc`, or anything that reaches them, between
the packet arriving and the order leaving.

The reason is not that allocation is slow on average. Modern allocators are fast on average.
The reason is that allocation has a **tail**, and in this business you are paid on the tail.

## What actually happens inside malloc

Trace a call to `operator new(64)` on a Linux box running glibc, and count the things that
can go wrong.

1. **A lock, or a thread-cache miss.** glibc's malloc gives each thread a small tcache of
   recently freed chunks. Hit it and you are twenty-odd nanoseconds. Miss it and you take the
   arena lock, a `pthread_mutex`, contended with every other thread that hashed to the same
   arena.
2. **A free-list walk.** On a miss the allocator searches size-class bins, may coalesce
   adjacent free chunks, may split a larger chunk. This is pointer chasing through metadata
   that is almost certainly cold, so each hop is a potential last-level-cache miss at
   roughly 80 to 100 ns on a typical server part.
3. **A syscall.** If no chunk fits, the allocator asks the kernel for more: `brk` to extend
   the heap, or `mmap` for a large request. A syscall is a mode transition, a few hundred
   nanoseconds at minimum, and on a kernel with speculative-execution mitigations enabled it
   is considerably worse.
4. **A first-touch page fault.** `mmap` does not give you memory, it gives you a promise.
   The first write to each 4 KB page traps into the kernel, which finds a physical frame,
   zeroes it, and updates the page tables. Around 1 to 3 microseconds per page on a typical
   server, and a 1 MB buffer is 256 pages of that.
5. **A TLB miss.** New pages mean new translations. The translation lookaside buffer has on
   the order of 1500 to 2500 entries for 4 KB pages on recent server cores. Fresh memory
   means fresh translations means a page-table walk on first access.
6. **`free` is not free either.** Coalescing, returning memory to the OS with `madvise`,
   possibly `munmap`. Deallocation can be slower than allocation.

:::key
The p50 of `malloc` might be 25 ns. The p99.9 is a page fault and a syscall, and it is three
orders of magnitude worse. A hot path that allocates has a latency distribution with a long
right tail, and market-moving events are exactly when you allocate most — bursts of messages,
deeper books, more orders — so the tail and the opportunity arrive together. That correlation
is what makes this a discipline rather than a micro-optimisation.
:::

The strategic answer is a single idea applied everywhere: **decide the capacity before the
market opens**. Everything in this lesson is a way of making bounded capacity ergonomic.

## Bounded capacity, and a StaticVector you can ship

`std::vector<T>` owns a heap buffer and grows by reallocating. `std::array<T, N>` owns
storage inline with no allocation, but its size is fixed at exactly `N` and every element is
default-constructed. What a trading system usually wants is the middle: **fixed capacity,
variable size, no allocation ever**.

That container is a fixed-capacity vector. It is in Boost, it is in most desks' internal
libraries, and it is not in the standard. Here it is in full.

```cpp static_vector.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace hft {

// Fixed capacity N, variable size, storage inline, never allocates.
template <typename T, std::size_t N>
class StaticVector {
    static_assert(N > 0, "capacity must be positive");

public:
    using value_type = T;
    // Keep the header small: a 32-level book does not need a 64-bit size.
    using size_type = std::conditional_t<(N <= 255), std::uint8_t,
                      std::conditional_t<(N <= 65535), std::uint16_t, std::uint32_t>>;

    StaticVector() = default;
    ~StaticVector() { clear(); }

    StaticVector(const StaticVector& other) {
        for (size_type i = 0; i < other.size_; ++i) push_back(other[i]);
    }
    StaticVector& operator=(const StaticVector& other) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.size_; ++i) push_back(other[i]);
        }
        return *this;
    }

    // Returns false instead of growing. Every wire-fed path checks this.
    bool push_back(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        if (size_ == N) return false;
        std::construct_at(data() + size_, value);
        ++size_;
        return true;
    }

    // Returns nullptr instead of growing.
    template <typename... Args>
    T* emplace_back(Args&&... args) {
        if (size_ == N) return nullptr;
        T* p = std::construct_at(data() + size_, std::forward<Args>(args)...);
        ++size_;
        return p;
    }

    void pop_back() noexcept { --size_; std::destroy_at(data() + size_); }
    void clear()    noexcept { std::destroy_n(data(), size_); size_ = 0; }

    // Order-destroying erase: O(1) because order is not an invariant here.
    void erase_unordered(size_type index) noexcept {
        data()[index] = std::move(data()[size_ - 1]);
        pop_back();
    }

    [[nodiscard]] T*       data()       noexcept { return reinterpret_cast<T*>(storage_); }
    [[nodiscard]] const T* data() const noexcept { return reinterpret_cast<const T*>(storage_); }

    [[nodiscard]] T&       operator[](size_type i)       noexcept { return data()[i]; }
    [[nodiscard]] const T& operator[](size_type i) const noexcept { return data()[i]; }
    [[nodiscard]] T&       back()                        noexcept { return data()[size_ - 1]; }

    [[nodiscard]] T*       begin()       noexcept { return data(); }
    [[nodiscard]] T*       end()         noexcept { return data() + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end()   const noexcept { return data() + size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full()  const noexcept { return size_ == N; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return N; }

private:
    alignas(T) std::byte storage_[sizeof(T) * N];
    size_type size_ = 0;
};

}  // namespace hft
```

The points that matter:

- Storage is `std::byte` with `alignas(T)`, not `T[N]`. Raw bytes mean no element is
  constructed until you ask for one, which is what lets `size()` differ from `capacity()`.
- `std::construct_at`, `std::destroy_at` and `std::destroy_n` from `<memory>` are the C++20
  spellings of placement-new and explicit destructor calls. They are constexpr-friendly and
  they say what they mean.
- `size_type` shrinks with `N`. A `StaticVector<Level, 32>` carries a one-byte size, so the
  whole object stays inside fewer cache lines. On a structure you touch per tick that is not
  cosmetic; see lesson 21.
- Neither `push_back` nor `emplace_back` can grow. One returns `false` and the other returns
  `nullptr`, and the caller must decide. That refusal is the entire design.

:::pitfall
`reinterpret_cast<T*>(storage_)` is the pragmatic spelling and it is what every real
implementation does, but strictly the standard wants `std::launder` on a pointer obtained by
casting the storage rather than from `construct_at`. GCC and Clang both accept the simple form
and generate correct code; add `std::launder` if your static analyser insists. What you must
not do is skip `construct_at` and assign into raw bytes, which is genuinely undefined for
non-trivial `T`.
:::

:::hft
The real design question is not "how do I avoid allocating" but "what is the bound". Answer it
explicitly for every hot-path container and write it down: 32 price levels because that is
where the exchange truncates depth; 4096 working orders because that is the risk limit; 512
messages per burst because that is the largest observed inter-packet batch plus 3x headroom.
Then decide what happens when the bound is exceeded, because it will be. The answer for a
trading system is almost always "reject, count it, alert" — never "grow" — because growing on
the hot path is exactly the failure you were avoiding.
:::

## Arenas and handle pools

Two allocation patterns cover almost everything a hot path needs.

### The bump allocator, for per-event scratch

If everything you allocate during one market event dies at the end of that event, you do not
need a general allocator. You need a pointer that moves forward and gets reset.

```cpp arena.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace hft {

// Bump allocator over caller-supplied storage. Allocation is an add and a compare.
class Arena {
public:
    Arena(std::byte* buffer, std::size_t bytes) noexcept
        : begin_(buffer), end_(buffer + bytes), cursor_(buffer) {}

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* p = allocate(sizeof(T), alignof(T));
        return p ? std::construct_at(static_cast<T*>(p), std::forward<Args>(args)...) : nullptr;
    }

    // Returns nullptr when exhausted. Never throws, never calls the OS.
    void* allocate(std::size_t bytes, std::size_t align) noexcept {
        auto addr = reinterpret_cast<std::uintptr_t>(cursor_);
        const std::uintptr_t aligned = (addr + align - 1) & ~(std::uintptr_t{align} - 1);
        auto* next = reinterpret_cast<std::byte*>(aligned) + bytes;
        if (next > end_) return nullptr;
        cursor_ = next;
        return reinterpret_cast<void*>(aligned);
    }

    // Frees everything at once. Valid only when every T placed here is
    // trivially destructible, or you have destroyed them yourself.
    void reset() noexcept { cursor_ = begin_; }

    [[nodiscard]] std::size_t used() const noexcept {
        return static_cast<std::size_t>(cursor_ - begin_);
    }

private:
    std::byte* begin_;
    std::byte* end_;
    std::byte* cursor_;
};

}  // namespace hft
```

```cpp arena_use.cpp
#include "arena.hpp"
#include <array>
#include <cstdint>

struct ParsedOrder { std::uint64_t id; std::int64_t price_ticks; std::uint32_t qty; };

// One arena per event loop, allocated once at startup.
alignas(64) static std::array<std::byte, 1 << 20> g_scratch_storage;
static hft::Arena g_scratch{g_scratch_storage.data(), g_scratch_storage.size()};

void on_packet(const std::byte* /*payload*/, std::size_t n_orders) noexcept {
    for (std::size_t i = 0; i < n_orders; ++i) {
        ParsedOrder* o = g_scratch.create<ParsedOrder>(i, 100'000 + std::int64_t(i), 10u);
        if (!o) break;                       // bounded: drop and count, never grow
        // ... use o ...
    }
    g_scratch.reset();                       // O(1) release of everything above
}
```

`allocate` is an add, a mask and a compare — a handful of cycles, no lock, no syscall, and no
page fault provided the storage was pre-faulted. `reset` frees a megabyte in one store.

### The object pool, returning 32-bit handles

When objects outlive the event that created them — working orders, book levels, subscriptions
— you need individual reuse. A free-list pool over a fixed array gives you that, and returning
a 32-bit **handle** rather than a pointer halves the size of every reference and makes them
trivially serialisable and comparable.

```cpp pool.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace hft {

// Slab of fixed-size objects addressed by 32-bit handles.
// The free list lives inside the index array, so there is no extra storage.
template <typename T, std::size_t N>
class ObjectPool {
public:
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    ObjectPool() {
        for (std::uint32_t i = 0; i < N - 1; ++i) next_free_[i] = i + 1;
        next_free_[N - 1] = kInvalid;
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    std::uint32_t acquire(Args&&... args) {
        if (free_head_ == kInvalid) return kInvalid;   // exhausted: caller decides
        const std::uint32_t handle = free_head_;
        free_head_ = next_free_[handle];
        std::construct_at(slot(handle), std::forward<Args>(args)...);
        ++live_;
        return handle;
    }

    void release(std::uint32_t handle) noexcept {
        std::destroy_at(slot(handle));
        next_free_[handle] = free_head_;
        free_head_ = handle;
        --live_;
    }

    [[nodiscard]] T& operator[](std::uint32_t handle) noexcept { return *slot(handle); }
    [[nodiscard]] std::uint32_t live() const noexcept { return live_; }
    static constexpr std::size_t capacity() noexcept { return N; }

private:
    T* slot(std::uint32_t i) noexcept { return reinterpret_cast<T*>(storage_) + i; }

    alignas(T) std::byte storage_[sizeof(T) * N];
    std::uint32_t next_free_[N]{};
    std::uint32_t free_head_ = 0;
    std::uint32_t live_ = 0;
};

}  // namespace hft
```

```cpp pool_use.cpp
#include "pool.hpp"
#include <cstdint>

struct WorkingOrder {
    std::uint64_t exch_id;
    std::int64_t  price_ticks;
    std::uint32_t qty_remaining;
};

using OrderPool = hft::ObjectPool<WorkingOrder, 4096>;
static OrderPool g_orders;

std::uint32_t send_new(std::uint64_t id, std::int64_t px, std::uint32_t qty) noexcept {
    const auto h = g_orders.acquire(id, px, qty);
    if (h == OrderPool::kInvalid) return h;        // at the risk limit: reject
    // ... write the order out ...
    return h;
}

void on_fill(std::uint32_t h, std::uint32_t filled) noexcept {
    WorkingOrder& o = g_orders[h];
    o.qty_remaining -= filled;
    if (o.qty_remaining == 0) g_orders.release(h);
}
```

`acquire` and `release` are each a load, a store and an increment. The pool's storage is one
contiguous block, so iterating live objects has predictable strides and the prefetcher can
follow them, which a `new`-per-object design never achieves.

:::warn
Handles are reused. Release handle 7 and the next `acquire` will very likely hand back 7,
pointing at a different order. A stale handle held across a release is a use-after-free that
does not crash — it silently reads someone else's order. If a handle can outlive its object,
pack a generation counter alongside the index (say 24 bits of index and 8 bits of generation
in the 32) and validate on dereference. Pay that cost once, at the boundary where handles
escape, not on every access.
:::

## Standard containers that stop calling malloc

Sometimes you need `std::vector`'s interface, or a third-party API demands a standard
container. `std::pmr` from `<memory_resource>` lets you keep the container and replace the
allocator, without the allocator type infecting your function signatures the way the classic
`Allocator` template parameter does.

```cpp pmr_burst.cpp
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

void on_snapshot(const Level* wire, std::size_t n) noexcept {
    // Storage on the stack: no allocation, and it is already faulted in.
    alignas(64) std::array<std::byte, 64 * 1024> buf;

    // null_memory_resource as upstream: exhaustion throws bad_alloc rather
    // than silently falling back to the global heap. That is the point.
    std::pmr::monotonic_buffer_resource rsrc{
        buf.data(), buf.size(), std::pmr::null_memory_resource()};

    std::pmr::vector<Level> levels{&rsrc};
    levels.reserve(n);                       // one bump from the buffer
    for (std::size_t i = 0; i < n; ++i) levels.push_back(wire[i]);

    // ... work with levels ...
}   // no deallocation walk: the resource's destructor drops everything at once
```

Three things to understand about `std::pmr`:

- `std::pmr::vector<T>` is `std::vector<T, std::pmr::polymorphic_allocator<T>>`. The allocator
  is a type-erased pointer to a `memory_resource`, so the container type does not change when
  the resource does.
- `monotonic_buffer_resource` is a bump allocator with the same shape as the `Arena` above. It
  never reuses individual deallocations; it releases everything in its destructor.
- **The upstream resource matters more than anything else here.** The default upstream is
  `new_delete_resource()`, which means overflowing your buffer quietly starts calling the
  global heap and your allocation-free property evaporates without a sound. Passing
  `null_memory_resource()` converts that silence into a `std::bad_alloc` you can catch in a
  test.

The costs are real and you should know them: every allocation goes through a virtual call on
`memory_resource`, and the type erasure blocks some inlining. For a burst of a few hundred
`push_back`s that is irrelevant. For a per-tick inner loop, use `StaticVector`.

### Small-buffer optimisation

The same idea applied inside a single object: keep storage inline for the common small case,
spill to the heap only for the rare large one.

```cpp sbo.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

// Holds a message body inline up to Inline bytes; heap only beyond that.
// Typical market-data messages are well under 128 bytes, so the heap path
// is exercised roughly never in production and exists only for correctness.
template <std::size_t Inline = 128>
class MessageBuffer {
public:
    explicit MessageBuffer(std::size_t n) {
        if (n <= Inline) { ptr_ = inline_; }
        else { heap_ = std::make_unique<std::byte[]>(n); ptr_ = heap_.get(); }
        size_ = n;
    }
    std::span<std::byte> bytes() noexcept { return {ptr_, size_}; }
    bool spilled() const noexcept { return heap_ != nullptr; }

private:
    std::byte  inline_[Inline];
    std::unique_ptr<std::byte[]> heap_{};
    std::byte* ptr_ = nullptr;
    std::size_t size_ = 0;
};
```

This is exactly how libstdc++ and libc++ implement `std::string`: 15 or 22 bytes inline
depending on the library, heap beyond that. It is the reason short strings are fast and the
reason you should still never build a string on the hot path.

## When you cannot avoid it: mitigation and pre-faulting

Some code you did not write allocates. Some paths are not hot enough to justify a rewrite.
Three mitigations, in order of value.

**`reserve()` is the minimum viable answer.** One allocation at a known size, at a moment you
choose, instead of a doubling sequence at moments the data chooses.

```cpp reserve.cpp
#include <cstdint>
#include <vector>

struct Level { std::int64_t price_ticks; std::uint32_t qty; };

class BookSide {
public:
    BookSide() {
        levels_.reserve(kMaxLevels);   // once, at construction, off the hot path
    }
    void clear_keep_capacity() noexcept { levels_.clear(); }  // clear() never frees

private:
    static constexpr std::size_t kMaxLevels = 256;
    std::vector<Level> levels_;
};
```

Note that `clear()` destroys elements but keeps capacity, so a vector reserved once at startup
and cleared per event never allocates again. What you must never do is `shrink_to_fit()`, or
copy the vector by value, or move it into a container that then reallocates.

**Pre-touch and lock everything at startup.** Reserving virtual address space is not the same
as owning physical pages. Force the fault before the market opens.

```cpp prefault.cpp
#include <cstddef>
#include <cstring>
#include <sys/mman.h>

// Linux. Touch every page so the kernel maps a physical frame now,
// then pin it so it can never be reclaimed or swapped.
void prefault_and_pin(void* p, std::size_t bytes) noexcept {
    std::memset(p, 0, bytes);          // first touch: one fault per 4 KB page
    ::mlock(p, bytes);                 // pin: no swap, no reclaim
}

void lock_process_memory() noexcept {
    // Everything mapped now, and everything mapped later.
    ::mlockall(MCL_CURRENT | MCL_FUTURE);
}
```

Combine this with `mmap(..., MAP_POPULATE | MAP_HUGETLB, ...)` for large arenas: `MAP_POPULATE`
pre-faults at map time, and 2 MB huge pages cut the number of TLB entries your working set
needs by a factor of 512. Startup gets slower by a few hundred milliseconds, which nobody is
measuring, and the first trade of the day stops being ten microseconds slower than the second,
which everybody is.

**Warm the path.** Run synthetic messages through the full hot path during the pre-open
window, with the risk gate set to reject. This faults in the code pages, populates the branch
predictor and the instruction cache, and exercises every lazily-initialised static.

## Enforcement: make an accidental allocation impossible to miss

None of this survives contact with a growing team unless it is enforced mechanically. The
enforcement trick is to replace the global allocator with one that aborts while a flag is set.

```cpp no_alloc_guard.hpp
#pragma once

namespace guard {
    // Not atomic: the hot path is single-threaded by design. Use a
    // thread_local bool if your test harness is not.
    inline bool g_hot_path_active = false;

    struct Scope {
        Scope()  noexcept { g_hot_path_active = true;  }
        ~Scope() noexcept { g_hot_path_active = false; }
    };
}  // namespace guard
```

```cpp no_alloc_guard.cpp
#include "no_alloc_guard.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

// Exactly one TU in the test binary defines these.
void* operator new(std::size_t n) {
    if (guard::g_hot_path_active) {
        std::fputs("FATAL: allocation on the hot path\n", stderr);
        std::abort();                     // core dump, with the call stack
    }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }

void operator delete(void* p)                  noexcept { std::free(p); }
void operator delete(void* p, std::size_t)     noexcept { std::free(p); }
void operator delete[](void* p)                noexcept { std::free(p); }
void operator delete[](void* p, std::size_t)   noexcept { std::free(p); }
```

```cpp no_alloc_test.cpp
#include "no_alloc_guard.hpp"
#include <cstddef>

void on_market_data(const std::byte* payload, std::size_t len) noexcept;

// In the test binary, not in production.
void test_hot_path_is_allocation_free(const std::byte* pkt, std::size_t len) {
    guard::Scope hot;          // any new between here and the closing brace aborts
    on_market_data(pkt, len);
}
```

Three notes on doing this properly. Link the guard only into the test binary, or gate it on
`NDEBUG`, so a production process facing a genuine surprise degrades rather than dying.
Override the aligned forms too — `operator new(std::size_t, std::align_val_t)` and its
partners — or an aligned allocation will slip past. And run the guarded test under the same
build flags as production, because the two builds do not allocate in the same places: `-O0`
allocates where `-O2` does not, and, in the direction that will actually catch you out, C++14
onward permits the compiler to **elide** an allocation whose result is unused, so a `-O2` build
can silently optimise away the very `new` your test was trying to trap.

The lighter-weight alternative for continuous monitoring is to count rather than abort:
increment a per-thread counter in `operator new`, and assert in your per-event telemetry that
the count did not change across the hot path. That catches the same bug without the risk of
killing a live process.

:::exercise
Take a feed handler that builds a `std::vector<Level>` per snapshot message and converts it to
an `hft::StaticVector<Level, 256>`. Then:

1. Link the aborting `operator new` into a test binary and confirm the original version trips
   it and the new version does not.
2. Measure both with the histogram approach from lesson 26, over at least a million messages,
   and compare p50, p99 and max — not the mean. Expect a modest p50 improvement and a max that
   improves by one to three orders of magnitude. That second number is the whole point.
3. Now deliberately break it: remove the `reserve` from a version that keeps the vector but
   clears it per event, and watch the max degrade as the doubling sequence reappears.
:::

## Takeaways

- The heap is banned on the hot path because of its tail, not its mean: a lock, a free-list
  walk, a `brk`/`mmap` syscall, a page fault at roughly 1 to 3 microseconds per page, and a
  TLB miss all hide behind one `new`.
- Bound every hot-path container's capacity at compile time and decide explicitly what happens
  on overflow: reject and count, never grow. `StaticVector<T, N>` is the workhorse — inline
  aligned storage, `std::construct_at`/`std::destroy_at` for lifetime, and a `size_type` sized
  to keep the header small.
- An arena resets a megabyte in one store; a free-list pool hands out 32-bit handles with O(1)
  acquire and release. Guard handle reuse with a generation counter if handles can go stale.
- `std::pmr::monotonic_buffer_resource` over stack storage with `null_memory_resource()`
  upstream keeps standard containers off the global heap and makes overflow loud.
- Pre-fault and `mlock` every buffer at startup, and warm the path before the open, so the
  first trade of the day is not the slowest.
- Enforce it: an `operator new` that aborts under a guard turns a design rule into a test
  failure.
