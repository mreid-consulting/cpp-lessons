// Tick-to-trade skeleton: receive, decode, update, decide, risk-check, send.
// One pinned thread does all of it, allocation free, with a cold logging thread
// draining a wait-free queue.
//
// Build: make && ./trader

#include "../bench.hpp"
#include "book.hpp"
#include "decoder.hpp"
#include "feed.hpp"
#include "risk.hpp"
#include "spsc.hpp"
#include "strategy.hpp"
#include "types.hpp"

#include <algorithm>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <thread>

namespace {

using namespace hft;

constexpr Price kBase{100'000};
constexpr std::size_t kMessages = 2'000'000;
constexpr std::size_t kBufferBytes = kMessages * sizeof(WireAdd);
constexpr std::size_t kWarmupMessages = 50'000;

// Binary log records, not formatted strings. Formatting happens on the cold side.
struct LogRecord {
    std::uint64_t seq;
    std::int64_t  price_ticks;
    std::uint32_t qty;
    std::uint8_t  side;
    std::uint8_t  risk;
};

SpscQueue<LogRecord, 8192> g_log;
std::atomic<bool> g_logging{true};

// Stands in for the order gateway. Writing to a volatile sink keeps the
// optimiser from deleting the work the whole program exists to do.
volatile std::uint64_t g_sink = 0;

void send_order(const OutboundOrder& o) noexcept {
    g_sink += static_cast<std::uint64_t>(o.price.v) + o.qty;
}

// Stands in for fill notifications from the exchange. A real system consumes an
// execution report on a separate session; this exists only so the inventory and
// position-limit paths are exercised rather than dead.
class FillSimulator {
public:
    [[nodiscard]] bool fills(std::uint64_t quote_number) const noexcept {
        return (quote_number & 63) == 0;
    }
};

void logging_thread() {
    LogRecord r{};
    std::uint64_t drained = 0;
    while (g_logging.load(std::memory_order_acquire) || g_log.size_approx() != 0) {
        if (g_log.try_pop(r)) ++drained;
    }
    std::printf("logger drained %llu records\n", static_cast<unsigned long long>(drained));
}

struct Stats {
    std::uint64_t decoded = 0;
    std::uint64_t quotes = 0;
    std::uint64_t rejected = 0;
    std::uint64_t fills = 0;
    std::uint64_t dropped_logs = 0;
    std::int64_t  peak_abs_position = 0;
};

// The hot path. Everything it touches was allocated before this function ran.
template <bool Measure>
Stats run(std::span<const std::byte> feed, Book& book, Strategy& strategy,
          RiskGate& risk, SequenceGuard& seq, bench::Histogram<8192, 4>& hist) {
    Stats st{};
    FillSimulator fills;
    std::uint64_t quote_number = 0;
    std::size_t off = 0;

    while (off + sizeof(WireHeader) <= feed.size()) {
        const std::byte* p = feed.data() + off;
        const DecodedHeader h = decode_header(p);
        if (h.length == 0 || off + h.length > feed.size()) [[unlikely]] break;

        const std::uint64_t t0 = Measure ? bench::now_ns() : 0;

        seq.accept(h.seq);

        switch (h.type) {
        case MsgType::Add:     book.on_add(decode_add(p)); break;
        case MsgType::Cancel:  { const auto c = decode_cancel(p);
                                 book.on_reduce(c.id, c.qty); } break;
        case MsgType::Execute: { const auto e = decode_execute(p);
                                 book.on_reduce(e.id, e.qty); } break;
        default: [[unlikely]] break;
        }
        ++st.decoded;

        const OutboundOrder order = strategy.on_book(book);
        if (order.valid) {
            const Price reference = order.side == Side::Buy ? book.best_bid() : book.best_ask();
            const RiskResult verdict =
                risk.check(order, strategy.position(), reference, t0);
            if (verdict == RiskResult::Accept) {
                send_order(order);
                ++st.quotes;
                if (fills.fills(quote_number++)) {
                    strategy.on_fill(order.side, order.qty);
                    ++st.fills;
                    const std::int64_t pos = strategy.position();
                    const std::int64_t abs_pos = pos < 0 ? -pos : pos;
                    if (abs_pos > st.peak_abs_position) st.peak_abs_position = abs_pos;
                }
            } else {
                ++st.rejected;
            }
            if (!g_log.try_push(LogRecord{h.seq, order.price.v, order.qty,
                                          static_cast<std::uint8_t>(order.side),
                                          static_cast<std::uint8_t>(verdict)}))
                ++st.dropped_logs;   // never block the hot path on the logger
        }

        if constexpr (Measure) hist.record(bench::now_ns() - t0);
        off += h.length;
    }
    return st;
}

}  // namespace

int main() {
    // ---- startup: every allocation in the program happens here ----
    auto buffer = std::make_unique<std::byte[]>(kBufferBytes);
    auto book = std::make_unique<Book>(kBase);
    auto hist = std::make_unique<bench::Histogram<8192, 4>>();

    FeedGenerator gen(kBase, 0x1234'5678'9ABC'DEF0ULL);
    const std::size_t bytes = gen.generate(std::span(buffer.get(), kBufferBytes), kMessages);
    std::printf("generated %zu bytes of feed\n", bytes);

    Strategy strategy(StrategyConfig{});
    RiskGate risk(RiskLimits{});
    SequenceGuard seq;

    std::thread logger(logging_thread);

    // ---- warmup: touch every page and train the branch predictors ----
    {
        Book warm_book(kBase);
        Strategy warm_strategy(StrategyConfig{});
        RiskGate warm_risk(RiskLimits{});
        SequenceGuard warm_seq;
        bench::Histogram<8192, 4> warm_hist;
        const std::size_t warm_bytes = std::min(bytes, kWarmupMessages * sizeof(WireAdd));
        run<false>(std::span(buffer.get(), warm_bytes), warm_book, warm_strategy,
                   warm_risk, warm_seq, warm_hist);
        run<true>(std::span(buffer.get(), warm_bytes), warm_book, warm_strategy,
                  warm_risk, warm_seq, warm_hist);
    }

    // ---- pass 1: throughput, with no instrumentation in the loop at all ----
    {
        Book t_book(kBase);
        Strategy t_strategy{StrategyConfig{}};
        RiskGate t_risk{RiskLimits{}};
        SequenceGuard t_seq;
        const std::uint64_t tt0 = bench::now_ns();
        const Stats ts = run<false>(std::span(buffer.get(), bytes), t_book, t_strategy,
                                    t_risk, t_seq, *hist);
        const double s_elapsed = double(bench::now_ns() - tt0) / 1e9;
        std::printf("uninstrumented: %llu messages in %.3f s, %.2f M msg/s, %.1f ns/msg\n",
                    static_cast<unsigned long long>(ts.decoded), s_elapsed,
                    double(ts.decoded) / s_elapsed / 1e6,
                    s_elapsed * 1e9 / double(ts.decoded));
    }

    // ---- pass 2: per-message distribution, two clock reads per message ----
    const std::uint64_t t0 = bench::now_ns();
    const Stats st = run<true>(std::span(buffer.get(), bytes), *book, strategy, risk, seq,
                               *hist);
    const double elapsed_s = double(bench::now_ns() - t0) / 1e9;

    g_logging.store(false, std::memory_order_release);
    logger.join();

    std::printf("\nmessages decoded : %llu in %.3f s (%.2f M msg/s)\n",
                static_cast<unsigned long long>(st.decoded), elapsed_s,
                double(st.decoded) / elapsed_s / 1e6);
    std::printf("quotes sent      : %llu\n", static_cast<unsigned long long>(st.quotes));
    std::printf("risk rejections  : %llu\n", static_cast<unsigned long long>(st.rejected));
    std::printf("fills simulated  : %llu, peak |position| %lld\n",
                static_cast<unsigned long long>(st.fills),
                static_cast<long long>(st.peak_abs_position));
    std::printf("log records lost : %llu\n", static_cast<unsigned long long>(st.dropped_logs));
    std::printf("sequence gaps    : %llu, duplicates %llu\n",
                static_cast<unsigned long long>(seq.gaps()),
                static_cast<unsigned long long>(seq.duplicates()));
    std::printf("live orders left : %u\n", book->live_orders());
    std::printf("final position   : %lld\n", static_cast<long long>(strategy.position()));
    std::printf("\nper-message, decode through send. Each sample carries two\n"
                "steady_clock reads, which on many machines cost more than the work:\n");
    hist->report("  tick to trade");
    std::printf("\nThe max is scheduler noise: this thread is not pinned and the machine\n"
                "is not tuned. Lesson 30 is about making that number small and boring.\n");
    std::printf("\nsink %llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
