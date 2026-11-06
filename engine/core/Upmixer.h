// LegacyAVR - stereo -> 3.1 / 5.1 active upmixer (time domain, zero added latency).
//
// Signal model (per sample; steering gains updated every 32 samples):
//   m = (L+R)/2  (mid)      s = (L-R)/2  (side)
//   rho    = short-term L/R correlation        (-1 .. +1)
//   dialog = speech-band (200 Hz - 4 kHz) mid / (mid + side) energy ratio
//   C   = sqrt2 * g_c * m                       g_c = f(rho, dialog) * centreLevel
//   L'  = L - g_c * m ,  R' = R - g_c * m        (energy-preserving centre extraction)
//   LFE = LR4_lowpass(m, 80 Hz) * lfeLevel      (-10 dB default: decoders add +10 dB in-band)
//   SL  =  g_s * AP1( LPF7k( delay(s) ) )        phase-delay surround extraction,
//   SR  = -g_s * AP2( LPF7k( delay(s) ) )        decorrelated by two all-pass chains
#pragma once

#include "Biquad.h"
#include "Types.h"

#include <array>
#include <cstddef>

namespace avr {

struct UpmixSettings {
    float centreLevel = 0.85f;      // 0..1 maximum centre steering
    float centreFloor = 0.30f;      // 0..1 fraction of centreLevel always present (stops "pumping")
    float dialogBoost = 0.6f;       // 0..1 extra centre steering when speech is detected
    float surroundLevel = 0.8f;     // 0..1
    float surroundDelayMs = 12.0f;  // Haas delay for surrounds (0..25 ms)
    float lfeCrossoverHz = 80.0f;   // LR4 crossover
    float lfeLevelDb = -10.0f;
};

class Upmixer {
public:
    void configure(const UpmixSettings& s) noexcept;   // coefficient maths only, no allocation
    void reset() noexcept;

    // bus: 6 planes, FL/FR hold the stereo input. Other planes are overwritten.
    void process(float* const* bus, size_t frames, UpmixMode mode) noexcept;

    [[nodiscard]] float correlation() const noexcept { return rho_; }
    [[nodiscard]] float dialogActivity() const noexcept { return dialog_; }

private:
    static constexpr size_t kDelayMax = 2048;   // > 25 ms @ 48 kHz, power of two
    static constexpr size_t kSubBlock = 32;

    UpmixSettings s_{};
    LinkwitzRiley4 lfeLp_;
    Biquad dlgHpM_, dlgLpM_, dlgHpS_, dlgLpS_;
    Biquad surLp_;
    Biquad ap1a_, ap1b_, ap2a_, ap2b_;

    std::array<float, kDelayMax> delay_{};
    size_t delayPos_ = 0;
    size_t delaySamples_ = 576;

    float ell_ = 1e-9f, err_ = 1e-9f, elr_ = 0.0f;
    float emBand_ = 1e-9f, esBand_ = 1e-9f;
    float statCoef_ = 0.0f;
    float rho_ = 0.0f, dialog_ = 0.0f;
    float gc_ = 0.0f, gs_ = 0.0f;
    float gcSmooth_ = 0.0f, gsSmooth_ = 0.0f;   // slow steering state (anti-pumping)
    float steerAttack_ = 0.0f, steerRelease_ = 0.0f;
    float lfeGain_ = 0.316f;
};

} // namespace avr
