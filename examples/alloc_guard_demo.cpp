// Demonstrates the allocation-free toolkit and the operator new guard that turns
// an accidental hot-path allocation into an immediate, obvious failure.
//
// Build: make alloc_guard_demo && ./alloc_guard_demo

#include "bench.hpp"
#include "static_vector.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace guard {

// Flipped on once startup is complete. Any allocation after that aborts.
bool g_hot_path_active = false;

struct Scope {
    Scope() { g_hot_path_active = true; }
    ~Scope() { g_hot_path_active = false; }
};

}  // namespace guard

void* operator new(std::size_t n) {
    if (guard::g_hot_path_active) {
        std::fputs("allocation on the hot path\n", stderr);
        std::abort();
    }
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc{};
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

struct Order {
    std::uint64_t exchange_id;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint8_t  side;
};

constexpr std::size_t kMaxLevels = 64;
constexpr std::size_t kPoolSize = 4096;

hft::ObjectPool<Order, kPoolSize> g_orders;
alignas(64) std::byte g_arena_bytes[1 << 16];

}  // namespace

int main() {
    std::printf("StaticVector<Order, %zu> size: %zu bytes (no heap)\n",
                kMaxLevels, sizeof(hft::StaticVector<Order, kMaxLevels>));
    std::printf("ObjectPool<Order, %zu>   size: %zu bytes\n\n",
                kPoolSize, sizeof(g_orders));

    hft::Arena arena(g_arena_bytes, sizeof(g_arena_bytes));
    hft::StaticVector<Order, kMaxLevels> book_side;

    {
        guard::Scope hot;   // from here, any operator new call aborts the process

        for (std::uint32_t i = 0; i < kMaxLevels; ++i)
            book_side.push_back(Order{i, 100'000 + i, 100, 0});

        std::uint32_t handles[256];
        for (std::uint32_t i = 0; i < 256; ++i)
            handles[i] = g_orders.acquire(Order{i, 100'000, 50, 1});
        for (std::uint32_t i = 0; i < 256; i += 2) g_orders.release(handles[i]);

        auto* scratch = static_cast<std::int64_t*>(arena.allocate(1024 * sizeof(std::int64_t),
                                                                 alignof(std::int64_t)));
        for (int i = 0; i < 1024; ++i) scratch[i] = book_side[i % kMaxLevels].price_ticks;
        bench::keep(scratch[0]);

        std::printf("levels: %u   live orders: %u   arena used: %zu bytes\n",
                    static_cast<unsigned>(book_side.size()), g_orders.live(), arena.used());
    }

    // Cost comparison, outside the guarded scope.
    // Each sample times a batch, so the two steady_clock reads (tens of ns) do not
    // dominate the thing being measured.
    constexpr int kBatch = 64;
    constexpr int kSamples = 20'000;
    bench::Histogram<8192, 4> heap_hist, static_hist;

    for (int s = 0; s < kSamples; ++s) {
        const std::uint64_t t0 = bench::now_ns();
        for (int b = 0; b < kBatch; ++b) {
            std::vector<Order> v;
            v.reserve(kMaxLevels);
            for (std::size_t j = 0; j < kMaxLevels; ++j) v.push_back(Order{j, 100'000, 100, 0});
            bench::keep(v.data());
        }
        heap_hist.record((bench::now_ns() - t0) / kBatch);
    }

    for (int s = 0; s < kSamples; ++s) {
        const std::uint64_t t0 = bench::now_ns();
        for (int b = 0; b < kBatch; ++b) {
            hft::StaticVector<Order, kMaxLevels> v;
            for (std::size_t j = 0; j < kMaxLevels; ++j) v.push_back(Order{j, 100'000, 100, 0});
            bench::keep(v.data());
        }
        static_hist.record((bench::now_ns() - t0) / kBatch);
    }

    std::printf("\nfilling %zu orders, mean of %d per sample:\n", kMaxLevels, kBatch);
    heap_hist.report("std::vector + reserve");
    static_hist.report("StaticVector (no heap)");
    std::printf("\nThe median gap is the allocator's fast path. The gap at p99.9 and max is\n"
                "its slow path: a size-class miss, a fresh page, a fault. That tail is what\n"
                "a trading system is graded on, and removing the heap removes it entirely.\n");
    return 0;
}
