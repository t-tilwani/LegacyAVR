// LegacyAVR - level meters. DSP thread writes, telemetry thread reads (relaxed atomics).
//  peak : block peak with 20 dB/s ballistic fall
//  rms  : 300 ms exponential RMS (VU-like integration)
#pragma once

#include "Types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace avr {

template <size_t N>
class MeterBank {
public:
    void reset() noexcept {
        for (size_t c = 0; c < N; ++c) {
            peak_[c] = 0; ms_[c] = 0;
            peakOut_[c].store(0, std::memory_order_relaxed);
            rmsOut_[c].store(0, std::memory_order_relaxed);
        }
    }

    // Planar input at kBusRate.
    void analyse(const float* const* planes, size_t channels, size_t frames) noexcept {
        const float blockSec = float(frames) / float(kBusRate);
        const float fall = std::pow(10.0f, -20.0f * blockSec / 20.0f);   // 20 dB/s
        const float rmsCoef = 1.0f - std::exp(-blockSec / 0.300f);
        for (size_t c = 0; c < N; ++c) {
            float pk = 0.0f, acc = 0.0f;
            if (c < channels && frames > 0) {
                const float* p = planes[c];
                for (size_t i = 0; i < frames; ++i) {
                    const float v = p[i];
                    pk = std::max(pk, std::fabs(v));
                    acc += v * v;
                }
                acc /= float(frames);
            }
            publish(c, pk, acc, fall, rmsCoef);
        }
    }

    // Interleaved input variant (input meters, before routing).
    void analyseInterleaved(const float* data, size_t channels, size_t frames, uint32_t rate) noexcept {
        const float blockSec = float(frames) / float(rate ? rate : kBusRate);
        const float fall = std::pow(10.0f, -20.0f * blockSec / 20.0f);
        const float rmsCoef = 1.0f - std::exp(-blockSec / 0.300f);
        std::array<float, N> pk{}, acc{};
        const size_t ch = std::min(channels, N);
        for (size_t i = 0; i < frames; ++i) {
            const float* f = data + i * channels;
            for (size_t c = 0; c < ch; ++c) {
                pk[c] = std::max(pk[c], std::fabs(f[c]));
                acc[c] += f[c] * f[c];
            }
        }
        for (size_t c = 0; c < N; ++c)
            publish(c, pk[c], frames ? acc[c] / float(frames) : 0.0f, fall, rmsCoef);
    }

    [[nodiscard]] float peak(size_t c) const noexcept { return peakOut_[c].load(std::memory_order_relaxed); }
    [[nodiscard]] float rms(size_t c) const noexcept { return rmsOut_[c].load(std::memory_order_relaxed); }

private:
    void publish(size_t c, float pk, float meanSq, float fall, float rmsCoef) noexcept {
        peak_[c] = std::max(pk, peak_[c] * fall);
        ms_[c] += rmsCoef * (meanSq - ms_[c]);
        peakOut_[c].store(peak_[c], std::memory_order_relaxed);
        rmsOut_[c].store(std::sqrt(ms_[c]), std::memory_order_relaxed);
    }

    std::array<float, N> peak_{}, ms_{};
    std::array<std::atomic<float>, N> peakOut_{}, rmsOut_{};
};

} // namespace avr
