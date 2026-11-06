// LegacyAVR - bass handling on the 5.1 bus (v1.2.0).
//
//  * Bass control : RBJ low shelf at 120 Hz on all six channels (-12 .. +12 dB).
//  * LFE generation ("5.0 -> 5.1", separate from the stereo upmixer): an LR4 low-pass of
//    the channel sum is written into the LFE plane when the source has no LFE of its
//    own. The level ramps in (0.5 s) and out (0.3 s) so switching never clicks or jumps.
//    Uses the same crossover / LFE level as the upmixer so both sound alike.
//      with a subwoofer : source = FL + FR + FC + SL + SR
//      without one      : source = FC + SL + SR  (bass of the centre/surround speakers,
//                         which is then folded into the fronts = simple bass redirect)
//  * No-subwoofer fold: LFE is added to FL and FR and the LFE plane is cleared.
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
    static constexpr float kFoldGain = 1.0f;      // LFE -> each front speaker (0 dB; LFE trim adjusts)
    static constexpr float kBassStepDb = 1.5f;    // v1.4.0: shelf glide per block (12 dB in ~0.26 s)

    void reset() noexcept;

    // Bass shelf. Call once per block with the wanted value; the shelf glides towards it.
    // Cheap: coefficients are only recomputed while it moves.
    void setBassDb(float target) noexcept;
    [[nodiscard]] float bassDb() const noexcept { return bassDb_; }
    void applyBass(float* const* bus, size_t frames) noexcept;

    // Generator settings (crossover + level shared with the upmixer).
    void setGenerator(float crossoverHz, float levelDb) noexcept;

    // Adds a generated LFE into bus[LFE] while `enable` (ramped). excludeFronts selects the
    // no-subwoofer source (centre + surrounds only). Returns the current ramp gain (0..1).
    float generateLfe(float* const* bus, size_t frames, bool enable, bool excludeFronts) noexcept;
    [[nodiscard]] float generatorGain() const noexcept { return genGain_; }

    // No subwoofer: LFE -> FL + FR, LFE cleared.
    static void foldLfeIntoFronts(float* const* bus, size_t frames, float gain = kFoldGain) noexcept;

private:
    std::array<Biquad, kBusChannels> shelf_{};
    float bassDb_ = 0.0f;
    bool bassActive_ = false;

    LinkwitzRiley4 genLp_;
    float genCrossover_ = -1.0f;
    float genLevel_ = 0.316f;
    float genGain_ = 0.0f;
};

} // namespace avr
