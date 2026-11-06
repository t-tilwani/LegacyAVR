// LegacyAVR - diagnostics signal generator (renders directly onto the 5.1 bus).
//
//  PinkNoise   : independent pink noise per enabled speaker, -20 dBFS RMS
//                (LFE band-limited to 120 Hz) - classic level-calibration signal.
//  ChannelId   : cycles FL, C, FR, SR, SL, LFE (AVR set-up order). Uses speech
//                clips synthesised by SAPI when available, otherwise N beeps for
//                channel N (LFE uses a 50 Hz tone).
//  CenterBleed : alternates 3 s of phantom centre (identical noise in FL+FR)
//                with 3 s of discrete C. Both should image at the same spot; if
//                the "discrete" phase comes from L/R, C is mis-mapped.
//  LfeSweep    : 8 s log sweep 20 -> 200 Hz on LFE only. Output should fade
//                around the AVR's LFE low-pass (80-120 Hz) and never come from mains.
//  PhaseCheck  : FL/FR then SL/SR pairs, 3 s in-phase, 3 s with one side
//                inverted. In-phase must sound focused/centred between the pair;
//                if the "inverted" step sounds MORE focused, a speaker is wired
//                with reversed polarity.
#pragma once

#include "Biquad.h"
#include "Types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace avr {

class TestSignalGenerator {
public:
    TestSignalGenerator();

    // Not real-time: stores a 48 kHz mono speech clip for a bus channel.
    void setSpeechClip(size_t busChannel, std::vector<float> clip);
    [[nodiscard]] bool hasSpeech() const noexcept;

    // Real-time. Renders kBlockFrames (or `frames`) of the selected test onto the bus
    // (all 6 planes are overwritten). mode/mask may change between calls.
    void render(TestSignal mode, uint32_t mask, float* const* bus, size_t frames) noexcept;

    // Status for telemetry / UI (DSP thread writes, others read).
    std::atomic<int32_t> currentChannel{-1};
    std::atomic<uint32_t> currentStep{0};
    std::atomic<float> currentFrequency{0.0f};

private:
    float white(size_t ch) noexcept;
    float pink(size_t ch) noexcept;
    void renderPink(uint32_t mask, float* const* bus, size_t frames, bool correlatedFront = false) noexcept;

    struct PinkState { float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0; };
    std::array<uint32_t, kBusChannels> rng_{};
    std::array<PinkState, kBusChannels> pinkState_{};
    LinkwitzRiley4 lfeBand_;
    std::array<std::vector<float>, kBusChannels> speech_{};

    float pinkNorm_ = 0.03f;    // calibrated in the constructor to -20 dBFS RMS
    TestSignal lastMode_ = TestSignal::Off;
    uint64_t pos_ = 0;          // samples since test start
    double phase_ = 0.0;        // oscillator phase (radians)
};

} // namespace avr
