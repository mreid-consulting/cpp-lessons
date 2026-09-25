// The loop and its helper in one translation unit, so the helper can be inlined.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

inline std::vector<std::int64_t> px(1 << 20);
inline void fill() { for (std::size_t i = 0; i < px.size(); ++i) px[i] = 100000 + static_cast<std::int64_t>(i % 97); }
inline std::int64_t scaled(std::int64_t p) { return p * 100; }
inline void run() {
    fill();
    const auto t0 = std::chrono::steady_clock::now();
    std::int64_t s = 0;
    for (int r = 0; r < 50; ++r)
        for (auto p : px) s += scaled(p);
    const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%.3f ns/element (checksum %lld)\n", ns / (50.0 * static_cast<double>(px.size())), static_cast<long long>(s));
}
