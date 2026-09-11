// Always-on ring tracer: one timestamp and one 32-byte store per trace point.
// Matches the implementation in lesson 28a, with a portable timestamp source.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace hft {

// Raw cycle counter. Never converted to nanoseconds on the hot path: the
// conversion is a multiply and a shift that belongs in the offline decoder.
[[nodiscard]] inline std::uint64_t tsc_raw() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    std::uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    // Virtual count register, readable from EL0. Typically 24 MHz on Apple
    // silicon, so a tick is about 41 ns and stage deltas need care.
    std::uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Ticks per second, measured once at startup.
[[nodiscard]] inline double tsc_hz() noexcept {
    using Clock = std::chrono::steady_clock;
    const auto wall0 = Clock::now();
    const std::uint64_t t0 = tsc_raw();
    while (Clock::now() - wall0 < std::chrono::milliseconds(50)) { /* calibrate */ }
    const std::uint64_t t1 = tsc_raw();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - wall0).count();
    return static_cast<double>(t1 - t0) / seconds;
}

enum class Ev : std::uint32_t {
    none = 0, wire_rx, decoded, book_applied, signal, order_sent
};

inline const char* name(Ev e) noexcept {
    switch (e) {
    case Ev::wire_rx:      return "wire_rx";
    case Ev::decoded:      return "decoded";
    case Ev::book_applied: return "book_applied";
    case Ev::signal:       return "signal";
    case Ev::order_sent:   return "order_sent";
    case Ev::none:         return "none";
    }
    return "?";
}

struct TraceRec {                 // 32 bytes: two per cache line
    std::uint64_t tsc;
    std::uint32_t ev;
    std::uint32_t pad;
    std::uint64_t a;              // sequence number
    std::uint64_t b;              // stage-specific payload
};
static_assert(sizeof(TraceRec) == 32);

template <std::size_t N>
class TraceRing {
    static_assert(std::has_single_bit(N), "N must be a power of two");

public:
    // Hot path: one counter read, one 32-byte store, one release.
    void put(Ev e, std::uint64_t a = 0, std::uint64_t b = 0) noexcept {
        const std::uint64_t i = head_.load(std::memory_order_relaxed);
        TraceRec& r = buf_[i & (N - 1)];
        r.tsc = tsc_raw();
        r.ev = static_cast<std::uint32_t>(e);
        r.a = a;
        r.b = b;
        head_.store(i + 1, std::memory_order_release);
    }

    // Cold thread. Best effort: a record may be overwritten as it is copied.
    std::size_t snapshot(TraceRec* out, std::size_t cap) const noexcept {
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::size_t have = static_cast<std::size_t>(h < N ? h : N);
        const std::size_t take = have < cap ? have : cap;
        for (std::size_t k = 0; k < take; ++k)
            out[k] = buf_[(h - take + k) & (N - 1)];
        return take;
    }

    [[nodiscard]] std::uint64_t written() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return N; }

private:
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::array<TraceRec, N> buf_{};
};

// Compile the tracer out entirely by defining HFT_TRACE=0.
#ifndef HFT_TRACE
#define HFT_TRACE 1
#endif

template <std::size_t N>
struct NullRing {
    void put(Ev, std::uint64_t = 0, std::uint64_t = 0) noexcept {}
    std::size_t snapshot(TraceRec*, std::size_t) const noexcept { return 0; }
    [[nodiscard]] std::uint64_t written() const noexcept { return 0; }
    static constexpr std::size_t capacity() noexcept { return 0; }
};

#if HFT_TRACE
template <std::size_t N> using Ring = TraceRing<N>;
#else
template <std::size_t N> using Ring = NullRing<N>;
#endif

}  // namespace hft
