// Wait-free single-producer single-consumer queue.
// Capacity must be a power of two so the wrap is a mask, not a division.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace hft {

inline constexpr std::size_t kCacheLine = 64;

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "hot-path payloads stay trivially copyable");

public:
    // Producer side only.
    bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - cached_tail_ > Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next - cached_tail_ > Capacity) return false;
        }
        slots_[head & kMask] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only.
    bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) return false;
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // The two indices live on separate cache lines, and each side keeps a private
    // copy of the other's index so the common case never loads a contended line.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::size_t cached_tail_ = 0;

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::size_t cached_head_ = 0;

    alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace hft
