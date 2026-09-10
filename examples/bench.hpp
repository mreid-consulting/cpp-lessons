// Minimal timing and histogram helpers shared by the examples.
// Portable: uses steady_clock rather than rdtsc so the examples run anywhere.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace bench {

using Clock = std::chrono::steady_clock;

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

// Prevents the optimiser from deleting a computation whose result is unused.
//
// The constraint is input-only with two alternatives: a register if the value
// fits one, memory otherwise. The read-write form `"+r,m"` that appears in many
// benchmark harnesses is x86-64 folklore; GCC rejects it on AArch64, and it
// cannot accept an operand too large for a register at all.
template <typename T>
inline void keep(T&& value) {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    volatile auto sink = value;
    (void)sink;
#endif
}

inline void clobber() {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : : "memory");
#endif
}

// Fixed-bucket latency histogram. Allocates once, at construction.
template <std::size_t Buckets = 4096, std::uint64_t NsPerBucket = 8>
class Histogram {
public:
    void record(std::uint64_t ns) {
        const std::size_t b = std::min<std::size_t>(ns / NsPerBucket, Buckets - 1);
        ++counts_[b];
        ++total_;
        max_ = std::max(max_, ns);
    }

    [[nodiscard]] std::uint64_t percentile(double p) const {
        const std::uint64_t target =
            static_cast<std::uint64_t>(static_cast<double>(total_) * p);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < Buckets; ++i) {
            seen += counts_[i];
            if (seen >= target) return i * NsPerBucket;
        }
        return max_;
    }

    void report(const char* label) const {
        std::printf("%-28s n=%-9llu p50=%-7llu p99=%-7llu p99.9=%-7llu max=%llu  (ns)\n",
                    label,
                    static_cast<unsigned long long>(total_),
                    static_cast<unsigned long long>(percentile(0.50)),
                    static_cast<unsigned long long>(percentile(0.99)),
                    static_cast<unsigned long long>(percentile(0.999)),
                    static_cast<unsigned long long>(max_));
    }

private:
    std::array<std::uint64_t, Buckets> counts_{};
    std::uint64_t total_ = 0;
    std::uint64_t max_ = 0;
};

// Deterministic xorshift; std::mt19937 is far too slow for generating test data.
class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ULL) : s_(seed) {}
    std::uint64_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 7;
        s_ ^= s_ << 17;
        return s_;
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }

private:
    std::uint64_t s_;
};

}  // namespace bench
