// LegacyAVR - input channel router / fold-down matrix (N channels -> 5.1 bus).
#pragma once

#include "Types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace avr {

class ChannelMatrix {
public:
    struct Tap { uint8_t in; uint8_t out; float gain; };

    // Builds routing for a WAVEFORMATEXTENSIBLE layout. mask == 0 => Windows default for count.
    void configure(uint16_t channels, uint32_t mask) noexcept;

    // De-interleave + route: interleaved input (channels() wide) -> 6 bus planes (overwritten).
    void apply(const float* interleaved, size_t frames, float* const* bus) const noexcept;

    [[nodiscard]] uint16_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t mask() const noexcept { return mask_; }
    [[nodiscard]] bool isStereoEndpoint() const noexcept { return stereo_; }
    [[nodiscard]] bool is71() const noexcept { return has71_; }
    [[nodiscard]] size_t tapCount() const noexcept { return tapCount_; }
    [[nodiscard]] const Tap& tap(size_t i) const noexcept { return taps_[i]; }
    // Speaker bit of the i-th interleaved input channel (0 if unassigned).
    [[nodiscard]] uint32_t speakerOf(size_t i) const noexcept { return i < speakers_.size() ? speakers_[i] : 0; }

    static uint32_t defaultMask(uint16_t channels) noexcept;
    // "PCM 2.0", "PCM 5.1", "PCM 7.1" ... for a set of *active* speakers.
    static const char* layoutName(uint32_t activeMask) noexcept;

private:
    void add(uint8_t in, uint8_t out, float g) noexcept {
        if (tapCount_ < taps_.size()) taps_[tapCount_++] = {in, out, g};
    }

    std::array<Tap, 32> taps_{};
    std::array<uint32_t, 32> speakers_{};
    size_t tapCount_ = 0;
    uint16_t channels_ = 0;
    uint32_t mask_ = 0;
    bool stereo_ = false;
    bool has71_ = false;
};

// Final-stage downmix for PCM outputs that cannot take 5.1 (ITU-R BS.775 Lo/Ro).
// Writes interleaved stereo. LFE is discarded (standard Lo/Ro behaviour).
void downmix51ToStereo(const float* const* bus, size_t frames, float* outInterleaved) noexcept;

} // namespace avr
