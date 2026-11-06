// LegacyAVR - dynamics: Night Mode compressor + output safety limiter.
// Both are channel-linked (one gain for the whole 5.1 bus) so the sound stage
// never shifts, and both run their detectors at a 32-sample sub-block rate.
#pragma once

#include "Types.h"

#include <cstddef>

namespace avr {

struct CompressorSettings {
    float thresholdDb = -30.0f;
    float ratio = 3.0f;
    float kneeDb = 8.0f;
    float attackMs = 8.0f;
    float releaseMs = 250.0f;
    float makeupDb = 8.0f;
};

class NightModeCompressor {
public:
    void configure(const CompressorSettings& s) noexcept;
    void reset() noexcept { envDb_ = -120.0f; gainDb_ = 0.0f; grDb_ = 0.0f; }
    void process(float* const* bus, size_t channels, size_t frames) noexcept;
    [[nodiscard]] float gainReductionDb() const noexcept { return grDb_; }
    [[nodiscard]] float staticCurveDb(float levelDb) const noexcept { return computeGainDb(levelDb); }

private:
    [[nodiscard]] float computeGainDb(float levelDb) const noexcept;
    CompressorSettings s_{};
    float attackCoef_ = 0, releaseCoef_ = 0;
    float envDb_ = -120.0f, gainDb_ = 0.0f, grDb_ = 0.0f;
};

// Sub-block peak limiter with instant attack: each 32-sample sub-block's peak is
// known before gain is applied, so the output never exceeds the ceiling.
class SafetyLimiter {
public:
    void configure(float ceilingDb = -0.3f, float releaseMs = 120.0f) noexcept;
    void reset() noexcept { gain_ = 1.0f; }
    void process(float* const* bus, size_t channels, size_t frames) noexcept;
    [[nodiscard]] float gainReductionDb() const noexcept { return linToDb(gain_); }

private:
    float ceiling_ = 0.966f;
    float releaseCoef_ = 0.0f;
    float gain_ = 1.0f;
};

} // namespace avr
