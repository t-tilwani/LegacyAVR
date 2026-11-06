#include "ProcessingCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace avr {

namespace {
constexpr float kActivityThreshold = 3.2e-4f;       // -70 dBFS
// v1.2.0: a non-front channel more than 50 dB below the louder front channel is treated
// as leakage (Windows/app mixing residue, dither), not as surround content. Before, any
// such residue above -70 dBFS made stereo look like multichannel, which silently blocked
// "Lossless PCM 2.0" and the stereo upmixer.
constexpr float kLeakRatio = 3.1623e-3f;            // -50 dB
constexpr uint64_t kContentHoldUs = 1'500'000;      // layout hysteresis
constexpr uint64_t kIdleUs = 40'000;                // no capture data for 40 ms => keep-alive
constexpr uint64_t kLfeGenConfirmUs = 3'000'000;    // Auto LFE generation: confirmed 5.0 source
} // namespace

QueueSizing queueSizing(uint32_t bufferMs, uint32_t renderPeriodFrames, uint32_t capturePacketFrames) noexcept {
    QueueSizing q;
    const uint32_t guard = kBusRate / 1000 * 2;   // 2 ms scheduling guard
    q.floorFrames = capturePacketFrames + renderPeriodFrames / 2 + guard;
    const uint32_t user = bufferMs * (kBusRate / 1000);
    q.marginFrames = std::max(user, q.floorFrames);
    q.targetFrames = uint32_t(kBlockFrames) + q.marginFrames;
    return q;
}

bool ProcessingCore::configure(const CoreConfig& cfg, LiveParams& params,
                               SpscRing<float>& input, SpscRing<uint8_t>& output, std::string& error) {
    cfg_ = cfg;
    params_ = &params;
    in_ = &input;
    out_ = &output;

    if (cfg.inChannels == 0 || cfg.inChannels > 32) { error = "unsupported capture channel count"; return false; }
    if (cfg.inRate < 8000 || cfg.inRate > 384000) { error = "unsupported capture sample rate"; return false; }

    matrix_.configure(cfg.inChannels, cfg.inMask);

    UpmixSettings us;
    upmixer_.configure(us);
    upmixer_.reset();
    upmixRev_ = ~0u;
    bass_.reset();
    eq_.reset();                 // v1.4.0: targets are pulled from params on the first block
    eqRev_ = ~0u;
    lastTestActive_ = false;
    bass_.setGenerator(params.lfeCrossoverHz.load(), params.lfeLevelDb.load());

    night_.configure(CompressorSettings{});
    night_.reset();
    limiter_.configure(-0.3f, 120.0f);
    limiter_.reset();

    if (!resampler_.open(cfg.inRate, int(kBusChannels), kMaxInputChunk, error)) return false;

    encMain_.close();
    enc2_.close();
    encIec_ = IecType::None;
    if (cfg.bitstream()) {
        const int mainCh = cfg.dedicatedSub ? 6 : 5;
        if (!encMain_.open(cfg.codec, cfg.bitrate, mainCh, error)) {
            // No-subwoofer 5.0 not accepted by this encoder build: 5.1 with a silent LFE
            // (the LFE is folded into the fronts either way).
            if (mainCh == 6 || !encMain_.open(cfg.codec, cfg.bitrate, 6, error)) return false;
            error.clear();
        }
        if (!enc2_.open(cfg.codec, cfg.bitrate, 2, error)) return false;
        encIec_ = encMain_.iecType();
        outBytesPerFrame_ = iecBurstBytes(encIec_) / encMain_.codecFrameSamples();
        if (encIec_ == IecType::EAC3) outBytesPerFrame_ = iecBurstBytes(encIec_) / kBlockFrames;
    } else {
        if (cfg.pcmChannels != 2 && cfg.pcmChannels != 6 && cfg.pcmChannels != 8) {
            error = "PCM output supports 2, 6 or 8 channels";
            return false;
        }
        outBytesPerFrame_ = size_t(cfg.pcmChannels) * sizeof(float);
    }

    // --- buffers ------------------------------------------------------------------
    inScratch_.assign(kMaxInputChunk * cfg.inChannels, 0.0f);
    words_.assign(kMaxInputChunk * 2, 0);
    stagingCap_ = kBlockFrames + size_t(resampler_.maxOutput(int(kMaxInputChunk)));
    silence_.assign(kBlockFrames * kBusChannels, 0.0f);
    for (size_t c = 0; c < kBusChannels; ++c) {
        matrixBuf_[c].assign(kMaxInputChunk, 0.0f);
        stagingBuf_[c].assign(stagingCap_, 0.0f);
        matrixPtr_[c] = matrixBuf_[c].data();
        stagingPtr_[c] = stagingBuf_[c].data();
        silencePtr_[c] = silence_.data() + c * kBlockFrames;
    }
    burst_.assign(kIecMaxBurstBytes, 0);
    pcmOut_.assign(kBlockFrames * 8, 0.0f);
    stagingFrames_ = 0;
    frequencyRange.configure();

    // --- state --------------------------------------------------------------------
    gainLin_.fill(0.0f);   // fade in from silence on start
    lastActiveUs_.fill(0);
    lastInputUs_ = 0;
    contentStereo_ = true;
    detectEligible_ = cfg.inChannels >= 2 && (cfg.inRate == 48000 || cfg.inRate == 192000);
    detector_.reset();
    lastNight_ = false;
    primed_ = false;
    startUs_ = 0;
    lastMultiUs_ = 0;
    multiSinceUs_ = 0;
    lastSignalUs_ = 0;
    nowUs_ = 0;
    blockBudgetUs_ = 1e6 * double(kBlockFrames) / double(kBusRate);

    sizing_ = queueSizing(cfg.bufferMs, cfg.renderPeriodFrames, cfg.capturePacketFrames);
    drift_.reset(double(sizing_.targetFrames));

    inputMeters.reset();
    outputMeters.reset();
    emitMeters.reset();
    lfeFoldMeter.reset();
    stats.blocks = 0;
    stats.outputOverruns = 0;
    stats.inputDrops = 0;
    stats.encoderErrors = 0;
    stats.silenceInserts = 0;
    stats.outputRequest = 0;
    stats.pcmSwitchStatus = 0;
    stats.pcmBlockMask = 0;
    stats.pcmHoldSeconds = 0.0f;
    stats.streamKind = static_cast<uint32_t>(StreamKind::None);
    stats.contentMask = 0;
    stats.inputActive = false;
    stats.passthrough = false;
    stats.bitstreamBlocked = false;
    stats.requestedIec = static_cast<uint16_t>(encIec_);
    stats.encoderChannels = cfg.bitstream() ? uint32_t(encMain_.channels()) : uint32_t(cfg.pcmChannels);
    stats.procState = static_cast<uint32_t>(ProcessingState::Idle);
    stats.lfeGenGain = 0.0f;
    stats.lfeStatus = static_cast<uint32_t>(LfeStatus::None);
    stats.lfeGameOverride = false;
    // Note: params.lfeInhibited / lastSoundUs are deliberately NOT reset here - the
    // native-LFE latch belongs to the playback session, not to one pipeline instance.
    stats.emitChannels = 0;
    stats.emitMask = 0;
    stats.postPushFill = float(sizing_.targetFrames);
    stats.driftPpm = 0.0f;
    return true;
}

double ProcessingCore::outputFillFrames() const noexcept {
    return out_ ? double(out_->size()) / double(outBytesPerFrame_) : 0.0;
}

double ProcessingCore::captureFillFrames() const noexcept {
    return in_ ? double(in_->size()) / double(cfg_.inChannels) : 0.0;
}

void ProcessingCore::refreshUpmixSettings() noexcept {
    const uint32_t rev = params_->upmixRevision.load(std::memory_order_acquire);
    if (rev == upmixRev_) return;
    upmixRev_ = rev;
    UpmixSettings s;
    s.centreLevel = params_->centreLevel.load(std::memory_order_relaxed);
    s.dialogBoost = params_->dialogBoost.load(std::memory_order_relaxed);
    s.surroundLevel = params_->surroundLevel.load(std::memory_order_relaxed);
    s.surroundDelayMs = params_->surroundDelayMs.load(std::memory_order_relaxed);
    s.lfeCrossoverHz = params_->lfeCrossoverHz.load(std::memory_order_relaxed);
    s.lfeLevelDb = params_->lfeLevelDb.load(std::memory_order_relaxed);
    s.centreFloor = params_->centreFloor.load(std::memory_order_relaxed);
    upmixer_.configure(s);   // coefficients only; filter state is kept (no click)
    bass_.setGenerator(s.lfeCrossoverHz, s.lfeLevelDb);
}

void ProcessingCore::updateContent(size_t frames, uint64_t nowUs) noexcept {
    const size_t ch = cfg_.inChannels;
    const size_t meterCh = std::min(ch, kMaxInChannels);
    std::array<float, kMaxInChannels> pk{};
    for (size_t i = 0; i < frames; ++i) {
        const float* f = inScratch_.data() + i * ch;
        for (size_t c = 0; c < meterCh; ++c) pk[c] = std::max(pk[c], std::fabs(f[c]));
    }
    float front = 0.0f;
    for (size_t c = 0; c < meterCh; ++c)
        if (matrix_.speakerOf(c) & kMaskStereo) front = std::max(front, pk[c]);
    const float leak = front * kLeakRatio;

    uint32_t mask = 0;
    bool any = false;
    for (size_t c = 0; c < meterCh; ++c) {
        const uint32_t spk = matrix_.speakerOf(c);
        const bool isFront = (spk & kMaskStereo) != 0;
        if (pk[c] > kActivityThreshold) {
            any = true;
            if (isFront || pk[c] > leak) lastActiveUs_[c] = nowUs;
        }
        if (lastActiveUs_[c] != 0 && nowUs - lastActiveUs_[c] < kContentHoldUs) mask |= spk;
    }
    if (any) lastSignalUs_ = nowUs;

    // v1.3.0 native-LFE latch. Uses the input's configured speaker mask (which capture
    // channel IS the LFE), not the active-content layout: a 5.1/7.1 source whose LFE is
    // merely quiet looks like "5.0" in the layout, and v1.2 Auto would generate an LFE for it.
    bool sound = false, nativeLfe = false;
    for (size_t c = 0; c < meterCh; ++c) {
        if (pk[c] > kSilenceThreshold) sound = true;
        if (matrix_.speakerOf(c) == SPK_LFE && pk[c] > kNativeLfeThreshold) nativeLfe = true;
    }
    if (sound) params_->lastSoundUs.store(nowUs, std::memory_order_relaxed);
    if (nativeLfe) params_->lfeInhibited.store(true, std::memory_order_relaxed);

    stats.contentMask.store(mask, std::memory_order_relaxed);
    if (matrix_.isStereoEndpoint()) { contentStereo_ = true; return; }
    // Anything beyond FL/FR active within the hold window => multichannel content.
    const bool wasStereo = contentStereo_;
    contentStereo_ = (mask & ~kMaskStereo) == 0;
    if (!contentStereo_) {
        lastMultiUs_ = nowUs;
        if (wasStereo || multiSinceUs_ == 0) multiSinceUs_ = nowUs;
    }
}

void ProcessingCore::forwardBitstream(size_t frames) noexcept {
    const IecType t = detector_.type();
    stats.passthrough.store(true, std::memory_order_relaxed);
    stats.requestedIec.store(static_cast<uint16_t>(t), std::memory_order_relaxed);
    stats.streamKind.store(static_cast<uint32_t>(t == IecType::AC3    ? StreamKind::BitstreamAC3
                                                 : t == IecType::EAC3 ? StreamKind::BitstreamEAC3
                                                                      : StreamKind::BitstreamDTS),
                           std::memory_order_relaxed);
    stats.procState.store(static_cast<uint32_t>(ProcessingState::Bitstream), std::memory_order_relaxed);
    stats.emitChannels.store(0, std::memory_order_relaxed);   // bit-exact: levels are not decoded
    stats.emitMask.store(0, std::memory_order_relaxed);
    stagingFrames_ = 0;

    if (!cfg_.bitstream()) {
        // Shared / PCM output cannot carry IEC 61937: output silence, flag it for the UI.
        stats.bitstreamBlocked.store(true, std::memory_order_relaxed);
        const size_t bytes = frames * outBytesPerFrame_;
        if (out_->writeAvailable() >= bytes) out_->fill(0, bytes);
        return;
    }
    stats.bitstreamBlocked.store(false, std::memory_order_relaxed);
    // Words are forwarded bit-exact; the renderer is (re)opened with the matching
    // IEC subtype by the host when requestedIec differs from its current type.
    const size_t bytes = frames * 4;
    if (out_->writeAvailable() >= bytes)
        out_->write(reinterpret_cast<const uint8_t*>(words_.data()), bytes);
    else
        stats.outputOverruns.fetch_add(1, std::memory_order_relaxed);
}

bool ProcessingCore::step(uint64_t nowUs) noexcept {
    if (!in_ || !out_) return false;
    bool produced = false;
    const size_t ch = cfg_.inChannels;
    nowUs_ = nowUs;

    refreshUpmixSettings();
    if (startUs_ == 0) startUs_ = nowUs ? nowUs : 1;
    updateLfeInhibit(nowUs);

    // Prime the output ring with exactly `target` frames of digital silence (zero words
    // are also the IEC 61937 idle pattern). While the first block is being assembled the
    // renderer drains about one block, so the first real block lands on the target.
    //  v1.1 primed (target - 1 block) in whole encoded blocks: the ring ran nearly dry
    //  before the first real block and an underrun-guard silence block was injected at
    //  every start (the failing check in Pipeline.ResampleEncodeAndDriftLock).
    //  Whole blocks would overshoot instead, and the deliberately slow drift loop needs
    //  ~10 s to bleed that off - priming to the exact frame avoids both.
    if (!primed_) {
        primed_ = true;
        const double want = drift_.target();
        const double have = outputFillFrames();
        if (have < want) {
            const size_t frames = size_t(want - have);
            out_->fill(0, std::min(frames * outBytesPerFrame_, out_->writeAvailable()));
        }
        produced = true;
    }

    // Latency guard: never let the capture backlog exceed ~100 ms (e.g. after a stall).
    const size_t maxBacklog = std::max<size_t>(kBlockFrames * 2, cfg_.inRate / 10);
    size_t avail = in_->readAvailable() / ch;
    if (avail > maxBacklog) {
        in_->discard((avail - kBlockFrames) * ch);
        stats.inputDrops.fetch_add(1, std::memory_order_relaxed);
    }

    const auto test = static_cast<TestSignal>(params_->testSignal.load(std::memory_order_relaxed));

    while ((avail = in_->readAvailable() / ch) > 0) {
        const size_t n = std::min(avail, kMaxInputChunk);
        in_->read(inScratch_.data(), n * ch);
        lastInputUs_ = nowUs;
        stats.inputActive.store(true, std::memory_order_relaxed);

        inputMeters.analyseInterleaved(inScratch_.data(), ch, n, cfg_.inRate);
        updateContent(n, nowUs);

        if (detectEligible_) {
            // Exact float -> int16 conversion: a bit-transparent mixer path (100% volume,
            // no enhancements) reproduces IEC 61937 words exactly.
            for (size_t i = 0; i < n; ++i) {
                for (size_t k = 0; k < 2; ++k) {
                    const float v = inScratch_[i * ch + k] * 32768.0f;
                    words_[2 * i + k] = static_cast<int16_t>(std::clamp(std::lrint(v), -32768L, 32767L));
                }
            }
            detector_.feed(words_.data(), n * 2);
            const bool rateOk = iecTransportRate(detector_.type()) == cfg_.inRate;
            if (detector_.locked() && rateOk && test == TestSignal::Off) {
                forwardBitstream(n);
                produced = true;
                continue;
            }
        }
        if (stats.passthrough.load(std::memory_order_relaxed)) {
            stats.passthrough.store(false, std::memory_order_relaxed);
            stats.bitstreamBlocked.store(false, std::memory_order_relaxed);
            stats.requestedIec.store(static_cast<uint16_t>(encIec_), std::memory_order_relaxed);
        }
        stats.streamKind.store(static_cast<uint32_t>(StreamKind::Pcm), std::memory_order_relaxed);

        matrix_.apply(inScratch_.data(), n, matrixPtr_.data());
        for (size_t c = 0; c < kBusChannels; ++c) stageCursor_[c] = stagingPtr_[c] + stagingFrames_;
        const int got = resampler_.process(matrixPtr_.data(), int(n), stageCursor_.data(),
                                           int(stagingCap_ - stagingFrames_));
        if (got > 0) stagingFrames_ += size_t(got);

        while (stagingFrames_ >= kBlockFrames) {
            frequencyRange.analyse(stagingPtr_.data(), kBlockFrames);   // input content, before processing
            processBlock(stagingPtr_.data(), contentStereo_, true);
            produced = true;
            stagingFrames_ -= kBlockFrames;
            if (stagingFrames_ > 0)
                for (size_t c = 0; c < kBusChannels; ++c)
                    std::memmove(stagingPtr_[c], stagingPtr_[c] + kBlockFrames, stagingFrames_ * sizeof(float));

            // Drift: compare the post-push output fill with the target and trim the ratio.
            if (!blockDropped_) {
                const double fill = outputFillFrames();
                const double ppm = drift_.update(fill);
                resampler_.setCompensationPpm(ppm);
                stats.driftPpm.store(float(ppm), std::memory_order_relaxed);
                stats.postPushFill.store(float(drift_.smoothedFill()), std::memory_order_relaxed);
                // Underrun guard: what is queued plus what is already staged for the next
                // block must cover that block and half a render period. If not, the
                // renderer would starve before the next push (capture stall / clock
                // step): inject one silent block of cushion instead of a hard dropout.
                // (Counting the staged frames avoids false alarms from the capture-packet
                // phase, which v1.1 mistook for an imminent underrun.)
                if (fill + double(stagingFrames_) < double(kBlockFrames) + 0.5 * double(cfg_.renderPeriodFrames)) {
                    pushSilenceBlock();
                    stats.silenceInserts.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    // Keep-alive: WASAPI loopback delivers nothing while the endpoint is silent.
    // Keep the encoder running so the AVR stays locked (no 0.5-1 s re-sync clipping
    // the start of the next sound) and so test signals play with no input at all.
    if (!produced && nowUs - lastInputUs_ > kIdleUs) {
        const double lowWater = double(sizing_.marginFrames);
        for (int guard = 0; guard < 4 && outputFillFrames() < lowWater; ++guard) {
            stats.inputActive.store(false, std::memory_order_relaxed);
            stats.streamKind.store(static_cast<uint32_t>(StreamKind::None), std::memory_order_relaxed);
            updateContent(0, nowUs);
            pushSilenceBlock();
            frequencyRange.decay(kBlockFrames);
            if (test == TestSignal::Off)
                stats.procState.store(static_cast<uint32_t>(ProcessingState::Idle), std::memory_order_relaxed);
            produced = true;
            if (blockDropped_) break;
        }
    }
    updateOutputRequest(nowUs);
    return produced;
}

void ProcessingCore::updateLfeInhibit(uint64_t nowUs) noexcept {
    // Release the native-LFE latch once every input channel has been silent for the
    // configured interval (loopback delivers nothing at all while Windows plays nothing,
    // which counts as silence too). The generator then fades back in on its own ramp.
    if (!params_->lfeInhibited.load(std::memory_order_relaxed)) return;
    const float sec = clampLfeResetSeconds(params_->lfeResetSeconds.load(std::memory_order_relaxed));
    const uint64_t last = params_->lastSoundUs.load(std::memory_order_relaxed);
    if (nowUs > last && nowUs - last >= uint64_t(double(sec) * 1e6))
        params_->lfeInhibited.store(false, std::memory_order_relaxed);
}

void ProcessingCore::pushSilenceBlock() noexcept {
    std::fill(silence_.begin(), silence_.end(), 0.0f);   // processBlock works in place
    processBlock(silencePtr_.data(), contentStereo_, false);
}

void ProcessingCore::updateOutputRequest(uint64_t nowUs) noexcept {
    constexpr uint64_t kHoldUs = 1'500'000;   // content must be stable this long before switching
    const auto mode = static_cast<UpmixMode>(params_->upmixMode.load(std::memory_order_relaxed));
    const bool testActive = params_->testSignal.load(std::memory_order_relaxed) != uint32_t(TestSignal::Off);
    const bool gaming = params_->gamingActive.load(std::memory_order_relaxed);
    const bool passthrough = stats.passthrough.load(std::memory_order_relaxed);
    const uint32_t mask = stats.contentMask.load(std::memory_order_relaxed);

    uint32_t req = 0;
    auto st = PcmSwitchStatus::NotApplicable;
    float hold = 0.0f;
    if (cfg_.stereoPcmOverride) {
        // Currently PCM 2.0: go back to the bitstream format as soon as multichannel
        // content, pre-encoded input, a test signal, a game or another stereo mode appears.
        if (mode != UpmixMode::PcmDirect || testActive || !contentStereo_ || gaming || passthrough) {
            req = 2;
            st = PcmSwitchStatus::ReturningToBitstream;
        } else {
            st = PcmSwitchStatus::Active;
        }
    } else if (mode == UpmixMode::PcmDirect) {
        if (!cfg_.bitstream()) st = PcmSwitchStatus::BlockedOutput;
        else if (passthrough) st = PcmSwitchStatus::BlockedPassthrough;
        else if (testActive) st = PcmSwitchStatus::BlockedTest;
        else if (gaming) st = PcmSwitchStatus::BlockedGaming;
        else if (!contentStereo_) st = PcmSwitchStatus::BlockedSurround;
        else if ((mask & kMaskStereo) == 0) st = PcmSwitchStatus::NoSignal;
        else {
            const uint64_t since = std::max(startUs_, lastMultiUs_);
            const uint64_t stable = nowUs > since ? nowUs - since : 0;
            hold = float(double(stable) / 1e6);
            if (stable > kHoldUs) {
                req = 1;
                st = PcmSwitchStatus::Requested;
            } else {
                st = PcmSwitchStatus::WaitingForStereo;
            }
        }
    }
    stats.pcmBlockMask.store(st == PcmSwitchStatus::BlockedSurround ? (mask & ~kMaskStereo) : 0u,
                             std::memory_order_relaxed);
    stats.pcmHoldSeconds.store(hold, std::memory_order_relaxed);
    stats.pcmSwitchStatus.store(static_cast<uint32_t>(st), std::memory_order_relaxed);
    stats.outputRequest.store(req, std::memory_order_relaxed);
}

void ProcessingCore::applyGains(float* const* planes) noexcept {
    const bool muteAll = params_->muteAll.load(std::memory_order_relaxed);
    const float master = params_->masterDb.load(std::memory_order_relaxed);
    // v1.4.0 EQ audition: only the soloed channel plays (temporary, saved mutes untouched).
    const int32_t solo = params_->auditionChannel.load(std::memory_order_relaxed);
    for (size_t c = 0; c < kBusChannels; ++c) {
        const float g = params_->gainDb[c].load(std::memory_order_relaxed);
        const bool soloedOut = solo >= 0 && solo < int32_t(kBusChannels) && int32_t(c) != solo;
        const bool muted = muteAll || soloedOut || params_->mute[c].load(std::memory_order_relaxed) || g <= kGainMuteDb;
        const float target = muted ? 0.0f : dbToLin(std::clamp(g, kGainMinDb, kGainMaxDb) + master);
        const float start = gainLin_[c];
        if (start == target) {
            if (target != 1.0f)
                for (size_t i = 0; i < kBlockFrames; ++i) planes[c][i] *= target;
        } else {
            const float d = (target - start) / float(kBlockFrames);   // de-zippered ramp
            float gg = start;
            for (size_t i = 0; i < kBlockFrames; ++i) { gg += d; planes[c][i] *= gg; }
        }
        gainLin_[c] = target;
    }
}

void ProcessingCore::processBlock(float* const* planes, bool stereoContent, bool realInput) noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    const auto test = static_cast<TestSignal>(params_->testSignal.load(std::memory_order_relaxed));
    const auto rawMode = static_cast<UpmixMode>(params_->upmixMode.load(std::memory_order_relaxed));
    const bool gaming = params_->gamingActive.load(std::memory_order_relaxed);
    // v1.3.0: Game Mode's own LFE toggle forces generation on (Always semantics) while a
    // game is active and overrides the general setting; otherwise the general mode applies.
    const bool gameForce = gaming && params_->gameLfeGen.load(std::memory_order_relaxed);
    const auto lfeMode = gameForce ? LfeGenMode::Always
                                   : static_cast<LfeGenMode>(params_->lfeGenMode.load(std::memory_order_relaxed));
    // Native-LFE latch: suppresses Auto and game-forced generation (the user's general
    // "Always" keeps its v1.2 behaviour).
    const bool latch = params_->lfeInhibited.load(std::memory_order_relaxed);
    const bool latchApplies = latch && (gameForce || lfeMode == LfeGenMode::Auto);
    bool inhibitedNow = false;
    bool upmixedLfe = false;
    const bool noSub = !cfg_.dedicatedSub;
    // PcmDirect behaves like Passthrough inside the DSP (no upmix); the switch to a PCM
    // 2.0 output is done by the host when updateOutputRequest() asks for it.
    const bool pcmDirect = rawMode == UpmixMode::PcmDirect;
    auto mode = pcmDirect ? UpmixMode::Passthrough : rawMode;
    // Games: stereo is never upmixed (no format/upmix churn when a game briefly plays
    // stereo, and less CPU). Real multichannel game audio is untouched anyway.
    if (gaming) mode = UpmixMode::Passthrough;

    bool twoCh = false;
    bool genEnable = false;
    auto ps = ProcessingState::Idle;
    if (test != TestSignal::Off) {
        tests_.render(test, params_->testMask.load(std::memory_order_relaxed), planes, kBlockFrames);
        ps = ProcessingState::TestSignal;
    } else {
        tests_.render(TestSignal::Off, 0, silencePtr_.data(), 0);   // resets generator state
        if (stereoContent) {
            upmixer_.process(planes, kBlockFrames, mode);
            const bool upmixed = mode != UpmixMode::Passthrough;
            // Without a subwoofer the upmixer's LFE would only duplicate the bass the
            // front speakers already play: drop it.
            if (upmixed && noSub) std::memset(planes[LFE], 0, kBlockFrames * sizeof(float));
            upmixedLfe = upmixed && !noSub;
            // "Always" LFE generation also covers stereo that is kept as stereo (2.0 -> 2.1).
            genEnable = !upmixed && lfeMode == LfeGenMode::Always && !pcmDirect && !noSub;
            if (genEnable && latchApplies) { genEnable = false; inhibitedNow = true; }
            const bool keep20 = !gaming && (pcmDirect || params_->stereoAsTwoChannel.load(std::memory_order_relaxed));
            twoCh = !upmixed && keep20 && !genEnable && bass_.generatorGain() <= 0.0f;
            ps = upmixed ? (mode == UpmixMode::Upmix51 ? ProcessingState::StereoUpmix51 : ProcessingState::StereoUpmix31)
                 : cfg_.stereoPcmOverride ? ProcessingState::StereoPcm
                 : genEnable              ? ProcessingState::StereoLfeGen
                                          : ProcessingState::StereoKept;
        } else {
            // Multichannel source: LFE generation when it has no LFE of its own.
            const uint32_t mask = stats.contentMask.load(std::memory_order_relaxed);
            const bool hasLfe = (mask & SPK_LFE) != 0;
            const bool confirmed = multiSinceUs_ != 0 && nowUs_ - multiSinceUs_ >= kLfeGenConfirmUs;
            genEnable = !hasLfe && (lfeMode == LfeGenMode::Always || (lfeMode == LfeGenMode::Auto && confirmed));
            if (genEnable && latchApplies) { genEnable = false; inhibitedNow = true; }
            const bool fold71 = (mask & (SPK_BL | SPK_BR)) != 0 && (mask & (SPK_SL | SPK_SR)) != 0;
            ps = fold71 ? (genEnable ? ProcessingState::FoldDown71LfeGen : ProcessingState::FoldDown71)
                        : (genEnable ? ProcessingState::LfeGen : ProcessingState::Discrete);
        }
    }

    // LFE generation (ramped; runs on while fading out after it was disabled).
    if (genEnable || bass_.generatorGain() > 0.0f) bass_.generateLfe(planes, kBlockFrames, genEnable, noSub);
    stats.lfeGenGain.store(bass_.generatorGain(), std::memory_order_relaxed);

    // Bass control (not on test signals: calibration must stay flat). v1.4.0: the custom EQ
    // replaces it - while Custom EQ is on the shelf glides to 0 dB; the saved value is kept.
    const bool eqOn = params_->eqEnabled.load(std::memory_order_relaxed);
    bass_.setBassDb(eqOn ? 0.0f : params_->bassDb.load(std::memory_order_relaxed));
    if (test == TestSignal::Off) bass_.applyBass(planes, kBlockFrames);

    // v1.4.0 custom per-channel EQ, on the routed / upmixed output channels (every PCM
    // path; bitstream passthrough never gets here). Test signals stay flat as well.
    if (const uint32_t rev = params_->eqRevision.load(std::memory_order_acquire); rev != eqRev_) {
        eqRev_ = rev;
        eq_.pull(*params_);
    } else {
        eq_.setEnabled(eqOn);
    }
    if (test == TestSignal::Off) {
        if (lastTestActive_) eq_.clearState();   // the filters missed the test signal
        eq_.process(planes, kBlockFrames);
    }
    lastTestActive_ = test != TestSignal::Off;
    stats.eqHeadroomDb.store(eq_.headroomDb(), std::memory_order_relaxed);
    stats.eqActiveMask.store(eq_.activeMask(), std::memory_order_relaxed);

    applyGains(planes);

    // No subwoofer: the (trimmed) LFE goes to the front speakers.
    if (noSub) {
        lfeFoldMeter.analyse(&planes[LFE], 1, kBlockFrames);
        BassManager::foldLfeIntoFronts(planes, kBlockFrames);
    }

    const bool night = params_->nightMode.load(std::memory_order_relaxed) && test == TestSignal::Off;
    if (night) {
        if (!lastNight_) night_.reset();
        night_.process(planes, kBusChannels, kBlockFrames);
    }
    lastNight_ = night;
    limiter_.process(planes, kBusChannels, kBlockFrames);
    outputMeters.analyse(planes, kBusChannels, kBlockFrames);

    // Latency guard on the output side: if the renderer stalled and the ring is far
    // above target, drop this block rather than accumulating delay.
    blockDropped_ = false;
    if (outputFillFrames() > drift_.target() + 3.0 * kBlockFrames) {
        blockDropped_ = true;
        stats.outputOverruns.fetch_add(1, std::memory_order_relaxed);
    } else if (cfg_.bitstream()) {
        AudioEncoder& enc = twoCh ? enc2_ : encMain_;
        stats.encoderChannels.store(uint32_t(enc.channels()), std::memory_order_relaxed);
        if (!enc.encodeBlock(planes, &ProcessingCore::onPacket, this))
            stats.encoderErrors.fetch_add(1, std::memory_order_relaxed);
        // Output visualiser: exactly the channels inside the bitstream.
        if (enc.channels() == 2) {
            emitMeters.analyse(planes, 2, kBlockFrames);
            stats.emitMask.store(kMaskStereo, std::memory_order_relaxed);
        } else if (enc.channels() == 5) {
            const std::array<const float*, 5> ep{planes[FL], planes[FR], planes[FC], planes[SL], planes[SR]};
            emitMeters.analyse(ep.data(), 5, kBlockFrames);
            stats.emitMask.store(SPK_FL | SPK_FR | SPK_FC | SPK_SL | SPK_SR, std::memory_order_relaxed);
        } else {
            emitMeters.analyse(planes, 6, kBlockFrames);
            stats.emitMask.store(kMask51Side, std::memory_order_relaxed);
        }
        stats.emitChannels.store(uint32_t(enc.channels()), std::memory_order_relaxed);
    } else {
        const bool genActive = bass_.generatorGain() > 0.0f;
        emitPcm(planes, twoCh || (stereoContent && mode == UpmixMode::Passthrough && test == TestSignal::Off && !genActive));
    }

    if (realInput || test != TestSignal::Off) {
        stats.procState.store(static_cast<uint32_t>(ps), std::memory_order_relaxed);
        // LFE status indicator: where the LFE output comes from right now.
        auto ls = LfeStatus::None;
        const bool sourceLfe = !stereoContent && (stats.contentMask.load(std::memory_order_relaxed) & SPK_LFE) != 0;
        if (test != TestSignal::Off) ls = LfeStatus::TestSignal;
        else if (bass_.generatorGain() > 0.0f) ls = LfeStatus::Generated;
        else if (sourceLfe) ls = LfeStatus::Source;
        else if (upmixedLfe) ls = LfeStatus::Upmixed;
        else if (inhibitedNow) ls = LfeStatus::Inhibited;
        stats.lfeStatus.store(static_cast<uint32_t>(ls), std::memory_order_relaxed);
        stats.lfeGameOverride.store(gameForce, std::memory_order_relaxed);
    }
    stats.correlation.store(upmixer_.correlation(), std::memory_order_relaxed);
    stats.dialog.store(upmixer_.dialogActivity(), std::memory_order_relaxed);
    stats.nightGrDb.store(night ? night_.gainReductionDb() : 0.0f, std::memory_order_relaxed);
    stats.limiterGrDb.store(limiter_.gainReductionDb(), std::memory_order_relaxed);
    stats.blocks.fetch_add(1, std::memory_order_relaxed);

    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    const float load = float(us / blockBudgetUs_);
    stats.dspLoad.store(stats.dspLoad.load(std::memory_order_relaxed) * 0.95f + load * 0.05f,
                        std::memory_order_relaxed);
}

void ProcessingCore::onPacket(void* user, const uint8_t* data, size_t bytes) {
    auto* self = static_cast<ProcessingCore*>(user);
    const size_t n = iecPackBurst(self->encIec_, data, bytes, self->burst_.data(), self->burst_.size());
    if (n == 0) return;   // frame too large for the burst (cannot happen with normalised bitrates)
    if (self->out_->writeAvailable() < n) {   // whole bursts only - never split one
        self->stats.outputOverruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    self->out_->write(self->burst_.data(), n);
}

void ProcessingCore::emitPcm(float* const* planes, bool twoChannelContent) noexcept {
    const size_t oc = cfg_.pcmChannels;
    float* o = pcmOut_.data();
    uint32_t mask = kMaskStereo;
    if (oc == 2) {
        if (twoChannelContent) {
            for (size_t i = 0; i < kBlockFrames; ++i) { o[2 * i] = planes[FL][i]; o[2 * i + 1] = planes[FR][i]; }
        } else {
            downmix51ToStereo(planes, kBlockFrames, o);
        }
    } else if (oc == 6) {      // device 5.1: FL FR FC LFE BL/SL BR/SR
        for (size_t i = 0; i < kBlockFrames; ++i)
            for (size_t c = 0; c < 6; ++c) o[6 * i + c] = planes[c][i];
        mask = kMask51Side;
    } else {                   // device 7.1: FL FR FC LFE BL BR SL SR -> surrounds on the side pair
        for (size_t i = 0; i < kBlockFrames; ++i) {
            float* f = o + 8 * i;
            f[0] = planes[FL][i]; f[1] = planes[FR][i]; f[2] = planes[FC][i]; f[3] = planes[LFE][i];
            f[4] = 0.0f; f[5] = 0.0f; f[6] = planes[SL][i]; f[7] = planes[SR][i];
        }
        mask = kMask71;
    }
    emitMeters.analyseInterleaved(o, oc, kBlockFrames, kBusRate);
    stats.emitChannels.store(uint32_t(oc), std::memory_order_relaxed);
    stats.emitMask.store(mask, std::memory_order_relaxed);

    const size_t bytes = kBlockFrames * oc * sizeof(float);
    if (out_->writeAvailable() < bytes) {
        stats.outputOverruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    out_->write(reinterpret_cast<const uint8_t*>(o), bytes);
}

} // namespace avr
