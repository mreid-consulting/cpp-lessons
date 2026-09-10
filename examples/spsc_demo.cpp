// Two measurements of the SPSC queue:
//   1. Handoff latency, via a ping-pong so the queue is never backed up.
//   2. Streaming throughput, with the producer running flat out.
//
// Build: make spsc_demo && ./spsc_demo

#include "bench.hpp"
#include "spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

struct Message {
    std::uint64_t seq;
    std::uint64_t sent_ns;
    std::int64_t  price_ticks;
    std::uint32_t qty;
};
static_assert(sizeof(Message) == 32);

constexpr std::uint64_t kPingPongs = 200'000;
constexpr std::uint64_t kStreamed = 5'000'000;
constexpr std::uint64_t kWarmup = 20'000;

hft::SpscQueue<Message, 1024> to_consumer;
hft::SpscQueue<Message, 1024> to_producer;
std::atomic<bool> consumer_ready{false};

// Round trip through both queues. Halving it gives the one-way handoff cost,
// which is the number that matters when a strategy thread hands an order to a
// sender thread.
void ping_pong() {
    bench::Histogram<16384, 4> hist;

    std::thread consumer([] {
        Message m{};
        consumer_ready.store(true, std::memory_order_release);
        for (std::uint64_t i = 0; i < kPingPongs + kWarmup; ++i) {
            while (!to_consumer.try_pop(m)) { /* spin: never block the hot path */ }
            while (!to_producer.try_push(m)) { /* spin */ }
        }
    });

    while (!consumer_ready.load(std::memory_order_acquire)) { /* wait for the consumer */ }

    Message reply{};
    for (std::uint64_t i = 0; i < kPingPongs + kWarmup; ++i) {
        const std::uint64_t t0 = bench::now_ns();
        Message m{i, t0, 100'000 + static_cast<std::int64_t>(i % 100), 100};
        while (!to_consumer.try_push(m)) { /* spin */ }
        while (!to_producer.try_pop(reply)) { /* spin */ }
        const std::uint64_t elapsed = bench::now_ns() - t0;
        if (i >= kWarmup) hist.record(elapsed / 2);
    }

    consumer.join();
    bench::keep(reply.seq);
    std::printf("one-way handoff, %llu samples after %llu warmup\n",
                static_cast<unsigned long long>(kPingPongs),
                static_cast<unsigned long long>(kWarmup));
    hist.report("  producer -> consumer");
    std::printf("  includes one steady_clock read pair per round trip.\n");
}

hft::SpscQueue<Message, 8192> stream;

void throughput() {
    std::atomic<bool> go{false};
    std::uint64_t consumed = 0;

    std::thread consumer([&] {
        Message m{};
        std::int64_t sink = 0;
        go.store(true, std::memory_order_release);
        while (consumed < kStreamed) {
            if (stream.try_pop(m)) {
                sink += m.price_ticks;
                ++consumed;
            }
        }
        bench::keep(sink);
    });

    while (!go.load(std::memory_order_acquire)) { /* wait */ }

    const std::uint64_t t0 = bench::now_ns();
    for (std::uint64_t i = 0; i < kStreamed; ++i) {
        Message m{i, 0, 100'000 + static_cast<std::int64_t>(i & 63), 100};
        while (!stream.try_push(m)) { /* consumer is behind */ }
    }
    consumer.join();
    const double elapsed_s = double(bench::now_ns() - t0) / 1e9;

    std::printf("\nstreaming %llu messages of %zu bytes\n",
                static_cast<unsigned long long>(kStreamed), sizeof(Message));
    std::printf("  %.1f M msg/s, %.2f ns per message, %.2f GB/s payload\n",
                double(kStreamed) / elapsed_s / 1e6,
                elapsed_s * 1e9 / double(kStreamed),
                double(kStreamed * sizeof(Message)) / elapsed_s / 1e9);
}

}  // namespace

int main() {
    std::printf("SpscQueue<Message, 1024> object size: %zu bytes, all of it preallocated\n\n",
                sizeof(to_consumer));
    ping_pong();
    throughput();
    std::printf("\nNeither number is meaningful unless both threads are pinned to known\n"
                "cores. Unpinned, the scheduler decides whether this is an L2 handoff or a\n"
                "cross-socket one, and the answer changes run to run.\n");
    return 0;
}
