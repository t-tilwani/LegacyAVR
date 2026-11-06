#include "Upmixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avr {

void Upmixer::configure(const UpmixSettings& s) noexcept {
    s_ = s;
    const double fs = kBusRate;
    lfeLp_.setLowpass(fs, std::clamp(s.lfeCrossoverHz, 40.0f, 200.0f));
    lfeGain_ = dbToLin(s.lfeLevelDb);

    const auto hp = BiquadCoeffs::highpass(fs, 200.0, 0.7071);
    const auto lp = BiquadCoeffs::lowpass(fs, 4000.0, 0.7071);
    dlgHpM_.set(hp); dlgHpS_.set(hp);
    dlgLpM_.set(lp); dlgLpS_.set(lp);

    surLp_.set(BiquadCoeffs::lowpass(fs, 7000.0, 0.7071));
    ap1a_.set(BiquadCoeffs::allpass(fs, 600.0, 0.5));
    ap1b_.set(BiquadCoeffs::allpass(fs, 3100.0, 0.5));
    ap2a_.set(BiquadCoeffs::allpass(fs, 1100.0, 0.5));
    ap2b_.set(BiquadCoeffs::allpass(fs, 4700.0, 0.5));

    const float ms = std::clamp(s.surroundDelayMs, 0.0f, 25.0f);
    delaySamples_ = static_cast<size_t>(ms * 0.001f * float(kBusRate));
    statCoef_ = 1.0f - std::exp(-1.0f / (0.120f * float(kBusRate)));   // ~120 ms statistics
    // Steering smoothing, evaluated once per 32-sample sub-block. Centre/surround gain
    // rises in ~150 ms and falls in ~600 ms so music with a moving vocal / wide mix no
    // longer makes the centre speaker "breathe" in and out (v1.1.0).
    const float subRate = float(kBusRate) / float(kSubBlock);
    steerAttack_ = 1.0f - std::exp(-1.0f / (0.150f * subRate));
    steerRelease_ = 1.0f - std::exp(-1.0f / (0.600f * subRate));
}

void Upmixer::reset() noexcept {
    lfeLp_.reset();
    for (Biquad* b : {&dlgHpM_, &dlgLpM_, &dlgHpS_, &dlgLpS_, &surLp_, &ap1a_, &ap1b_, &ap2a_, &ap2b_})
        b->reset();
    delay_.fill(0.0f);
    delayPos_ = 0;
    ell_ = err_ = 1e-9f; elr_ = 0; emBand_ = esBand_ = 1e-9f;
    rho_ = dialog_ = gc_ = gs_ = 0;
    gcSmooth_ = gsSmooth_ = 0;
}

void Upmixer::process(float* const* bus, size_t frames, UpmixMode mode) noexcept {
    float* L = bus[FL];
    float* R = bus[FR];
    if (mode == UpmixMode::Passthrough) {
        for (size_t c = FC; c < kBusChannels; ++c) std::memset(bus[c], 0, frames * sizeof(float));
        return;
    }
    const bool surrounds = (mode == UpmixMode::Upmix51);
    constexpr float kSqrt2 = 1.41421356f;
    const float a = statCoef_;

    for (size_t base = 0; base < frames; base += kSubBlock) {
        const size_t n = std::min(kSubBlock, frames - base);

        // 1. statistics ------------------------------------------------------------
        for (size_t i = base; i < base + n; ++i) {
            const float l = L[i], r = R[i];
            ell_ += a * (l * l - ell_);
            err_ += a * (r * r - err_);
            elr_ += a * (l * r - elr_);
            const float mb = dlgLpM_.process(dlgHpM_.process(0.5f * (l + r)));
            const float sb = dlgLpS_.process(dlgHpS_.process(0.5f * (l - r)));
            emBand_ += a * (mb * mb - emBand_);
            esBand_ += a * (sb * sb - esBand_);
        }
        const float energy = ell_ + err_;
        rho_ = energy > 1e-10f ? std::clamp(elr_ / std::sqrt(ell_ * err_ + 1e-20f), -1.0f, 1.0f) : 0.0f;
        dialog_ = emBand_ / (emBand_ + esBand_ + 1e-12f);   // 0.5 neutral, -> 1 centred speech

        // 2. steering targets ----------------------------------------------------------
        const float corrSteer = std::clamp((rho_ - 0.2f) / 0.8f, 0.0f, 1.0f);
        const float dlg = std::clamp((dialog_ - 0.6f) / 0.35f, 0.0f, 1.0f);
        const float steer = std::clamp(corrSteer + s_.dialogBoost * dlg * (1.0f - corrSteer), 0.0f, 1.0f);
        const float cfloor = std::clamp(s_.centreFloor, 0.0f, 1.0f);
        const float gcRaw = std::clamp(s_.centreLevel * (cfloor + (1.0f - cfloor) * steer), 0.0f, 1.0f);
        const float gsRaw = surrounds ? s_.surroundLevel * std::clamp(0.6f - 0.4f * rho_, 0.2f, 1.0f) : 0.0f;
        gcSmooth_ += (gcRaw > gcSmooth_ ? steerAttack_ : steerRelease_) * (gcRaw - gcSmooth_);
        gsSmooth_ += (gsRaw > gsSmooth_ ? steerAttack_ : steerRelease_) * (gsRaw - gsSmooth_);
        const float gcTarget = gcSmooth_;
        const float gsTarget = gsSmooth_;

        // 3. render with linear gain interpolation across the sub-block ------------------
        const float dgc = (gcTarget - gc_) / float(n);
        const float dgs = (gsTarget - gs_) / float(n);
        for (size_t i = base; i < base + n; ++i) {
            gc_ += dgc; gs_ += dgs;
            const float l = L[i], r = R[i];
            const float m = 0.5f * (l + r);
            const float s = 0.5f * (l - r);

            bus[FC][i] = kSqrt2 * gc_ * m;
            L[i] = l - gc_ * m;
            R[i] = r - gc_ * m;
            bus[LFE][i] = lfeGain_ * lfeLp_.process(m);

            delay_[delayPos_] = s;
            const float sd = delay_[(delayPos_ - delaySamples_) & (kDelayMax - 1)];
            delayPos_ = (delayPos_ + 1) & (kDelayMax - 1);
            const float sf = surLp_.process(sd);
            if (surrounds) {
                bus[SL][i] = gs_ * ap1b_.process(ap1a_.process(sf));
                bus[SR][i] = -gs_ * ap2b_.process(ap2a_.process(sf));
            } else {
                bus[SL][i] = 0.0f;
                bus[SR][i] = 0.0f;
            }
        }
        gc_ = gcTarget; gs_ = gsTarget;
    }
    lfeLp_.sanitize(); surLp_.sanitize();
    ap1a_.sanitize(); ap1b_.sanitize(); ap2a_.sanitize(); ap2b_.sanitize();
    dlgHpM_.sanitize(); dlgLpM_.sanitize(); dlgHpS_.sanitize(); dlgLpS_.sanitize();
}

} // namespace avr
