#include "Dynamics.h"

#include <algorithm>
#include <cmath>

namespace avr {

namespace {
constexpr size_t kSub = 32;
float subCoef(float ms) noexcept {
    // one-pole coefficient evaluated once per 32-sample sub-block
    return 1.0f - std::exp(-float(kSub) / (std::max(ms, 0.1f) * 0.001f * float(kBusRate)));
}
} // namespace

void NightModeCompressor::configure(const CompressorSettings& s) noexcept {
    s_ = s;
    attackCoef_ = subCoef(s.attackMs);
    releaseCoef_ = subCoef(s.releaseMs);
}

float NightModeCompressor::computeGainDb(float x) const noexcept {
    // Soft-knee static curve (Giannoulis, Massberg & Reiss, JAES 2012).
    const float t = s_.thresholdDb, w = s_.kneeDb, r = s_.ratio;
    float y;
    if (2.0f * (x - t) < -w) {
        y = x;
    } else if (2.0f * std::fabs(x - t) <= w) {
        const float d = x - t + w * 0.5f;
        y = x + (1.0f / r - 1.0f) * d * d / (2.0f * w);
    } else {
        y = t + (x - t) / r;
    }
    return y - x;   // <= 0
}

void NightModeCompressor::process(float* const* bus, size_t channels, size_t frames) noexcept {
    const float makeup = s_.makeupDb;
    for (size_t base = 0; base < frames; base += kSub) {
        const size_t n = std::min(kSub, frames - base);
        float ms = 0.0f;
        for (size_t c = 0; c < channels; ++c) {
            if (c == LFE) continue;   // LFE does not drive the detector
            float acc = 0.0f;
            for (size_t i = base; i < base + n; ++i) acc += bus[c][i] * bus[c][i];
            ms = std::max(ms, acc / float(n));
        }
        const float levelDb = 10.0f * std::log10(ms + 1e-12f);
        const float coef = levelDb > envDb_ ? attackCoef_ : releaseCoef_;
        envDb_ += coef * (levelDb - envDb_);
        const float target = computeGainDb(envDb_) + makeup;
        const float g0 = dbToLin(gainDb_), g1 = dbToLin(target);
        const float dg = (g1 - g0) / float(n);
        for (size_t c = 0; c < channels; ++c) {
            float g = g0;
            for (size_t i = base; i < base + n; ++i) { g += dg; bus[c][i] *= g; }
        }
        gainDb_ = target;
        grDb_ = target - makeup;
    }
}

void SafetyLimiter::configure(float ceilingDb, float releaseMs) noexcept {
    ceiling_ = dbToLin(ceilingDb);
    releaseCoef_ = subCoef(releaseMs);
}

void SafetyLimiter::process(float* const* bus, size_t channels, size_t frames) noexcept {
    for (size_t base = 0; base < frames; base += kSub) {
        const size_t n = std::min(kSub, frames - base);
        float peak = 0.0f;
        for (size_t c = 0; c < channels; ++c)
            for (size_t i = base; i < base + n; ++i) peak = std::max(peak, std::fabs(bus[c][i]));
        const float needed = peak > ceiling_ ? ceiling_ / peak : 1.0f;
        float target = gain_ + releaseCoef_ * (1.0f - gain_);   // release toward unity
        if (needed < target) target = needed;                    // instant attack
        const bool attacking = target < gain_;
        const float dg = (target - gain_) / float(n);
        for (size_t c = 0; c < channels; ++c) {
            if (attacking) {
                for (size_t i = base; i < base + n; ++i) bus[c][i] *= target;
            } else {   // release ramp; target <= needed so the peak stays under the ceiling
                float g = gain_;
                for (size_t i = base; i < base + n; ++i) { g += dg; bus[c][i] *= g; }
            }
        }
        gain_ = target;
    }
}

} // namespace avr
