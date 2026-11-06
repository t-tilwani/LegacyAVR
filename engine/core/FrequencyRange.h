// LegacyAVR - live input frequency range (v1.2.0).
//
// Every 1536-sample bus block the latest 4096 samples are analysed with a
// Blackman-Harris window and one complex FFT that carries two real signals
// (left group = FL + SL + (FC + LFE)/2 in the real part, right group in the imaginary
// part), so out-of-phase L/R content cannot cancel. The power spectrum is averaged
// with a 7.5 s time constant; the reported range is the lowest and highest frequency
// whose averaged level is within 50 dB of the strongest band and above -100 dBFS.
//
// DSP thread: analyse()/decay(). Any thread: lowHz()/highHz() (relaxed atomics).
// configure() allocates; everything else is allocation-free.
#pragma once

#include "Types.h"

#include <atomic>
#include <complex>
#include <cstddef>
#include <vector>

namespace avr {

class FrequencyRange {
public:
    static constexpr size_t kN = 4096;                 // 11.7 Hz bins at 48 kHz
    static constexpr float kAverageSeconds = 7.5f;
    static constexpr float kRelativeDb = 50.0f;
    static constexpr float kFloorDbfs = -100.0f;
    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 20000.0f;

    void configure();                                  // allocates tables (not real-time)
    void reset() noexcept;

    // One bus block (6 planes, `frames` <= kN samples).
    void analyse(const float* const* bus, size_t frames) noexcept;
    // Silence / no input for `frames`: lets the average decay so the range fades out.
    void decay(size_t frames) noexcept;

    // 0 when there is not enough signal.
    [[nodiscard]] float lowHz() const noexcept { return low_.load(std::memory_order_relaxed); }
    [[nodiscard]] float highHz() const noexcept { return high_.load(std::memory_order_relaxed); }

    // Testing helper: recompute the range from the current average.
    void updateRange() noexcept;

private:
    void fft(std::complex<float>* x) const noexcept;

    std::vector<float> window_, histL_, histR_, avg_;
    std::vector<std::complex<float>> twiddle_, buf_;
    std::vector<uint32_t> bitrev_;
    size_t histPos_ = 0;
    size_t filled_ = 0;
    std::atomic<float> low_{0.0f}, high_{0.0f};
};

} // namespace avr
