#include "TestSignals.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace avr {

namespace {
constexpr float kPinkRmsTarget = 0.1f;        // -20 dBFS RMS
constexpr double kTwoPi = 2.0 * std::numbers::pi;
// AVR speaker set-up order: FL, C, FR, SR, SL, LFE
constexpr std::array<size_t, kBusChannels> kIdOrder{FL, FC, FR, SR, SL, LFE};
constexpr uint64_t kIdSlot = 3 * kBusRate;    // 3 s per channel
} // namespace

TestSignalGenerator::TestSignalGenerator() {
    lfeBand_.setLowpass(kBusRate, 120.0);
    // Calibrate the pink filter's output level once (deterministic: fixed seed).
    rng_[0] = 0x12345678u;
    pinkNorm_ = 1.0f;
    double acc = 0.0;
    constexpr int kCal = 1 << 18;
    for (int i = 0; i < kCal; ++i) { const double v = pink(0); acc += v * v; }
    pinkNorm_ = kPinkRmsTarget / float(std::sqrt(acc / kCal));
    pinkState_ = {};
    for (size_t c = 0; c < kBusChannels; ++c) rng_[c] = 0x9E3779B9u * uint32_t(c + 1);
}

void TestSignalGenerator::setSpeechClip(size_t ch, std::vector<float> clip) {
    if (ch < kBusChannels) speech_[ch] = std::move(clip);
}

bool TestSignalGenerator::hasSpeech() const noexcept {
    return std::all_of(speech_.begin(), speech_.end(), [](const auto& v) { return !v.empty(); });
}

float TestSignalGenerator::white(size_t ch) noexcept {
    uint32_t x = rng_[ch];                 // xorshift32
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_[ch] = x;
    return float(int32_t(x)) * (1.0f / 2147483648.0f);
}

float TestSignalGenerator::pink(size_t ch) noexcept {
    // Paul Kellet's refined pink filter (+/-0.05 dB above 9.2 Hz @ 44.1-48 kHz).
    PinkState& s = pinkState_[ch];
    const float w = white(ch);
    s.b0 = 0.99886f * s.b0 + w * 0.0555179f;
    s.b1 = 0.99332f * s.b1 + w * 0.0750759f;
    s.b2 = 0.96900f * s.b2 + w * 0.1538520f;
    s.b3 = 0.86650f * s.b3 + w * 0.3104856f;
    s.b4 = 0.55000f * s.b4 + w * 0.5329522f;
    s.b5 = -0.7616f * s.b5 - w * 0.0168980f;
    const float out = s.b0 + s.b1 + s.b2 + s.b3 + s.b4 + s.b5 + s.b6 + w * 0.5362f;
    s.b6 = w * 0.115926f;
    return out * pinkNorm_;
}

void TestSignalGenerator::renderPink(uint32_t mask, float* const* bus, size_t frames, bool correlatedFront) noexcept {
    for (size_t c = 0; c < kBusChannels; ++c) {
        if (!(mask & (1u << c))) { std::memset(bus[c], 0, frames * sizeof(float)); continue; }
        for (size_t i = 0; i < frames; ++i) {
            float v;
            if (correlatedFront && c == FR) v = bus[FL][i];          // identical signal => phantom centre
            else v = pink(c);
            bus[c][i] = (c == LFE) ? lfeBand_.process(v) * 2.0f : v;  // band-limited LFE, level-matched
        }
    }
}

void TestSignalGenerator::render(TestSignal mode, uint32_t mask, float* const* bus, size_t frames) noexcept {
    if (mode != lastMode_) {
        lastMode_ = mode;
        pos_ = 0;
        phase_ = 0.0;
        lfeBand_.reset();
    }
    for (size_t c = 0; c < kBusChannels; ++c) std::memset(bus[c], 0, frames * sizeof(float));

    switch (mode) {
        case TestSignal::Off:
            currentChannel.store(-1, std::memory_order_relaxed);
            currentStep.store(0, std::memory_order_relaxed);
            return;

        case TestSignal::PinkNoise:
            renderPink(mask & 0x3F, bus, frames);
            currentChannel.store(-1, std::memory_order_relaxed);
            break;

        case TestSignal::ChannelId: {
            const uint64_t slot = pos_ / kIdSlot;
            const size_t ch = kIdOrder[slot % kBusChannels];
            const uint64_t t0 = pos_ % kIdSlot;
            currentChannel.store(int32_t(ch), std::memory_order_relaxed);
            currentStep.store(uint32_t(slot % kBusChannels), std::memory_order_relaxed);
            const auto& clip = speech_[ch];
            if (!clip.empty() && ch != LFE) {
                for (size_t i = 0; i < frames; ++i) {
                    const uint64_t t = t0 + i;
                    bus[ch][i] = t < clip.size() ? clip[size_t(t)] * 0.5f : 0.0f;
                }
            } else {
                // Beep code: (index+1) beeps of 150 ms, 150 ms apart. LFE: 50 Hz, others 1 kHz.
                const size_t beeps = slot % kBusChannels + 1;
                const double freq = ch == LFE ? 50.0 : 1000.0;
                const uint64_t beepLen = kBusRate * 15 / 100;
                for (size_t i = 0; i < frames; ++i) {
                    const uint64_t t = t0 + i;
                    const uint64_t idx = t / (2 * beepLen);
                    const bool on = idx < beeps && (t % (2 * beepLen)) < beepLen;
                    phase_ += kTwoPi * freq / kBusRate;
                    if (phase_ > kTwoPi) phase_ -= kTwoPi;
                    bus[ch][i] = on ? 0.25f * float(std::sin(phase_)) : 0.0f;
                }
            }
            break;
        }

        case TestSignal::CenterBleed: {
            const uint32_t step = uint32_t((pos_ / (3 * kBusRate)) % 2);
            currentStep.store(step, std::memory_order_relaxed);
            if (step == 0) {
                renderPink((1u << FL) | (1u << FR), bus, frames, /*correlatedFront*/ true);
                for (size_t i = 0; i < frames; ++i) { bus[FL][i] *= 0.7071f; bus[FR][i] *= 0.7071f; }
                currentChannel.store(-1, std::memory_order_relaxed);
            } else {
                renderPink(1u << FC, bus, frames);
                currentChannel.store(FC, std::memory_order_relaxed);
            }
            break;
        }

        case TestSignal::LfeSweep: {
            const double dur = 8.0;
            currentChannel.store(LFE, std::memory_order_relaxed);
            for (size_t i = 0; i < frames; ++i) {
                const double t = std::fmod(double(pos_ + i) / kBusRate, dur);
                const double f = 20.0 * std::pow(10.0, t / dur);   // 20 -> 200 Hz
                phase_ += kTwoPi * f / kBusRate;
                if (phase_ > kTwoPi) phase_ -= kTwoPi;
                bus[LFE][i] = 0.3f * float(std::sin(phase_));
                if (i == 0) currentFrequency.store(float(f), std::memory_order_relaxed);
            }
            break;
        }

        case TestSignal::PhaseCheck: {
            // step 0: FL/FR in phase, 1: FR inverted, 2: SL/SR in phase, 3: SR inverted
            const uint32_t step = uint32_t((pos_ / (3 * kBusRate)) % 4);
            currentStep.store(step, std::memory_order_relaxed);
            const size_t a = step < 2 ? FL : SL;
            const size_t b = step < 2 ? FR : SR;
            const float sign = (step & 1) ? -1.0f : 1.0f;
            currentChannel.store(-1, std::memory_order_relaxed);
            for (size_t i = 0; i < frames; ++i) {
                const float v = pink(a) * 0.7071f;
                bus[a][i] = v;
                bus[b][i] = sign * v;
            }
            break;
        }
    }
    pos_ += frames;
}

} // namespace avr
