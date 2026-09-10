// Fixed-capacity vector with no dynamic allocation.
// Objects are constructed in place; capacity is a compile-time constant.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hft {

template <typename T, std::size_t N>
class StaticVector {
    static_assert(N > 0, "capacity must be positive");

public:
    using value_type = T;
    using size_type = std::conditional_t<(N <= 255), std::uint8_t,
                      std::conditional_t<(N <= 65535), std::uint16_t, std::uint32_t>>;

    StaticVector() = default;

    StaticVector(std::initializer_list<T> init) {
        for (const T& v : init) push_back(v);
    }

    StaticVector(const StaticVector& other) {
        for (size_type i = 0; i < other.size_; ++i) push_back(other[i]);
    }

    StaticVector& operator=(const StaticVector& other) {
        if (this != &other) {
            clear();
            for (size_type i = 0; i < other.size_; ++i) push_back(other[i]);
        }
        return *this;
    }

    ~StaticVector() { clear(); }

    // Precondition: size() < capacity(). Returns false instead of growing.
    bool push_back(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        if (size_ == N) return false;
        std::construct_at(data() + size_, value);
        ++size_;
        return true;
    }

    template <typename... Args>
    T* emplace_back(Args&&... args) {
        if (size_ == N) return nullptr;
        T* p = std::construct_at(data() + size_, std::forward<Args>(args)...);
        ++size_;
        return p;
    }

    void pop_back() noexcept {
        --size_;
        std::destroy_at(data() + size_);
    }

    // Order-destroying erase: O(1) because element order is not an invariant here.
    void erase_unordered(size_type index) noexcept {
        data()[index] = std::move(data()[size_ - 1]);
        pop_back();
    }

    void clear() noexcept {
        std::destroy_n(data(), size_);
        size_ = 0;
    }

    [[nodiscard]] T* data() noexcept { return reinterpret_cast<T*>(storage_); }
    [[nodiscard]] const T* data() const noexcept { return reinterpret_cast<const T*>(storage_); }

    [[nodiscard]] T& operator[](size_type i) noexcept { return data()[i]; }
    [[nodiscard]] const T& operator[](size_type i) const noexcept { return data()[i]; }

    [[nodiscard]] T& front() noexcept { return data()[0]; }
    [[nodiscard]] T& back() noexcept { return data()[size_ - 1]; }

    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == N; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return N; }

private:
    alignas(T) std::byte storage_[sizeof(T) * N];
    size_type size_ = 0;
};

// Bump allocator. Hands out aligned bytes and reclaims them all at once.
class Arena {
public:
    Arena(std::byte* buffer, std::size_t bytes) noexcept
        : begin_(buffer), end_(buffer + bytes), cursor_(buffer) {}

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* p = allocate(sizeof(T), alignof(T));
        return p ? std::construct_at(static_cast<T*>(p), std::forward<Args>(args)...) : nullptr;
    }

    // Returns nullptr when exhausted. Never calls the global allocator.
    void* allocate(std::size_t bytes, std::size_t align) noexcept {
        auto addr = reinterpret_cast<std::uintptr_t>(cursor_);
        const std::uintptr_t aligned = (addr + align - 1) & ~(std::uintptr_t{align} - 1);
        auto* next = reinterpret_cast<std::byte*>(aligned) + bytes;
        if (next > end_) return nullptr;
        cursor_ = next;
        return reinterpret_cast<void*>(aligned);
    }

    // Valid only for trivially destructible contents.
    void reset() noexcept { cursor_ = begin_; }

    [[nodiscard]] std::size_t used() const noexcept {
        return static_cast<std::size_t>(cursor_ - begin_);
    }

private:
    std::byte* begin_;
    std::byte* end_;
    std::byte* cursor_;
};

// Slab of fixed-size objects addressed by 32-bit handles.
// Handles stay valid for the lifetime of the pool and are half the size of a pointer.
template <typename T, std::size_t N>
class ObjectPool {
public:
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    ObjectPool() {
        for (std::uint32_t i = 0; i < N - 1; ++i) next_free_[i] = i + 1;
        next_free_[N - 1] = kInvalid;
    }

    template <typename... Args>
    std::uint32_t acquire(Args&&... args) {
        if (free_head_ == kInvalid) return kInvalid;
        const std::uint32_t handle = free_head_;
        free_head_ = next_free_[handle];
        std::construct_at(slot(handle), std::forward<Args>(args)...);
        ++live_;
        return handle;
    }

    void release(std::uint32_t handle) noexcept {
        std::destroy_at(slot(handle));
        next_free_[handle] = free_head_;
        free_head_ = handle;
        --live_;
    }

    [[nodiscard]] T& operator[](std::uint32_t handle) noexcept { return *slot(handle); }
    [[nodiscard]] std::uint32_t live() const noexcept { return live_; }

private:
    T* slot(std::uint32_t i) noexcept { return reinterpret_cast<T*>(storage_) + i; }

    alignas(T) std::byte storage_[sizeof(T) * N];
    std::uint32_t next_free_[N]{};
    std::uint32_t free_head_ = 0;
    std::uint32_t live_ = 0;
};

}  // namespace hft
