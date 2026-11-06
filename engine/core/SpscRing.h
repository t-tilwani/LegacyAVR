// LegacyAVR - wait-free single-producer / single-consumer ring buffer.
//
//  * Storage is allocated once (allocate()) on a non-real-time thread.
//  * write()/read() never allocate, never lock and never block.
//  * Indices are monotonically increasing 64-bit counters (no ABA, no wrap
//    ambiguity); capacity is a power of two so masking replaces modulo.
//  * Producer and consumer indices live on separate cache lines to avoid
//    false sharing between the capture / DSP / render threads.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace avr {

inline constexpr size_t kCacheLine = 64;

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing requires trivially copyable T");

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Not real-time safe. Call while neither side is running.
    void allocate(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        buffer_ = std::make_unique<T[]>(cap);
        capacity_ = cap;
        mask_ = cap - 1;
        reset();
    }

    // Both sides must be idle.
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] size_t readAvailable() const noexcept {        // consumer side
        return static_cast<size_t>(head_.load(std::memory_order_acquire) -
                                   tail_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] size_t writeAvailable() const noexcept {       // producer side
        return capacity_ - static_cast<size_t>(head_.load(std::memory_order_relaxed) -
                                               tail_.load(std::memory_order_acquire));
    }
    [[nodiscard]] size_t size() const noexcept {                 // any thread (telemetry)
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return h >= t ? static_cast<size_t>(h - t) : 0;
    }

    // Producer: copies up to n elements, returns number written.
    size_t write(const T* src, size_t n) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        n = std::min(n, capacity_ - static_cast<size_t>(head - tail));
        if (n == 0) return 0;
        const size_t idx = static_cast<size_t>(head) & mask_;
        const size_t first = std::min(n, capacity_ - idx);
        std::memcpy(buffer_.get() + idx, src, first * sizeof(T));
        if (n > first) std::memcpy(buffer_.get(), src + first, (n - first) * sizeof(T));
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Producer: writes n copies of value (silence fill).
    size_t fill(const T& value, size_t n) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        n = std::min(n, capacity_ - static_cast<size_t>(head - tail));
        for (size_t i = 0; i < n; ++i) buffer_[(static_cast<size_t>(head) + i) & mask_] = value;
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer: copies up to n elements, returns number read.
    size_t read(T* dst, size_t n) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(head - tail));
        if (n == 0) return 0;
        const size_t idx = static_cast<size_t>(tail) & mask_;
        const size_t first = std::min(n, capacity_ - idx);
        std::memcpy(dst, buffer_.get() + idx, first * sizeof(T));
        if (n > first) std::memcpy(dst + first, buffer_.get(), (n - first) * sizeof(T));
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Consumer: drops up to n elements without copying.
    size_t discard(size_t n) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(head - tail));
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

private:
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};   // written by producer only
    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};   // written by consumer only
    alignas(kCacheLine) std::unique_ptr<T[]> buffer_;
    size_t capacity_ = 0;
    size_t mask_ = 0;
};

} // namespace avr
