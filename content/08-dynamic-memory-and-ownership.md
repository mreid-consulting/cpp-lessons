---
title: new, delete and Smart Pointers
part: Part I - Foundations
summary: What operator new really does to your latency, how unique_ptr costs nothing and shared_ptr costs more than you think, and a decision table for who owns what.
time: 35 min
level: intermediate
tags: heap, malloc, unique_ptr, shared_ptr, ownership
---

There is a reason that "does it allocate?" is the first question asked in a trading code
review. A heap allocation is not a slow operation; it is an *unpredictable* one. Its
median cost is small enough to ignore and its tail is large enough to lose you a fill. To
argue about that intelligently you need to know what the allocator is actually doing, and
then you need a vocabulary for expressing ownership without reaching for the heap at all.

## What `new` actually does

`new Order{...}` is two things bolted together: a call to `operator new(size_t)` to obtain
raw storage, and then construction of the object in that storage. The second half is
usually free. The first half is a general-purpose allocator, typically glibc's malloc,
jemalloc or tcmalloc, and here is roughly what it does on a modern glibc:

1. **Size class lookup.** Round the request up to a bin size. Small requests hit a
   per-thread cache (glibc's tcache), which is a singly linked free list — a load, a
   pointer update, done.
2. **Possible lock.** If the tcache bin is empty, fall back to the arena. That means an
   atomic compare-exchange or a mutex on a structure shared with other threads on the same
   arena. Under contention this is where the microseconds appear.
3. **Possible syscall.** If the arena has no suitable free chunk, extend the heap with
   `brk`, or for requests above the mmap threshold (128 KiB by default) call `mmap`
   directly. That is a kernel transition of roughly 1-2 microseconds, plus whatever the
   kernel does with the page tables.
4. **Possible page fault.** `mmap` returns virtual address space, not memory. The first
   write to each 4 KiB page traps into the kernel, which finds a physical page, zeroes it,
   and installs a page table entry. Several microseconds each, and it happens on *your*
   first touch, not on the allocation.

```cpp
#include <cstdint>
#include <memory>
#include <new>

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

void what_new_does() {
    void* raw = ::operator new(sizeof(Order));      // step 1: storage
    Order* p  = new (raw) Order{1, 10050, 100};     // step 2: placement new, construction
    p->~Order();                                     // explicit destruction
    ::operator delete(raw);
}
```

`new Order{...}` is exactly that pair, written for you, with the added guarantee that if
the constructor throws the storage is released.

:::perf
Rough figures on a modern x86-64 server, glibc 2.35, single-threaded warm heap: a small
`malloc`/`free` pair costs on the order of **20-40 ns** at the median. The p99 under
multi-threaded load is commonly **hundreds of nanoseconds**, and the p99.99, where you hit
arena contention or a fresh `mmap` plus page faults, runs into **tens of microseconds**.
Measure your own allocator on your own workload; the distribution, not the mean, is the
thing to look at.
:::

:::hft Why the tail is the whole story
A tick-to-trade budget of 2 microseconds does not care that the average allocation is
30 ns. It cares that one allocation in ten thousand takes 20 microseconds, because the
tick that hits that allocation is disproportionately likely to be the one that mattered:
allocator slow paths correlate with bursts of market activity, which is precisely when
every other participant is also trying to trade. Tail latency in the allocator is not
random noise sprinkled evenly across your day. It clusters exactly where your P&L is.

The conclusion the industry has reached is not "use a faster allocator". It is "do not
allocate on the hot path at all". Lesson 24 covers the replacements: arenas, object pools,
fixed-capacity containers and `std::pmr`.
:::

## The four ways manual memory goes wrong

```cpp
#include <cstdint>

void leaks(std::uint32_t n) {
    auto* buf = new std::uint8_t[n];
    if (n == 0) return;              // leak: buf never freed on this path
    delete[] buf;
}

void mismatched(std::uint32_t n) {
    auto* buf = new std::uint8_t[n];
    delete buf;                      // UB: new[] must be paired with delete[]
}

void double_free() {
    auto* p = new int{7};
    delete p;
    delete p;                        // UB: heap metadata corruption, crash somewhere else later
}

int use_after_free() {
    auto* p = new int{7};
    delete p;
    return *p;                       // UB: reads freed memory, often "works" in testing
}
```

None of these produce a diagnostic. A **leak** costs you memory and eventually a page-fault
storm. A **double free** corrupts allocator metadata and crashes in an unrelated
allocation minutes later. A **use after free** reads whatever was written there since,
which in a trading system means an order for the wrong quantity at the wrong price. Build
your tests with `-fsanitize=address,undefined`; it catches all four.

The fix is not discipline. The fix is to make the compiler do the freeing, which is what
RAII and smart pointers are for.

## unique_ptr: a pointer that owns

`std::unique_ptr<T>` holds exactly one pointer and calls `delete` on it in its destructor.
It is move-only: copying it would create two owners, so the copy constructor is deleted.

```cpp
#include <cstdint>
#include <memory>
#include <utility>

struct Session {
    std::uint64_t id;
    std::uint8_t  buffer[4096];
};

std::unique_ptr<Session> open_session(std::uint64_t id) {
    auto s = std::make_unique<Session>();        // value-initialises, one allocation
    s->id = id;
    return s;                                    // moved out, no copy
}

void hand_off() {
    std::unique_ptr<Session> a = open_session(7);
    std::unique_ptr<Session> b = std::move(a);   // a is now null, b owns it
    // exactly one delete happens, when b goes out of scope
}
```

With the default deleter, `sizeof(std::unique_ptr<Session>) == sizeof(Session*)` — 8 bytes
— and at `-O2` the generated code is identical to a raw pointer plus a `delete` at the
right place. That is what "zero-overhead abstraction" means concretely: you pay for the
`delete`, which you needed anyway, and nothing else.

Prefer `std::make_unique<T>(args...)` over `std::unique_ptr<T>(new T(args...))`: it is one
expression, it never leaks under exceptions, and it does not repeat the type.

### Custom deleters change the size

The deleter is a template parameter, and a *stateful* deleter must be stored inside the
`unique_ptr`.

```cpp
#include <cstdio>
#include <memory>

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;   // stateless: sizeof == 8

FilePtr open_log(const char* path) {
    return FilePtr{std::fopen(path, "wb")};
}
```

```cpp
#include <cstddef>
#include <memory>

struct PoolDeleter {
    void* pool;                                            // state!
    void operator()(void* p) const noexcept;
};

// sizeof(std::unique_ptr<std::byte, PoolDeleter>) == 16: the pointer plus the pool handle.
```

A stateless functor is stored as an empty base and costs nothing. A lambda with captures,
or a function pointer, adds its own size. `std::unique_ptr<T, void(*)(T*)>` is 16 bytes and
loses the inlining, because the deleter is now an indirect call. Prefer a stateless struct
with `operator()`.

## shared_ptr: what you are really buying

`std::shared_ptr<T>` is two pointers: one to the object, one to a **control block** holding
a strong count, a weak count, the deleter and the allocator. So `sizeof(std::shared_ptr<T>)`
is 16 on x86-64, twice a raw pointer, and copying it is not a pointer copy — it is an
atomic increment of the strong count.

```cpp
#include <cstdint>
#include <memory>

struct RiskLimits {
    std::int64_t max_notional_ticks;
    std::uint32_t max_order_qty;
};

void sharing() {
    auto limits = std::make_shared<RiskLimits>(1'000'000'000, 10'000u);
    auto copy   = limits;      // atomic fetch_add on the strong count
    // when both die, the last one to hit zero destroys the object and the control block
}
```

`std::make_shared` allocates the object and the control block in **one** allocation, which
is both faster and better for locality than `std::shared_ptr<T>(new T)`. The tradeoff is
that the object's storage cannot be released until the last `weak_ptr` also dies, since the
control block lives in the same block.

:::warn The cache line ping-pong
The reference count is a single word. If four threads each hold a copy of the same
`shared_ptr` and copy or destroy it, all four are doing `lock xadd` on the *same* cache
line. That line bounces between cores in the Modified state, and each transfer is a
coherence round trip.

Order of magnitude on a modern server: an uncontended atomic increment is roughly **5-10
ns**; the same increment on a line actively contended by another core costs **50-100 ns**,
and worse across sockets. So copying a `shared_ptr` in a loop across threads is not "a
pointer copy", it is a cross-core synchronisation event costing more than the work it
guards. Pass `const std::shared_ptr<T>&` or, better, a plain `const T&` when the callee
does not extend the lifetime.
:::

`std::weak_ptr<T>` is a non-owning observer of the same control block. It does not keep the
object alive; to use it you call `lock()`, which returns a `shared_ptr` that is null if the
object has already gone. It exists to break reference cycles and to hold references to
objects that may legitimately disappear.

```cpp
#include <cstdint>
#include <memory>
#include <print>

struct RiskLimits { std::int64_t max_notional_ticks; std::uint32_t max_order_qty; };

void observing(std::weak_ptr<RiskLimits> w) {
    if (auto s = w.lock()) {                   // atomic: only succeeds if count > 0
        std::print("max qty {}\n", s->max_order_qty);
    } else {
        std::print("limits already retired\n");
    }
}
```

### When shared ownership is honest

Shared ownership is right when the lifetime genuinely is not knowable at one point in the
code:

- **Configuration and reference data.** A symbol table or risk-limit snapshot published at
  startup, read by several components, replaced occasionally. Readers hold a `shared_ptr`
  so an in-flight reader is not invalidated by a swap.
- **Cold, long-lived objects.** Session objects, connection handles, logging sinks.
- **Background tasks.** Work handed to a thread pool that must keep its inputs alive
  until it finishes, with no ordering guarantee against the submitter.

It is a design smell when it is being used to *avoid deciding* who owns a thing. Symptoms:
`shared_ptr` in a struct that has exactly one owner; `shared_ptr` passed by value into
every function in a call chain; a `shared_ptr<T>` where `T&` would do because the caller
outlives the callee. Each of those is an atomic increment and decrement bought for nothing.

## An ownership decision table

| Situation | Use | Why |
|---|---|---|
| A small POD you are storing or returning | the value itself | no indirection, fits in registers |
| A parameter the function only reads | `const T&` or `T` if small | zero ownership, zero cost |
| A parameter the function writes through | `T&` | explicit at the definition, invisible at the call |
| An optional non-owning reference | `const T*` | null is meaningful, no lifetime claim |
| A contiguous sequence you do not own | `std::span<const T>` (lesson 16) | pointer plus length, no ownership |
| One owner, polymorphic or large, cold path | `std::unique_ptr<T>` | zero-overhead, move-only, self-documenting |
| Genuinely shared lifetime, cold path | `std::shared_ptr<T>` | pay the atomics where they do not matter |
| Breaking a cycle, or observing a maybe-dead object | `std::weak_ptr<T>` | non-owning, checked |
| Hot-path objects with a bounded population | an index into a preallocated array | no allocation, better locality, smaller handles |

That last row is the one that separates a trading system from ordinary application code.

```cpp order_arena.hpp
#pragma once
#include <array>
#include <cstdint>

struct Order {
    std::uint64_t id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};

// A handle is 4 bytes, copyable, comparable, and never dangles into freed memory:
// a stale handle indexes a live slot, which a generation counter can detect.
enum class OrderId : std::uint32_t {};

class OrderArena {
public:
    static constexpr std::uint32_t kCapacity = 65'536;

    OrderId create(std::int64_t price_ticks, std::uint32_t qty) noexcept {
        const std::uint32_t slot = next_++;
        slots_[slot] = Order{slot, price_ticks, qty};
        return static_cast<OrderId>(slot);
    }

    Order& operator[](OrderId h) noexcept {
        return slots_[static_cast<std::uint32_t>(h)];
    }

private:
    std::array<Order, kCapacity> slots_{};
    std::uint32_t next_ = 0;
};
```

No `new`, no `delete`, no reference counts, contiguous storage the prefetcher can follow,
and handles that are half the size of a pointer. Lesson 24 develops this properly with free
lists, generation counters and `std::pmr`.

:::exercise
Write a benchmark that allocates and frees one million `Session` objects (4 KiB each)
through `std::make_unique`, recording each allocation's duration with
`std::chrono::steady_clock`. Print the median, p99 and maximum. Then repeat with four
threads doing the same thing concurrently. You should see the median barely move and the
p99 rise substantially; the maximum will be dominated by `mmap` and page faults. Finally,
replace the allocation with an index into a preallocated `std::vector<Session>` of a
million entries and confirm the distribution flattens to a handful of nanoseconds.
:::

## Takeaways

- `new` is `operator new` plus construction. The allocator's cost is a size-class lookup at
  best, and a lock, a syscall and page faults at worst.
- Median allocation cost of tens of nanoseconds is irrelevant. The tail of tens of
  microseconds is what breaks a latency budget, and it clusters during market bursts.
- `std::unique_ptr` with a stateless deleter is the same size and speed as a raw pointer,
  and it cannot leak. Always create it with `std::make_unique`.
- `std::shared_ptr` is 16 bytes plus a control block, and copying it is an atomic RMW that
  costs 50-100 ns when contended across cores. Use it for configuration and cold objects,
  never in a loop.
- Default to values and references. Reach for owning pointers only when lifetime genuinely
  outlives a scope, and for hot data prefer an index handle into an arena.
