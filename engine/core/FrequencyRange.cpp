#include "FrequencyRange.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace avr {

void FrequencyRange::configure() {
    window_.assign(kN, 0.0f);
    // 4-term Blackman-Harris: -92 dB side lobes, so a strong bass note does not smear
    // the measured range over the whole spectrum.
    constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    double wsum = 0;
    for (size_t i = 0; i < kN; ++i) {
        const double x = 2.0 * std::numbers::pi * double(i) / double(kN - 1);
        window_[i] = float(a0 - a1 * std::cos(x) + a2 * std::cos(2 * x) - a3 * std::cos(3 * x));
        wsum += window_[i];
    }
    // Normalise so a full-scale sine reads ~0 dB in its bin.
    const float norm = float(2.0 / wsum);
    for (auto& w : window_) w *= norm;

    twiddle_.resize(kN / 2);
    for (size_t k = 0; k < kN / 2; ++k) {
        const double a = -2.0 * std::numbers::pi * double(k) / double(kN);
        twiddle_[k] = {float(std::cos(a)), float(std::sin(a))};
    }
    bitrev_.resize(kN);
    size_t bits = 0;
    while ((size_t(1) << bits) < kN) ++bits;
    for (size_t i = 0; i < kN; ++i) {
        size_t r = 0;
        for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
        bitrev_[i] = uint32_t(r);
    }
    histL_.assign(kN, 0.0f);
    histR_.assign(kN, 0.0f);
    buf_.assign(kN, {0.0f, 0.0f});
    avg_.assign(kN / 2 + 1, 0.0f);
    reset();
}

void FrequencyRange::reset() noexcept {
    std::fill(histL_.begin(), histL_.end(), 0.0f);
    std::fill(histR_.begin(), histR_.end(), 0.0f);
    std::fill(avg_.begin(), avg_.end(), 0.0f);
    histPos_ = 0;
    filled_ = 0;
    low_.store(0.0f, std::memory_order_relaxed);
    high_.store(0.0f, std::memory_order_relaxed);
}

void FrequencyRange::fft(std::complex<float>* x) const noexcept {
    for (size_t i = 0; i < kN; ++i) {
        const size_t j = bitrev_[i];
        if (j > i) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= kN; len <<= 1) {
        const size_t half = len / 2;
        const size_t stride = kN / len;
        for (size_t i = 0; i < kN; i += len) {
            for (size_t k = 0; k < half; ++k) {
                const std::complex<float> t = twiddle_[k * stride] * x[i + k + half];
                x[i + k + half] = x[i + k] - t;
                x[i + k] += t;
            }
        }
    }
}

void FrequencyRange::analyse(const float* const* bus, size_t frames) noexcept {
    if (window_.empty() || frames == 0) return;
    frames = std::min(frames, kN);
    for (size_t i = 0; i < frames; ++i) {
        const float shared = 0.5f * (bus[FC][i] + bus[LFE][i]);
        histL_[histPos_] = bus[FL][i] + bus[SL][i] + shared;
        histR_[histPos_] = bus[FR][i] + bus[SR][i] + shared;
        histPos_ = (histPos_ + 1) & (kN - 1);
    }
    filled_ = std::min(kN, filled_ + frames);
    // Only analyse a window that is completely filled with input. A half-empty window
    // (start-up, or the first blocks after the keep-alive silence) contains an abrupt
    // silence -> signal step, and its broadband splatter dragged the averaged range
    // down to the first bin (a 200 Hz tone read as "12 Hz - 4 kHz").
    if (filled_ < kN) return;

    for (size_t i = 0; i < kN; ++i) {
        const size_t idx = (histPos_ + i) & (kN - 1);   // oldest first
        buf_[i] = {histL_[idx] * window_[i], histR_[idx] * window_[i]};
    }
    fft(buf_.data());

    const float alpha = 1.0f - std::exp(-float(frames) / (kAverageSeconds * float(kBusRate)));
    for (size_t k = 1; k <= kN / 2; ++k) {
        const std::complex<float> z = buf_[k];
        const std::complex<float> zc = std::conj(buf_[(kN - k) & (kN - 1)]);
        const std::complex<float> a = 0.5f * (z + zc);                          // left spectrum
        const std::complex<float> b = std::complex<float>(0.0f, -0.5f) * (z - zc); // right spectrum
        const float p = std::norm(a) + std::norm(b);
        avg_[k] += alpha * (p - avg_[k]);
    }
    updateRange();
}

void FrequencyRange::decay(size_t frames) noexcept {
    if (avg_.empty()) return;
    const float keep = std::exp(-float(frames) / (kAverageSeconds * float(kBusRate)));
    for (auto& v : avg_) v *= keep;
    filled_ = 0;   // no input: wait for a full window of new audio before analysing again
    updateRange();
}

void FrequencyRange::updateRange() noexcept {
    const float binHz = float(kBusRate) / float(kN);
    const size_t kLo = std::max<size_t>(1, size_t(std::floor(kMinHz / binHz)));
    const size_t kHi = std::min(kN / 2, size_t(std::ceil(kMaxHz / binHz)));
    float peak = 0.0f;
    for (size_t k = kLo; k <= kHi; ++k) peak = std::max(peak, avg_[k]);
    const float floorPow = std::pow(10.0f, kFloorDbfs / 10.0f);
    if (peak <= floorPow) {
        low_.store(0.0f, std::memory_order_relaxed);
        high_.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const float thr = std::max(floorPow, peak * std::pow(10.0f, -kRelativeDb / 10.0f));
    size_t lo = kHi, hi = kLo;
    for (size_t k = kLo; k <= kHi; ++k) if (avg_[k] >= thr) { lo = k; break; }
    for (size_t k = kHi + 1; k-- > kLo;) if (avg_[k] >= thr) { hi = k; break; }
    // Interpolate the threshold crossing between neighbouring bins (in dB) for a
    // smoother read-out than the 23 Hz bin grid.
    auto edge = [&](size_t k, int dir) {
        const size_t n = size_t(int(k) + dir);
        if (n < kLo || n > kHi) return float(k) * binHz;
        const float a = 10.0f * std::log10(avg_[k] + 1e-30f);
        const float b = 10.0f * std::log10(avg_[n] + 1e-30f);
        const float t = 10.0f * std::log10(thr);
        const float frac = (a - b) > 1e-6f ? std::clamp((a - t) / (a - b), 0.0f, 1.0f) : 0.0f;
        return (float(k) + float(dir) * frac) * binHz;
    };
    const float lowHz = std::max(kMinHz, edge(lo, -1));
    const float highHz = std::min(kMaxHz, edge(hi, +1));
    low_.store(lowHz, std::memory_order_relaxed);
    high_.store(std::max(highHz, lowHz), std::memory_order_relaxed);
}

} // namespace avr
