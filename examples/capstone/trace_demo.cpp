// Lesson 28a's exercise, done: trace five stage boundaries of the capstone
// pipeline, then ask which stage was slow on the worst messages.
//
// This is the workflow a sampling profiler cannot perform. The slow messages
// here are roughly one in a hundred thousand; a 4 kHz profiler would take one
// sample per 250 us and never attribute a single one of them.
//
// Build: make trace_demo && ./trace_demo
//        make trace_demo TRACE=0   # same binary with the tracer compiled out

#include "../bench.hpp"
#include "../trace_ring.hpp"
#include "book.hpp"
#include "decoder.hpp"
#include "feed.hpp"
#include "risk.hpp"
#include "strategy.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace hft;

constexpr Price kBase{100'000};
constexpr std::size_t kMessages = 400'000;
constexpr std::size_t kBufferBytes = kMessages * sizeof(WireAdd);
constexpr std::size_t kRingSlots = 1u << 21;   // 2M records, 64 MB

Ring<kRingSlots> g_trace;

// One message's journey, reassembled from its trace records.
struct Journey {
    std::uint64_t seq = 0;
    std::uint64_t total = 0;
    std::uint64_t decode = 0;
    std::uint64_t book = 0;
    std::uint64_t strat = 0;
    std::uint64_t send = 0;
    bool complete = false;
};

volatile std::uint64_t g_sink = 0;

}  // namespace

int main() {
    auto buffer = std::make_unique<std::byte[]>(kBufferBytes);
    auto book = std::make_unique<Book>(kBase);

    FeedGenerator gen(kBase, 0xC0FFEE'1234'5678ULL);
    const std::size_t bytes = gen.generate(std::span(buffer.get(), kBufferBytes), kMessages);

    Strategy strategy{StrategyConfig{}};
    RiskGate risk{RiskLimits{}};
    SequenceGuard seq_guard;

    const double hz = tsc_hz();
    std::printf("tracer: %s, %zu slots, counter %.3f MHz\n",
                HFT_TRACE ? "on" : "compiled out", decltype(g_trace)::capacity(), hz / 1e6);
    if (HFT_TRACE && hz < 100e6) {
        std::printf("NOTE: this counter ticks every %.1f ns, which is coarser than a\n"
                    "      single pipeline stage. Per-stage figures below quantise to\n"
                    "      multiples of that, and only the large outliers are real.\n"
                    "      On x86-64 rdtsc runs at the nominal core frequency and the\n"
                    "      same code resolves sub-nanosecond.\n", 1e9 / hz);
    }

    // ---- the hot loop, with five trace points ----
    const std::uint64_t wall0 = bench::now_ns();
    std::size_t off = 0;
    std::uint64_t processed = 0;

    while (off + sizeof(WireHeader) <= bytes) {
        const std::byte* p = buffer.get() + off;
        const DecodedHeader h = decode_header(p);
        if (h.length == 0 || off + h.length > bytes) break;

        g_trace.put(Ev::wire_rx, h.seq);

        seq_guard.accept(h.seq);
        switch (h.type) {
        case MsgType::Add:     book->on_add(decode_add(p)); break;
        case MsgType::Cancel:  { const auto c = decode_cancel(p);
                                 book->on_reduce(c.id, c.qty); } break;
        case MsgType::Execute: { const auto e = decode_execute(p);
                                 book->on_reduce(e.id, e.qty); } break;
        default: break;
        }
        g_trace.put(Ev::decoded, h.seq);
        g_trace.put(Ev::book_applied, h.seq, book->live_orders());

        const OutboundOrder order = strategy.on_book(*book);
        g_trace.put(Ev::signal, h.seq, order.valid ? 1u : 0u);

        if (order.valid) {
            const Price ref = order.side == Side::Buy ? book->best_bid() : book->best_ask();
            if (risk.check(order, strategy.position(), ref, wall0) == RiskResult::Accept)
                g_sink += static_cast<std::uint64_t>(order.price.v);
        }
        g_trace.put(Ev::order_sent, h.seq);

        ++processed;
        off += h.length;
    }
    const double elapsed_s = double(bench::now_ns() - wall0) / 1e9;

    std::printf("processed %llu messages in %.3f s (%.1f ns/msg, tracer included)\n\n",
                static_cast<unsigned long long>(processed), elapsed_s,
                elapsed_s * 1e9 / double(processed));

    if (!HFT_TRACE) {
        std::printf("Tracer compiled out. Compare the ns/msg above with the traced run:\n"
                    "the difference is the price of always-on observability.\n");
        std::printf("sink %llu\n", static_cast<unsigned long long>(g_sink));
        return 0;
    }

    // ---- offline decode: everything below here is the cold path ----
    std::vector<TraceRec> recs(decltype(g_trace)::capacity());
    const std::size_t n = g_trace.snapshot(recs.data(), recs.size());
    std::printf("drained %zu of %llu records written\n", n,
                static_cast<unsigned long long>(g_trace.written()));

    // Records arrive in order, five per message. Reassemble journeys.
    std::vector<Journey> journeys;
    journeys.reserve(n / 5 + 1);
    Journey cur;
    std::uint64_t t_rx = 0, t_dec = 0, t_book = 0, t_sig = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const TraceRec& r = recs[i];
        switch (static_cast<Ev>(r.ev)) {
        case Ev::wire_rx:      cur = Journey{}; cur.seq = r.a; t_rx = r.tsc; break;
        case Ev::decoded:      t_dec = r.tsc; break;
        case Ev::book_applied: t_book = r.tsc; break;
        case Ev::signal:       t_sig = r.tsc; break;
        case Ev::order_sent:
            if (t_rx && t_dec >= t_rx) {
                cur.decode = t_dec - t_rx;
                cur.book = t_book - t_dec;
                cur.strat = t_sig - t_book;
                cur.send = r.tsc - t_sig;
                cur.total = r.tsc - t_rx;
                cur.complete = true;
                journeys.push_back(cur);
            }
            t_rx = 0;
            break;
        default: break;
        }
    }

    if (journeys.size() < 100) {
        std::printf("too few complete journeys to analyse\n");
        return 1;
    }

    const double ns_per_tick = 1e9 / hz;
    auto to_ns = [&](std::uint64_t ticks) { return double(ticks) * ns_per_tick; };

    std::vector<Journey> sorted = journeys;
    std::sort(sorted.begin(), sorted.end(),
              [](const Journey& a, const Journey& b) { return a.total < b.total; });

    const Journey& med = sorted[sorted.size() / 2];
    const Journey& p999 = sorted[sorted.size() * 999 / 1000];

    std::printf("\n%zu complete journeys\n\n", journeys.size());
    std::printf("%-12s %10s %10s %10s %10s %10s\n",
                "", "decode", "book", "strategy", "send", "TOTAL");
    auto row = [&](const char* label, const Journey& j) {
        std::printf("%-12s %9.1f %9.1f %9.1f %9.1f %9.1f  (ns)\n", label,
                    to_ns(j.decode), to_ns(j.book), to_ns(j.strat), to_ns(j.send),
                    to_ns(j.total));
    };
    row("median", med);
    row("p99.9", p999);

    std::printf("\nthe ten slowest messages, and where the time went:\n");
    std::printf("%-12s %10s %10s %10s %10s %10s %s\n",
                "seq", "decode", "book", "strategy", "send", "TOTAL", "worst stage");
    for (std::size_t k = 0; k < 10; ++k) {
        const Journey& j = sorted[sorted.size() - 1 - k];
        const std::uint64_t stages[4] = {j.decode, j.book, j.strat, j.send};
        const char* names[4] = {"decode", "book", "strategy", "send"};
        const std::size_t worst =
            static_cast<std::size_t>(std::max_element(stages, stages + 4) - stages);
        std::printf("%-12llu %9.1f %9.1f %9.1f %9.1f %9.1f  %s\n",
                    static_cast<unsigned long long>(j.seq),
                    to_ns(j.decode), to_ns(j.book), to_ns(j.strat), to_ns(j.send),
                    to_ns(j.total), names[worst]);
    }

    std::printf("\nRead the 'worst stage' column. If it is the same stage every time you\n"
                "have a code problem; if it varies, you have a machine problem, and\n"
                "lesson 30 is where you go next. Neither answer is available from a\n"
                "sampling profile, because none of these messages appears in one.\n");
    std::printf("\nsink %llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
