// LegacyAVR - clock-drift controller.
//
// The virtual cable (capture clock) and the HDMI/S/PDIF device (render clock)
// run from different oscillators; a 50 ppm mismatch drifts ~170 ms per hour.
// A PI controller watches the smoothed output-ring fill (in 48 kHz frames, sampled
// right after each block is pushed) and trims the resampler ratio in ppm.
//
// Tuning (per 32 ms block): the plant is an integrator, fill += 0.0015 * ppm frames
// per block. With kp = 3, ki = 0.005 the loop has w = sqrt(0.0015*ki) = 0.0027 rad/block
// (~12 s time constant) and damping z = 0.0015*kp / 2w = 0.82 - slow enough that
// capture-packet jitter in the fill measurement does not modulate pitch audibly
// (< 1 cent), fast enough to absorb any real crystal mismatch.
#pragma once

#include <algorithm>

namespace avr {

class DriftController {
public:
    struct Gains {
        double kp = 3.0;         // ppm per frame of error
        double ki = 0.005;       // ppm per frame of error per block
        double maxPpm = 1000.0;  // hard clamp (well above any real crystal mismatch)
        double smoothing = 0.02; // EMA on the measured fill (~50 blocks)
    };

    void reset(double targetFrames) noexcept {
        target_ = targetFrames;
        smoothed_ = targetFrames;
        integral_ = 0.0;
        ppm_ = 0.0;
        primed_ = false;
    }
    void setGains(const Gains& g) noexcept { g_ = g; }
    void setTarget(double targetFrames) noexcept { target_ = targetFrames; }

    // Returns the compensation (positive => produce more samples).
    double update(double fillFrames) noexcept {
        if (!primed_) { smoothed_ = fillFrames; primed_ = true; }
        smoothed_ += g_.smoothing * (fillFrames - smoothed_);
        const double err = smoothed_ - target_;                 // >0: too much buffered
        integral_ = std::clamp(integral_ + g_.ki * err, -g_.maxPpm, g_.maxPpm);
        ppm_ = std::clamp(-(g_.kp * err + integral_), -g_.maxPpm, g_.maxPpm);
        return ppm_;
    }

    [[nodiscard]] double ppm() const noexcept { return ppm_; }
    [[nodiscard]] double smoothedFill() const noexcept { return smoothed_; }
    [[nodiscard]] double target() const noexcept { return target_; }

private:
    Gains g_{};
    double target_ = 0, smoothed_ = 0, integral_ = 0, ppm_ = 0;
    bool primed_ = false;
};

} // namespace avr
