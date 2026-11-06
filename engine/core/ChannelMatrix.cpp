#include "ChannelMatrix.h"

#include <cstring>

namespace avr {

namespace {
constexpr float kMinus3dB = 0.70710678f;
}

uint32_t ChannelMatrix::defaultMask(uint16_t channels) noexcept {
    switch (channels) {
        case 1: return SPK_FC;
        case 2: return kMaskStereo;
        case 3: return kMaskStereo | SPK_LFE;                     // 2.1
        case 4: return kMaskStereo | SPK_BL | SPK_BR;             // quad
        case 5: return kMaskStereo | SPK_LFE | SPK_BL | SPK_BR;   // 4.1
        case 6: return kMask51Back;                                // KSAUDIO_SPEAKER_5POINT1
        case 8: return kMask71;                                    // KSAUDIO_SPEAKER_7POINT1_SURROUND
        default: return 0;
    }
}

const char* ChannelMatrix::layoutName(uint32_t m) noexcept {
    if (m == 0) return "Silence";
    const bool lfe = (m & SPK_LFE) != 0;
    const int full = popcount32(m & ~SPK_LFE);
    const bool hasBack = (m & (SPK_BL | SPK_BR)) != 0;
    const bool hasSide = (m & (SPK_SL | SPK_SR)) != 0;
    if (full <= 1) return lfe ? "PCM 1.1" : "PCM 1.0";
    if (full == 2) return lfe ? "PCM 2.1" : "PCM 2.0";
    if (full == 3) return lfe ? "PCM 3.1" : "PCM 3.0";
    if (full == 4) return lfe ? "PCM 4.1" : "PCM 4.0";
    if (full == 5) return lfe ? "PCM 5.1" : "PCM 5.0";
    if (full == 6) return lfe ? "PCM 6.1" : "PCM 6.0";
    if (hasBack && hasSide) return lfe ? "PCM 7.1" : "PCM 7.0";
    return "PCM multichannel";
}

void ChannelMatrix::configure(uint16_t channels, uint32_t mask) noexcept {
    tapCount_ = 0;
    speakers_.fill(0);
    channels_ = channels;
    if (mask == 0) mask = defaultMask(channels);
    mask_ = mask;

    // Interleaved order follows ascending speaker bit; channels beyond the mask
    // are unassigned (Windows convention) and dropped.
    uint8_t idx = 0;
    for (uint32_t bit = 1; bit != 0 && idx < channels && idx < speakers_.size(); bit <<= 1)
        if (mask & bit) speakers_[idx++] = bit;

    const bool hasBack = (mask & (SPK_BL | SPK_BR)) != 0;
    const bool hasSide = (mask & (SPK_SL | SPK_SR)) != 0;
    has71_ = hasBack && hasSide;
    stereo_ = channels <= 2;

    // 7.1 -> 5.1 discrete fold-down: side + back summed at -3 dB each (equal power
    // for uncorrelated content; keeps front/back pans inside the surround pair).
    const float surroundGain = has71_ ? kMinus3dB : 1.0f;

    for (uint8_t i = 0; i < channels && i < speakers_.size(); ++i) {
        switch (speakers_[i]) {
            case SPK_FL:  add(i, FL, 1.0f); break;
            case SPK_FR:  add(i, FR, 1.0f); break;
            case SPK_FC:
                if (channels == 1) { add(i, FL, kMinus3dB); add(i, FR, kMinus3dB); }   // mono -> phantom
                else add(i, FC, 1.0f);
                break;
            case SPK_LFE: add(i, LFE, 1.0f); break;
            case SPK_BL:  add(i, SL, surroundGain); break;
            case SPK_BR:  add(i, SR, surroundGain); break;
            case SPK_SL:  add(i, SL, surroundGain); break;
            case SPK_SR:  add(i, SR, surroundGain); break;
            case SPK_FLC: add(i, FL, kMinus3dB); add(i, FC, kMinus3dB); break;
            case SPK_FRC: add(i, FR, kMinus3dB); add(i, FC, kMinus3dB); break;
            case SPK_BC:  add(i, SL, kMinus3dB); add(i, SR, kMinus3dB); break;
            default: break;   // top / unknown speakers are discarded
        }
    }
}

void ChannelMatrix::apply(const float* in, size_t frames, float* const* bus) const noexcept {
    for (size_t c = 0; c < kBusChannels; ++c) std::memset(bus[c], 0, frames * sizeof(float));
    const size_t stride = channels_;
    for (size_t t = 0; t < tapCount_; ++t) {
        const Tap tp = taps_[t];
        float* dst = bus[tp.out];
        const float* src = in + tp.in;
        const float g = tp.gain;
        for (size_t n = 0; n < frames; ++n) dst[n] += g * src[n * stride];
    }
}

void downmix51ToStereo(const float* const* bus, size_t frames, float* out) noexcept {
    // Lo = L + 0.707 C + 0.707 Ls ; Ro = R + 0.707 C + 0.707 Rs ; normalised to avoid clipping
    constexpr float k = kMinus3dB;
    constexpr float norm = 1.0f / (1.0f + 2.0f * k);
    for (size_t n = 0; n < frames; ++n) {
        const float c = bus[FC][n] * k;
        out[2 * n]     = (bus[FL][n] + c + k * bus[SL][n]) * norm;
        out[2 * n + 1] = (bus[FR][n] + c + k * bus[SR][n]) * norm;
    }
}

} // namespace avr
