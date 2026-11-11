// LegacyAVR - bass handling on the 5.1 bus (v1.2.0).
//
//  * Bass control : RBJ low shelf at 120 Hz on all six channels (-12 .. +12 dB).
//  * LFE generation ("5.0 -> 5.1", separate from the stereo upmixer): an LR4 low-pass of
//    the channel sum is written into the LFE plane when the source has no LFE of its
//    own. The level ramps in (0.5 s) and out (0.3 s) so switching never clicks or jumps.
//    Uses the same crossover / LFE level as the upmixer so both sound alike.
//    Source = FL + FR + FC + SL + SR. Only with a dedicated subwoofer (v1.5.0: without one
//    LegacyAVR never generates an LFE channel).
//  * LFE fold (v1.5.0; v1.6.0: required when the output has no LFE channel, optional and
//    experimental without a subwoofer - otherwise the LFE is passed through untouched): the source's own LFE is low-passed
//    (LR4, 120 Hz - the LFE band limit, removes out-of-band rubbish), added to FL and FR at
//    -3 dB each (equal power: the pair carries the LFE at its original power) and the LFE
//    plane is cleared. Gain changes are ramped. The safety limiter after it guarantees the
//    -0.3 dBFS ceiling, so a full-scale LFE on top of full-scale fronts cannot clip.
//    The LFE channel trim (applied before the fold) sets how loud the folded bass is.
//
// Real-time safe: no allocation, coefficient maths only when a value changes.
#pragma once

#include "Biquad.h"
#include "Types.h"

#include <array>
#include <cstddef>

namespace avr {

class BassManager {
public:
    static constexpr float kGenSumGain = 0.35f;   // channel-sum scaling before the LFE level
    static constexpr float kFoldGain = 0.70710678f;   // LFE -> each front speaker (-3 dB; LFE trim adjusts)
    static constexpr float kFoldCutoffHz = 120.0f;    // LFE band limit
    static constexpr float kBassStepDb = 1.5f;    // v1.4.0: shelf glide per block (12 dB in ~0.26 s)

    void reset() noexcept;

    // Bass shelf. Call once per block with the wanted value; the shelf glides towards it.
    // Cheap: coefficients are only recomputed while it moves.
    void setBassDb(float target) noexcept;
    [[nodiscard]] float bassDb() const noexcept { return bassDb_; }
    void applyBass(float* const* bus, size_t frames) noexcept;

    // Generator settings (crossover + level shared with the upmixer).
    void setGenerator(float crossoverHz, float levelDb) noexcept;

    // Adds a generated LFE into bus[LFE] while `enable` (ramped). Returns the current ramp
    // gain (0..1).
    float generateLfe(float* const* bus, size_t frames, bool enable) noexcept;
    [[nodiscard]] float generatorGain() const noexcept { return genGain_; }

    // `enable` folds the (filtered) LFE into FL + FR; the fold gain ramps over the block when
    // `enable` changes, so switching never clicks. `clearLfe` empties the LFE plane afterwards
    // (true while folding; false while ramping the fold out because the LFE is passed through
    // again - v1.6.0).
    void foldLfeIntoFronts(float* const* bus, size_t frames, bool enable, bool clearLfe = true) noexcept;
    [[nodiscard]] float foldGain() const noexcept { return foldGain_; }

private:
    std::array<Biquad, kBusChannels> shelf_{};
    float bassDb_ = 0.0f;
    bool bassActive_ = false;

    LinkwitzRiley4 genLp_;
    float genCrossover_ = -1.0f;
    float genLevel_ = 0.316f;
    float genGain_ = 0.0f;

    LinkwitzRiley4 foldLp_;
    bool foldLpReady_ = false;
    float foldGain_ = 0.0f;   // current per-front gain (0 .. kFoldGain)
};

} // namespace avr
