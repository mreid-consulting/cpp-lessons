// Four measurements that motivate Part III: contiguous vs pointer-chasing,
// array-of-structs vs struct-of-arrays, and false sharing between threads.
//
// Build: make cache_effects && ./cache_effects

#include "bench.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kOrders = 1u << 20;   // ~1M orders, far larger than L2
constexpr int kReps = 20;

// ---------------------------------------------------------------- layouts

struct OrderAoS {
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint32_t venue_id;
    std::uint64_t exchange_id;   // cold: only used for cancels
    std::uint64_t recv_ns;       // cold: only used for reporting
    char          symbol[16];    // cold
};
static_assert(sizeof(OrderAoS) == 48);

struct OrdersSoA {
    std::vector<std::int64_t>  price_ticks;
    std::vector<std::uint32_t> qty;
    explicit OrdersSoA(std::size_t n) : price_ticks(n), qty(n) {}
};

// ------------------------------------------------------- pointer chasing

struct Node {
    Node* next;
    std::int64_t price_ticks;
    std::uint32_t qty;
};

double run_aos(const std::vector<OrderAoS>& v) {
    const std::uint64_t t0 = bench::now_ns();
    std::int64_t notional = 0;
    for (int r = 0; r < kReps; ++r) {
        for (const auto& o : v) notional += o.price_ticks * o.qty;
        bench::clobber();
    }
    bench::keep(notional);
    return double(bench::now_ns() - t0) / double(kReps * v.size());
}

double run_soa(const OrdersSoA& s) {
    const std::uint64_t t0 = bench::now_ns();
    std::int64_t notional = 0;
    for (int r = 0; r < kReps; ++r) {
        for (std::size_t i = 0; i < s.qty.size(); ++i)
            notional += s.price_ticks[i] * s.qty[i];
        bench::clobber();
    }
    bench::keep(notional);
    return double(bench::now_ns() - t0) / double(kReps * s.qty.size());
}

double run_list(Node* head, std::size_t n) {
    const std::uint64_t t0 = bench::now_ns();
    std::int64_t notional = 0;
    for (int r = 0; r < kReps; ++r) {
        for (Node* p = head; p; p = p->next) notional += p->price_ticks * p->qty;
        bench::clobber();
    }
    bench::keep(notional);
    return double(bench::now_ns() - t0) / double(kReps * n);
}

// ---------------------------------------------------------- false sharing

struct Shared {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};        // same cache line as a
};

struct Padded {
    alignas(64) std::atomic<std::uint64_t> a{0};
    alignas(64) std::atomic<std::uint64_t> b{0};
};

template <typename T>
double run_counters(std::uint64_t iters) {
    T s;
    const std::uint64_t t0 = bench::now_ns();
    std::thread t1([&] {
        for (std::uint64_t i = 0; i < iters; ++i)
            s.a.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread t2([&] {
        for (std::uint64_t i = 0; i < iters; ++i)
            s.b.fetch_add(1, std::memory_order_relaxed);
    });
    t1.join();
    t2.join();
    return double(bench::now_ns() - t0) / double(iters);
}

}  // namespace

int main() {
    bench::Rng rng;

    std::vector<OrderAoS> aos(kOrders);
    OrdersSoA soa(kOrders);
    for (std::size_t i = 0; i < kOrders; ++i) {
        const auto price = static_cast<std::int64_t>(100'000 + rng.below(2000));
        const auto qty = rng.below(500) + 1;
        aos[i].price_ticks = price;
        aos[i].qty = qty;
        soa.price_ticks[i] = price;
        soa.qty[i] = qty;
    }

    // Nodes allocated in shuffled order so the prefetcher cannot help.
    std::vector<std::unique_ptr<Node>> storage(kOrders);
    std::vector<std::size_t> order(kOrders);
    std::iota(order.begin(), order.end(), 0);
    for (std::size_t i = kOrders - 1; i > 0; --i)
        std::swap(order[i], order[rng.below(static_cast<std::uint32_t>(i + 1))]);
    for (std::size_t i = 0; i < kOrders; ++i) {
        storage[i] = std::make_unique<Node>();
        storage[i]->price_ticks = aos[i].price_ticks;
        storage[i]->qty = aos[i].qty;
    }
    for (std::size_t i = 0; i + 1 < kOrders; ++i)
        storage[order[i]]->next = storage[order[i + 1]].get();
    storage[order[kOrders - 1]]->next = nullptr;
    Node* head = storage[order[0]].get();

    std::printf("orders: %zu   AoS element: %zu bytes   SoA hot bytes/elem: %zu\n\n",
                kOrders, sizeof(OrderAoS),
                sizeof(std::int64_t) + sizeof(std::uint32_t));

    std::printf("%-34s %8.3f ns/element\n", "array of structs (48 B stride)", run_aos(aos));
    std::printf("%-34s %8.3f ns/element\n", "struct of arrays (12 B hot)", run_soa(soa));
    std::printf("%-34s %8.3f ns/element\n", "linked list (shuffled nodes)", run_list(head, kOrders));

    constexpr std::uint64_t kIters = 2'000'000;
    std::printf("\n%-34s %8.3f ns/increment\n", "two counters, one cache line",
                run_counters<Shared>(kIters));
    std::printf("%-34s %8.3f ns/increment\n", "two counters, 64 B apart",
                run_counters<Padded>(kIters));
    return 0;
}
