// LegacyAVR - optional custom per-channel EQ (v1.4.0).
//
// Five parametric bands per output channel (FL FR C LFE SL SR), always in this order:
//     band 0 low shelf | bands 1-3 peaking (bell) | band 4 high shelf
// Gain is limited to +/-6 dB, Q (bells only) to 0.5..4, frequency to 20 Hz..20 kHz
// (20..200 Hz on the LFE channel). Everything starts flat.
//
// It runs on the 5.1 bus after routing / upmixing / LFE generation, so it applies to
// stereo kept as stereo, upmixed stereo and real multichannel alike; bit-exact
// passthrough never reaches it.
//
// Click-free by construction:
//   * parameters glide towards their targets block by block (gain <= 0.75 dB, frequency
//     <= 1/3 octave, Q <= x1.25 per 32 ms block) while the filter state is kept;
//   * bypass (master or channel) glides every gain to 0 dB - where the RBJ sections are
//     exact unity - and only then stops processing the channel, so settings are never lost
//     and switching never jumps;
//   * headroom: the largest boost of any active channel's combined response is measured on
//     a log-spaced grid and the same amount is taken off every channel (ramped), so a
//     +6 dB boost cannot drive the bus into clipping and the speaker balance is kept. The
//     safety limiter after it still catches anything else.
//
// Real-time safe: no allocation, no locks; trigonometry only while a parameter glides.
#pragma once

#include "Biquad.h"
#include "Types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace avr {

struct LiveParams;

inline constexpr size_t kEqBands = 5;
inline constexpr float kEqGainMaxDb = 6.0f;
inline constexpr float kEqQMin = 0.5f;
inline constexpr float kEqQMax = 4.0f;
inline constexpr float kEqQDefault = 1.0f;
inline constexpr float kEqFreqMinHz = 20.0f;
inline constexpr float kEqFreqMaxHz = 20000.0f;
inline constexpr float kEqLfeFreqMaxHz = 200.0f;   // the LFE channel only carries bass

enum class EqBandType : uint8_t { LowShelf = 0, Peak = 1, HighShelf = 2 };

struct EqBand {
    float freqHz = 1000.0f;
    float gainDb = 0.0f;
    float q = kEqQDefault;
};

struct EqConfig {
    bool enabled = false;                                       // master "Custom EQ"
    std::array<bool, kBusChannels> channelOn{};                 // per-channel enable
    std::array<std::array<EqBand, kEqBands>, kBusChannels> bands{};
};

[[nodiscard]] constexpr EqBandType eqBandType(size_t band) noexcept {
    return band == 0 ? EqBandType::LowShelf : band == kEqBands - 1 ? EqBandType::HighShelf : EqBandType::Peak;
}
[[nodiscard]] constexpr float eqFreqMax(size_t channel) noexcept {
    return channel == LFE ? kEqLfeFreqMaxHz : kEqFreqMaxHz;
}
// Factory default (flat) for one band.
[[nodiscard]] EqBand eqDefaultBand(size_t channel, size_t band) noexcept;
// Brings a band into the allowed range for its channel (non-finite values -> default).
[[nodiscard]] EqBand eqClampBand(size_t channel, size_t band, EqBand b) noexcept;
// Master off, every channel enabled, every band flat.
[[nodiscard]] EqConfig eqDefaultConfig() noexcept;
[[nodiscard]] BiquadCoeffs eqCoeffs(EqBandType type, double fs, const EqBand& b) noexcept;

class ChannelEqBank {
public:
    static constexpr float kGainStepDb = 0.75f;        // per block
    static constexpr float kFreqStepRatio = 1.2599f;   // 1/3 octave per block
    static constexpr float kQStepRatio = 1.25f;
    static constexpr size_t kGridPoints = 48;          // headroom estimate, 20 Hz .. 20 kHz

    ChannelEqBank();

    void reset() noexcept;             // flat and bypassed, filter memory cleared
    void clearState() noexcept;        // filter memory only (e.g. after a test signal)

    // Targets. pull() reads LiveParams (call when params.eqRevision changed).
    void pull(const LiveParams& p) noexcept;
    void setEnabled(bool master) noexcept { master_ = master; }
    void setChannelEnabled(size_t ch, bool on) noexcept { if (ch < kBusChannels) chOn_[ch] = on; }
    void setBand(size_t ch, size_t band, const EqBand& b) noexcept;

    // In place on the six bus planes.
    void process(float* const* bus, size_t frames) noexcept;

    [[nodiscard]] float headroomDb() const noexcept { return headroomDb_; }   // <= 0
    [[nodiscard]] uint32_t activeMask() const noexcept { return activeMask_; } // channels being filtered
    [[nodiscard]] bool idle() const noexcept { return activeMask_ == 0 && headroomLin_ == 1.0f; }
    // Current (glided) band of a channel - tests.
    [[nodiscard]] const EqBand& current(size_t ch, size_t band) const noexcept { return cur_[ch][band]; }

private:
    bool glide(size_t ch) noexcept;    // moves cur_ towards the effective target; true if changed
    void updateHeadroom() noexcept;

    bool master_ = false;
    std::array<bool, kBusChannels> chOn_{};
    std::array<std::array<EqBand, kEqBands>, kBusChannels> tgt_{}, cur_{};
    std::array<std::array<Biquad, kEqBands>, kBusChannels> filt_{};
    uint32_t activeMask_ = 0;
    std::array<int, kBusChannels> linger_{};
    float headroomDb_ = 0.0f;
    float headroomLin_ = 1.0f;
    bool dirty_ = true;
    std::array<float, kGridPoints> gridHz_{}, c1_{}, s1_{}, c2_{}, s2_{};
};

} // namespace avr
