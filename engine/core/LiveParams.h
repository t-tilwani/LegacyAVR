// LegacyAVR - parameters that change while audio is running.
// Written by the control (named-pipe) thread, read by the DSP thread once per
// 1536-sample block. All fields are independent lock-free atomics; anything that
// needs re-allocation (codec, bitrate, devices, buffer size) is NOT here - it goes
// through a full pipeline restart instead (EngineHost::requestRestart).
#pragma once

#include "ChannelEq.h"
#include "Types.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace avr {

struct LiveParams {
    LiveParams() noexcept { setEq(eqDefaultConfig()); }

    std::atomic<float> masterDb{0.0f};
    std::array<std::atomic<float>, kBusChannels> gainDb{};     // +6 .. -48 dB; <= -48.5 => mute
    std::array<std::atomic<bool>, kBusChannels> mute{};
    std::atomic<bool> muteAll{false};
    std::atomic<bool> nightMode{false};
    std::atomic<uint32_t> upmixMode{static_cast<uint32_t>(UpmixMode::Upmix51)};
    std::atomic<bool> stereoAsTwoChannel{true};   // passthrough stereo encodes as 2.0 (AVR may upmix)

    // Upmixer tuning. DSP thread re-runs Upmixer::configure() when revision changes.
    std::atomic<float> centreLevel{0.85f};
    std::atomic<float> centreFloor{0.30f};
    std::atomic<float> dialogBoost{0.6f};
    std::atomic<float> surroundLevel{0.8f};
    std::atomic<float> surroundDelayMs{12.0f};
    std::atomic<float> lfeCrossoverHz{80.0f};
    std::atomic<float> lfeLevelDb{-10.0f};
    std::atomic<uint32_t> upmixRevision{0};

    // v1.2.0: bass / subwoofer / game handling
    std::atomic<float> bassDb{0.0f};                 // low shelf (120 Hz) on every channel, -12..+12 dB
    std::atomic<uint32_t> lfeGenMode{static_cast<uint32_t>(LfeGenMode::Auto)};
    std::atomic<bool> gamingActive{false};           // effective game state (mode + UI detection)
    // v1.3.0: Game Mode's own "LFE generation" toggle. While a game is active it forces
    // generation on (like Always) and overrides the general LFE-generation setting.
    std::atomic<bool> gameLfeGen{false};
    // v1.3.0: native-LFE inhibit. Once the input's own LFE channel carries real signal,
    // Auto (and game) generation stays off until every input channel has been silent for
    // lfeResetSeconds. The latch lives here, not in ProcessingCore, so it survives pipeline
    // re-configuration (format switches, Lossless PCM re-opens, device restarts).
    std::atomic<float> lfeResetSeconds{10.0f};       // 1..300 s, default 10 (v1.4.0: was 10..600)
    std::atomic<bool> lfeInhibited{false};           // written by the DSP thread only
    std::atomic<uint64_t> lastSoundUs{0};            // last input block with any channel above silence

    // v1.4.0: custom per-channel EQ (see ChannelEq.h). The DSP thread re-reads the band
    // values when eqRevision changes; eqEnabled / channel enables are read every block.
    std::atomic<bool> eqEnabled{false};
    std::array<std::atomic<bool>, kBusChannels> eqChannelEnabled{};
    std::array<std::atomic<float>, kBusChannels * kEqBands * 3> eqBand{};   // freq, gain, q
    std::atomic<uint32_t> eqRevision{0};
    // v1.4.0: EQ audition - solo one output channel while tuning (-1 = off). Temporary:
    // never written to the configuration, and the host clears it when the dashboard stops
    // refreshing it. Saved mixer mutes are not touched.
    std::atomic<int32_t> auditionChannel{-1};

    // Diagnostics
    std::atomic<uint32_t> testSignal{static_cast<uint32_t>(TestSignal::Off)};
    std::atomic<uint32_t> testMask{0x3F};

    [[nodiscard]] static constexpr size_t eqIndex(size_t ch, size_t band, size_t field) noexcept {
        return (ch * kEqBands + band) * 3 + field;
    }
    void setEqBand(size_t ch, size_t band, const EqBand& b) noexcept {
        eqBand[eqIndex(ch, band, 0)].store(b.freqHz, std::memory_order_relaxed);
        eqBand[eqIndex(ch, band, 1)].store(b.gainDb, std::memory_order_relaxed);
        eqBand[eqIndex(ch, band, 2)].store(b.q, std::memory_order_relaxed);
    }
    void setEq(const EqConfig& c) noexcept {
        eqEnabled.store(c.enabled, std::memory_order_relaxed);
        for (size_t ch = 0; ch < kBusChannels; ++ch) {
            eqChannelEnabled[ch].store(c.channelOn[ch], std::memory_order_relaxed);
            for (size_t b = 0; b < kEqBands; ++b) setEqBand(ch, b, c.bands[ch][b]);
        }
        eqRevision.fetch_add(1, std::memory_order_release);
    }
};

inline constexpr float kGainMinDb = -48.0f;
inline constexpr float kGainMaxDb = 6.0f;
inline constexpr float kGainMuteDb = -48.5f;   // slider position below -48 == mute
inline constexpr float kBassMinDb = -12.0f;
inline constexpr float kBassMaxDb = 12.0f;
inline constexpr float kBassShelfHz = 120.0f;
// v1.4.0: whole seconds from 1 to 300; 10 s is the default and the recommendation.
inline constexpr float kLfeResetMinSeconds = 1.0f;
inline constexpr float kLfeResetDefaultSeconds = 10.0f;
inline constexpr float kLfeResetMaxSeconds = 300.0f;
// Brings any value into the allowed range (whole seconds, non-finite -> default).
[[nodiscard]] inline float clampLfeResetSeconds(float s) noexcept {
    if (!(s == s) || s > 1e9f || s < -1e9f) return kLfeResetDefaultSeconds;
    const float r = float(static_cast<long>(s + (s >= 0 ? 0.5f : -0.5f)));
    return r < kLfeResetMinSeconds ? kLfeResetMinSeconds : r > kLfeResetMaxSeconds ? kLfeResetMaxSeconds : r;
}
inline constexpr float kNativeLfeThreshold = 1.0e-3f;  // -60 dBFS peak on the input LFE channel = "meaningful"
inline constexpr float kSilenceThreshold = 1.0e-4f;    // -80 dBFS peak on every channel = "silent"

} // namespace avr
