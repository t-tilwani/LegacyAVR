// LegacyAVR - biquad filters (RBJ Audio-EQ cookbook), transposed direct form II.
// Real-time safe: no allocation, no branches in the sample loop.
#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>

namespace avr {

struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static BiquadCoeffs lowpass(double fs, double f0, double q) { return make(0, fs, f0, q); }
    static BiquadCoeffs highpass(double fs, double f0, double q) { return make(1, fs, f0, q); }
    static BiquadCoeffs bandpass(double fs, double f0, double q) { return make(2, fs, f0, q); }
    static BiquadCoeffs allpass(double fs, double f0, double q) { return make(3, fs, f0, q); }

    // RBJ low shelf, shelf slope S = 1 (no overshoot). gainDb = 0 => exact unity.
    static BiquadCoeffs lowShelf(double fs, double f0, double gainDb) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * f0 / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / 2.0 * std::sqrt(2.0);          // S = 1
        const double sa = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1) + (A - 1) * cw + sa;
        BiquadCoeffs c;
        c.b0 = float(A * ((A + 1) - (A - 1) * cw + sa) / a0);
        c.b1 = float(2 * A * ((A - 1) - (A + 1) * cw) / a0);
        c.b2 = float(A * ((A + 1) - (A - 1) * cw - sa) / a0);
        c.a1 = float(-2 * ((A - 1) + (A + 1) * cw) / a0);
        c.a2 = float(((A + 1) + (A - 1) * cw - sa) / a0);
        return c;
    }

    // RBJ high shelf, shelf slope S = 1. gainDb = 0 => exact unity.     (v1.4.0, custom EQ)
    static BiquadCoeffs highShelf(double fs, double f0, double gainDb) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * f0 / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / 2.0 * std::sqrt(2.0);          // S = 1
        const double sa = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1) - (A - 1) * cw + sa;
        BiquadCoeffs c;
        c.b0 = float(A * ((A + 1) + (A - 1) * cw + sa) / a0);
        c.b1 = float(-2 * A * ((A - 1) + (A + 1) * cw) / a0);
        c.b2 = float(A * ((A + 1) + (A - 1) * cw - sa) / a0);
        c.a1 = float(2 * ((A - 1) - (A + 1) * cw) / a0);
        c.a2 = float(((A + 1) - (A - 1) * cw - sa) / a0);
        return c;
    }

    // RBJ peaking (bell) EQ. gainDb = 0 => exact unity.                   (v1.4.0, custom EQ)
    static BiquadCoeffs peaking(double fs, double f0, double gainDb, double q) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * f0 / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1 + alpha / A;
        BiquadCoeffs c;
        c.b0 = float((1 + alpha * A) / a0);
        c.b1 = float(-2 * cw / a0);
        c.b2 = float((1 - alpha * A) / a0);
        c.a1 = float(-2 * cw / a0);
        c.a2 = float((1 - alpha / A) / a0);
        return c;
    }

    // Squared magnitude from precomputed cos/sin of w and 2w (allocation- and trig-free;
    // used by the EQ headroom estimate on the DSP thread).
    [[nodiscard]] float magnitudeSq(float c1, float s1, float c2, float s2) const noexcept {
        const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const float dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return (nr * nr + ni * ni) / (dr * dr + di * di);
    }

    // Magnitude response at frequency f (tests / UI plots).
    [[nodiscard]] double magnitude(double fs, double f) const {
        const double w = 2.0 * std::numbers::pi * f / fs;
        const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }

private:
    static BiquadCoeffs make(int type, double fs, double f0, double q) {
        const double w0 = 2.0 * std::numbers::pi * f0 / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        double b0d = 0, b1d = 0, b2d = 0;
        const double a0 = 1 + alpha, a1d = -2 * cw, a2d = 1 - alpha;
        switch (type) {
            case 0: b0d = (1 - cw) / 2; b1d = 1 - cw;    b2d = (1 - cw) / 2; break;  // LPF
            case 1: b0d = (1 + cw) / 2; b1d = -(1 + cw); b2d = (1 + cw) / 2; break;  // HPF
            case 2: b0d = alpha;        b1d = 0;         b2d = -alpha;       break;  // BPF 0 dB peak
            case 3: b0d = 1 - alpha;    b1d = -2 * cw;   b2d = 1 + alpha;    break;  // APF
            default: break;
        }
        BiquadCoeffs c;
        c.b0 = float(b0d / a0); c.b1 = float(b1d / a0); c.b2 = float(b2d / a0);
        c.a1 = float(a1d / a0); c.a2 = float(a2d / a0);
        return c;
    }
};

class Biquad {
public:
    void set(const BiquadCoeffs& c) noexcept { c_ = c; }
    [[nodiscard]] const BiquadCoeffs& coeffs() const noexcept { return c_; }
    void reset() noexcept { z1_ = z2_ = 0; }

    inline float process(float x) noexcept {
        const float y = c_.b0 * x + z1_;
        z1_ = c_.b1 * x - c_.a1 * y + z2_;
        z2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }
    void process(float* data, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i) data[i] = process(data[i]);
    }
    // Flush denormals that accumulate in silent passages (FTZ/DAZ is also set per thread).
    void sanitize() noexcept {
        if (std::fabs(z1_) < 1e-20f) z1_ = 0;
        if (std::fabs(z2_) < 1e-20f) z2_ = 0;
    }

private:
    BiquadCoeffs c_;
    float z1_ = 0, z2_ = 0;
};

// 4th-order Linkwitz-Riley (two cascaded Butterworth sections): -6 dB at fc,
// LP + HP sum flat in magnitude - the standard home-theatre crossover.
class LinkwitzRiley4 {
public:
    void setLowpass(double fs, double fc) noexcept {
        const auto c = BiquadCoeffs::lowpass(fs, fc, std::numbers::sqrt2 / 2.0);
        a_.set(c); b_.set(c);
    }
    void setHighpass(double fs, double fc) noexcept {
        const auto c = BiquadCoeffs::highpass(fs, fc, std::numbers::sqrt2 / 2.0);
        a_.set(c); b_.set(c);
    }
    void reset() noexcept { a_.reset(); b_.reset(); }
    inline float process(float x) noexcept { return b_.process(a_.process(x)); }
    void sanitize() noexcept { a_.sanitize(); b_.sanitize(); }

private:
    Biquad a_, b_;
};

} // namespace avr
