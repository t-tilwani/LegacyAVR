#include "ChannelEq.h"

#include "LiveParams.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace avr {

namespace {
// Flat starting points: the bands are spread so each one is useful without moving it.
constexpr float kDefaultHz[kEqBands] = {100.0f, 300.0f, 1000.0f, 3000.0f, 8000.0f};
constexpr float kDefaultLfeHz[kEqBands] = {30.0f, 45.0f, 70.0f, 110.0f, 160.0f};
constexpr int kLingerBlocks = 4;   // keep filtering this long at unity before bypassing

float finiteOr(float v, float def) noexcept { return std::isfinite(v) ? v : def; }
} // namespace

EqBand eqDefaultBand(size_t channel, size_t band) noexcept {
    band = std::min(band, kEqBands - 1);
    EqBand b;
    b.freqHz = channel == LFE ? kDefaultLfeHz[band] : kDefaultHz[band];
    b.gainDb = 0.0f;
    b.q = kEqQDefault;
    return b;
}

EqBand eqClampBand(size_t channel, size_t band, EqBand b) noexcept {
    const EqBand def = eqDefaultBand(channel, band);
    b.freqHz = std::clamp(finiteOr(b.freqHz, def.freqHz), kEqFreqMinHz, eqFreqMax(channel));
    b.gainDb = std::clamp(finiteOr(b.gainDb, 0.0f), -kEqGainMaxDb, kEqGainMaxDb);
    b.q = std::clamp(finiteOr(b.q, kEqQDefault), kEqQMin, kEqQMax);
    return b;
}

EqConfig eqDefaultConfig() noexcept {
    EqConfig c;
    c.enabled = false;
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        c.channelOn[ch] = true;
        for (size_t b = 0; b < kEqBands; ++b) c.bands[ch][b] = eqDefaultBand(ch, b);
    }
    return c;
}

BiquadCoeffs eqCoeffs(EqBandType type, double fs, const EqBand& b) noexcept {
    switch (type) {
        case EqBandType::LowShelf: return BiquadCoeffs::lowShelf(fs, b.freqHz, b.gainDb);
        case EqBandType::HighShelf: return BiquadCoeffs::highShelf(fs, b.freqHz, b.gainDb);
        case EqBandType::Peak: break;
    }
    return BiquadCoeffs::peaking(fs, b.freqHz, b.gainDb, b.q);
}

ChannelEqBank::ChannelEqBank() {
    for (size_t i = 0; i < kGridPoints; ++i) {
        const double f = double(kEqFreqMinHz) * std::pow(double(kEqFreqMaxHz) / double(kEqFreqMinHz),
                                                         double(i) / double(kGridPoints - 1));
        const double w = 2.0 * std::numbers::pi * f / double(kBusRate);
        gridHz_[i] = float(f);
        c1_[i] = float(std::cos(w));
        s1_[i] = float(std::sin(w));
        c2_[i] = float(std::cos(2 * w));
        s2_[i] = float(std::sin(2 * w));
    }
    reset();
}

void ChannelEqBank::reset() noexcept {
    master_ = false;
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        chOn_[ch] = true;
        for (size_t b = 0; b < kEqBands; ++b) {
            tgt_[ch][b] = cur_[ch][b] = eqDefaultBand(ch, b);
            filt_[ch][b].set(eqCoeffs(eqBandType(b), double(kBusRate), cur_[ch][b]));
            filt_[ch][b].reset();
        }
    }
    activeMask_ = 0;
    linger_.fill(0);
    headroomDb_ = 0.0f;
    headroomLin_ = 1.0f;
    dirty_ = false;
}

void ChannelEqBank::clearState() noexcept {
    for (auto& ch : filt_)
        for (auto& f : ch) f.reset();
}

void ChannelEqBank::setBand(size_t ch, size_t band, const EqBand& b) noexcept {
    if (ch >= kBusChannels || band >= kEqBands) return;
    tgt_[ch][band] = eqClampBand(ch, band, b);
}

void ChannelEqBank::pull(const LiveParams& p) noexcept {
    master_ = p.eqEnabled.load(std::memory_order_relaxed);
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        chOn_[ch] = p.eqChannelEnabled[ch].load(std::memory_order_relaxed);
        for (size_t b = 0; b < kEqBands; ++b) {
            EqBand v;
            v.freqHz = p.eqBand[LiveParams::eqIndex(ch, b, 0)].load(std::memory_order_relaxed);
            v.gainDb = p.eqBand[LiveParams::eqIndex(ch, b, 1)].load(std::memory_order_relaxed);
            v.q = p.eqBand[LiveParams::eqIndex(ch, b, 2)].load(std::memory_order_relaxed);
            tgt_[ch][b] = eqClampBand(ch, b, v);
        }
    }
}

bool ChannelEqBank::glide(size_t ch) noexcept {
    const bool on = master_ && chOn_[ch];
    bool changed = false;
    for (size_t b = 0; b < kEqBands; ++b) {
        EqBand& c = cur_[ch][b];
        const EqBand& t = tgt_[ch][b];
        const float tg = on ? t.gainDb : 0.0f;
        if (c.gainDb != tg) {
            const float d = tg - c.gainDb;
            c.gainDb = std::fabs(d) <= kGainStepDb ? tg : c.gainDb + (d > 0 ? kGainStepDb : -kGainStepDb);
            changed = true;
        }
        if (c.freqHz != t.freqHz) {
            const float r = t.freqHz / c.freqHz;
            c.freqHz = r > kFreqStepRatio ? c.freqHz * kFreqStepRatio
                     : r < 1.0f / kFreqStepRatio ? c.freqHz / kFreqStepRatio : t.freqHz;
            changed = true;
        }
        if (c.q != t.q) {
            const float r = t.q / c.q;
            c.q = r > kQStepRatio ? c.q * kQStepRatio : r < 1.0f / kQStepRatio ? c.q / kQStepRatio : t.q;
            changed = true;
        }
    }
    return changed;
}

void ChannelEqBank::process(float* const* bus, size_t frames) noexcept {
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        const uint32_t bit = 1u << ch;
        const bool changed = glide(ch);
        bool anyGain = false;
        for (size_t b = 0; b < kEqBands; ++b) anyGain |= cur_[ch][b].gainDb != 0.0f;
        if (changed) {
            for (size_t b = 0; b < kEqBands; ++b)
                filt_[ch][b].set(eqCoeffs(eqBandType(b), double(kBusRate), cur_[ch][b]));
            dirty_ = true;
        }
        if (anyGain) {
            // A unity (0 dB) RBJ section keeps a zero state, so starting from a cleared
            // state here is exactly as if the filter had been running all along.
            activeMask_ |= bit;
            linger_[ch] = kLingerBlocks;
        } else if (activeMask_ & bit) {
            // Back at unity: let the last glide step's state ring out, then bypass.
            if (--linger_[ch] <= 0) {
                activeMask_ &= ~bit;
                for (auto& f : filt_[ch]) f.reset();
                continue;
            }
        } else {
            continue;   // bypassed: untouched (bit-identical to no EQ)
        }
        float* x = bus[ch];
        for (size_t b = 0; b < kEqBands; ++b) {
            filt_[ch][b].process(x, frames);
            filt_[ch][b].sanitize();
        }
    }

    if (dirty_) {
        updateHeadroom();
        dirty_ = false;
    }
    const float target = dbToLin(headroomDb_);
    const float start = headroomLin_;
    if (start != 1.0f || target != 1.0f) {
        const float d = (target - start) / float(frames ? frames : 1);
        for (size_t ch = 0; ch < kBusChannels; ++ch) {
            float g = start;
            float* x = bus[ch];
            if (d == 0.0f) {
                for (size_t i = 0; i < frames; ++i) x[i] *= g;
            } else {
                for (size_t i = 0; i < frames; ++i) { g += d; x[i] *= g; }
            }
        }
    }
    headroomLin_ = std::fabs(target - 1.0f) < 1e-6f ? 1.0f : target;
}

void ChannelEqBank::updateHeadroom() noexcept {
    float worst = 1.0f;   // largest |H|^2 of any channel's combined response
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        bool anyBoost = false;
        for (size_t b = 0; b < kEqBands; ++b) anyBoost |= cur_[ch][b].gainDb > 0.0f;
        if (!anyBoost) continue;   // cuts only: the response never exceeds 0 dB
        const float fMax = ch == LFE ? 250.0f : kEqFreqMaxHz;
        auto at = [&](float c1, float s1, float c2, float s2) {
            float m = 1.0f;
            for (size_t b = 0; b < kEqBands; ++b) m *= filt_[ch][b].coeffs().magnitudeSq(c1, s1, c2, s2);
            return m;
        };
        for (size_t i = 0; i < kGridPoints; ++i) {
            if (gridHz_[i] > fMax) break;
            worst = std::max(worst, at(c1_[i], s1_[i], c2_[i], s2_[i]));
        }
        // Narrow bells can fall between grid points: also look at each band's centre.
        for (size_t b = 0; b < kEqBands; ++b) {
            const double w = 2.0 * std::numbers::pi * double(cur_[ch][b].freqHz) / double(kBusRate);
            worst = std::max(worst, at(float(std::cos(w)), float(std::sin(w)), float(std::cos(2 * w)), float(std::sin(2 * w))));
        }
    }
    headroomDb_ = worst > 1.0f ? -10.0f * std::log10(worst) : 0.0f;
}

} // namespace avr
