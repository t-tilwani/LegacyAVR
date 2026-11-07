#include "BassManager.h"

#include "LiveParams.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avr {

void BassManager::reset() noexcept {
    for (auto& b : shelf_) b.reset();
    genLp_.reset();
    genGain_ = 0.0f;
    foldLp_.reset();
    foldGain_ = 0.0f;
}

void BassManager::setBassDb(float target) noexcept {
    // v1.4.0: glide towards the target (<= kBassStepDb per call = per 32 ms block) so a big
    // jump - e.g. Custom EQ taking over and the shelf returning to 0 dB - cannot thump.
    target = std::clamp(target, kBassMinDb, kBassMaxDb);
    const float d = target - bassDb_;
    const float db = std::fabs(d) <= kBassStepDb ? target : bassDb_ + (d > 0 ? kBassStepDb : -kBassStepDb);
    if (db == bassDb_ && (bassActive_ || db == 0.0f)) return;
    bassDb_ = db;
    bassActive_ = std::fabs(db) >= 0.05f;
    if (!bassActive_) {
        for (auto& b : shelf_) b.reset();   // bypassed: next activation starts clean
        return;
    }
    const auto c = BiquadCoeffs::lowShelf(double(kBusRate), double(kBassShelfHz), double(db));
    for (auto& b : shelf_) b.set(c);         // state kept: changing the knob does not click
}

void BassManager::applyBass(float* const* bus, size_t frames) noexcept {
    if (!bassActive_) return;
    for (size_t c = 0; c < kBusChannels; ++c) {
        shelf_[c].process(bus[c], frames);
        shelf_[c].sanitize();
    }
}

void BassManager::setGenerator(float crossoverHz, float levelDb) noexcept {
    crossoverHz = std::clamp(crossoverHz, 40.0f, 200.0f);
    if (crossoverHz != genCrossover_) {
        genCrossover_ = crossoverHz;
        genLp_.setLowpass(double(kBusRate), double(crossoverHz));
    }
    genLevel_ = dbToLin(std::clamp(levelDb, -30.0f, 6.0f));
}

float BassManager::generateLfe(float* const* bus, size_t frames, bool enable) noexcept {
    if (!enable && genGain_ <= 0.0f) {
        genLp_.reset();
        return 0.0f;
    }
    // Ramp: 0 -> 1 in 0.5 s, 1 -> 0 in 0.3 s, linear across the block.
    const float blockSec = float(frames) / float(kBusRate);
    const float step = enable ? blockSec / 0.5f : -blockSec / 0.3f;
    const float target = std::clamp(genGain_ + step, 0.0f, 1.0f);
    const float d = (target - genGain_) / float(frames ? frames : 1);
    float g = genGain_;
    const float k = kGenSumGain * genLevel_;
    float* lfe = bus[LFE];
    for (size_t i = 0; i < frames; ++i) {
        g += d;
        const float sum = bus[FL][i] + bus[FR][i] + bus[FC][i] + bus[SL][i] + bus[SR][i];
        lfe[i] += g * k * genLp_.process(sum);
    }
    genLp_.sanitize();
    genGain_ = target;
    return genGain_;
}

void BassManager::foldLfeIntoFronts(float* const* bus, size_t frames, bool enable) noexcept {
    float* lfe = bus[LFE];
    const float target = enable ? kFoldGain : 0.0f;
    if (target > 0.0f || foldGain_ > 0.0f) {
        if (!foldLpReady_) {
            foldLp_.setLowpass(double(kBusRate), double(kFoldCutoffHz));
            foldLpReady_ = true;
        }
        const float d = (target - foldGain_) / float(frames ? frames : 1);
        float g = foldGain_;
        for (size_t i = 0; i < frames; ++i) {
            g += d;
            const float v = g * foldLp_.process(lfe[i]);
            bus[FL][i] += v;
            bus[FR][i] += v;
        }
        foldLp_.sanitize();
        foldGain_ = target;
    } else {
        foldLp_.reset();   // next fold starts from a clean filter
    }
    std::memset(lfe, 0, frames * sizeof(float));
}

} // namespace avr
