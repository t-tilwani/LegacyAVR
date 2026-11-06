// Integration tests: FFmpeg encoders, digital channel-mapping loopback via the
// FFmpeg decoders, and the full ProcessingCore (resampling, drift, passthrough).
#include "TestFramework.h"

#include "../engine/core/Encoder.h"
#include "../engine/core/Iec61937.h"
#include "../engine/core/OutputPolicy.h"
#include "../engine/core/ProcessingCore.h"
#include "PipelineSim.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <numbers>
#include <vector>

using namespace avr;

namespace {
struct Planes {
    std::array<std::vector<float>, kBusChannels> buf;
    std::array<const float*, kBusChannels> ptr{};
    Planes() {
        for (size_t c = 0; c < kBusChannels; ++c) { buf[c].assign(kBlockFrames, 0.0f); ptr[c] = buf[c].data(); }
    }
    void tone(size_t ch, double freq, double& phase) {
        for (auto& b : buf) std::fill(b.begin(), b.end(), 0.0f);
        for (size_t i = 0; i < kBlockFrames; ++i) {
            buf[ch][i] = 0.5f * float(std::sin(phase));
            phase += 2 * std::numbers::pi * freq / 48000.0;
        }
    }
};

struct Collected {
    std::vector<std::vector<uint8_t>> packets;
    static void sink(void* u, const uint8_t* d, size_t n) {
        static_cast<Collected*>(u)->packets.emplace_back(d, d + n);
    }
};
} // namespace

TEST(Encoder, Ac3FramesAndSync) {
    AudioEncoder e;
    std::string err;
    CHECK(e.open(Codec::AC3, 640000, 6, err));
    CHECK_EQ(e.codecFrameSamples(), size_t(1536));
    Planes p;
    double ph = 0;
    Collected c;
    for (int i = 0; i < 4; ++i) { p.tone(FL, 1000, ph); CHECK(e.encodeBlock(p.ptr.data(), &Collected::sink, &c)); }
    CHECK_EQ(c.packets.size(), size_t(4));
    for (auto& pk : c.packets) {
        CHECK_EQ(pk.size(), size_t(2560));             // 640 kbps * 32 ms / 8
        CHECK_EQ(pk[0], 0x0B);
        CHECK_EQ(pk[1], 0x77);
    }
    CHECK(e.iecType() == IecType::AC3);
}

TEST(Encoder, DtsInteractiveFitsBurst) {
    AudioEncoder e;
    std::string err;
    CHECK(e.open(Codec::DTS, 1509750, 6, err));
    CHECK_EQ(e.codecFrameSamples(), size_t(512));
    Planes p;
    double ph = 0;
    p.tone(FC, 440, ph);
    Collected c;
    CHECK(e.encodeBlock(p.ptr.data(), &Collected::sink, &c));
    CHECK_EQ(c.packets.size(), size_t(3));             // 3 x 512 per 1536 block
    const auto& pk = c.packets[0];
    CHECK_EQ(pk.size(), size_t(2016));
    CHECK(pk[0] == 0x7F && pk[1] == 0xFE && pk[2] == 0x80 && pk[3] == 0x01);
    std::vector<uint8_t> burst(kIecMaxBurstBytes);
    CHECK_EQ(iecPackBurst(e.iecType(), pk.data(), pk.size(), burst.data(), burst.size()), size_t(2048));
    CHECK_EQ(burst[0], 0x72);                          // 2016 + 8 <= 2048: preamble kept
}

TEST(Encoder, Eac3Frames) {
    AudioEncoder e;
    std::string err;
    CHECK(e.open(Codec::EAC3, 1024000, 6, err));
    Planes p;
    double ph = 0;
    p.tone(SL, 300, ph);
    Collected c;
    CHECK(e.encodeBlock(p.ptr.data(), &Collected::sink, &c));
    CHECK_EQ(c.packets.size(), size_t(1));
    CHECK(c.packets[0][0] == 0x0B && c.packets[0][1] == 0x77);
    CHECK_EQ(int(c.packets[0][5] >> 3), 16);           // bsid 16 => E-AC-3
}

// Digital loopback: a tone on bus channel k must decode on channel k only.
// This pins the FL FR FC LFE SL SR ordering end-to-end through libavcodec.
static void mappingRoundTrip(Codec codec, const char* decoderName) {
    AudioEncoder e;
    std::string err;
    CHECK(e.open(codec, defaultBitrate(codec), 6, err));
    const AVCodec* dc = avcodec_find_decoder_by_name(decoderName);
    CHECK(dc != nullptr);
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        AVCodecContext* d = avcodec_alloc_context3(dc);
        CHECK(avcodec_open2(d, dc, nullptr) == 0);
        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        Planes p;
        double ph = 0;
        const double freq = ch == LFE ? 60.0 : 1000.0;
        std::array<double, 8> energy{};
        for (int blk = 0; blk < 12; ++blk) {
            p.tone(ch, freq, ph);
            Collected c;
            CHECK(e.encodeBlock(p.ptr.data(), &Collected::sink, &c));
            for (auto& data : c.packets) {
                av_new_packet(pkt, int(data.size()));
                std::copy(data.begin(), data.end(), pkt->data);
                if (avcodec_send_packet(d, pkt) == 0) {
                    while (avcodec_receive_frame(d, fr) == 0) {
                        if (blk < 4) continue;   // skip codec start-up
                        const int nch = fr->ch_layout.nb_channels;
                        for (int k = 0; k < nch && k < 8; ++k) {
                            const auto* s = reinterpret_cast<const float*>(fr->extended_data[k]);
                            for (int i = 0; i < fr->nb_samples; ++i) energy[size_t(k)] += double(s[i]) * s[i];
                        }
                    }
                }
                av_packet_unref(pkt);
            }
        }
        size_t best = 0;
        for (size_t k = 1; k < energy.size(); ++k) if (energy[k] > energy[best]) best = k;
        CHECK_EQ(best, ch);
        double others = 0;
        for (size_t k = 0; k < energy.size(); ++k) if (k != ch) others += energy[k];
        CHECK(others < energy[ch] * 0.01);               // > 20 dB isolation
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&d);
    }
}

TEST(Mapping, Ac3ChannelOrder) { mappingRoundTrip(Codec::AC3, "ac3"); }
TEST(Mapping, DtsChannelOrder) { mappingRoundTrip(Codec::DTS, "dca"); }

// ---------------------------------------------------------------------------
// ProcessingCore with simulated clocks: 96 kHz 7.1 capture, render consuming at
// +150 ppm. The drift controller must keep the output buffer bounded.
TEST(Pipeline, ResampleEncodeAndDriftLock) {
    LiveParams params;
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    in.allocate(8 * 96000);
    out.allocate(4 * 48000);
    CoreConfig cfg;
    cfg.inRate = 96000;
    cfg.inChannels = 8;
    cfg.inMask = kMask71;
    cfg.codec = Codec::AC3;
    cfg.bitrate = 640000;
    cfg.renderPeriodFrames = 480;
    cfg.capturePacketFrames = 480;
    cfg.bufferMs = 20;
    ProcessingCore core;
    std::string err;
    CHECK(core.configure(cfg, params, in, out, err));

    double ppmSum = 0;
    int ppmCount = 0;
    std::vector<float> chunk(8 * 960);
    std::vector<uint8_t> sink(480 * 4 * 2);
    double phase = 0, renderAcc = 0;
    uint64_t now = 0;
    Iec61937Detector det;
    double minFill = 1e9, maxFill = 0;
    const int steps = 30 * 100;   // 30 s in 10 ms steps (~2.5 drift-loop time constants)
    for (int s = 0; s < steps; ++s) {
        for (size_t i = 0; i < 960; ++i) {   // 10 ms @ 96 kHz, tone on FL and SL
            const float v = 0.3f * float(std::sin(phase));
            phase += 2 * std::numbers::pi * 997.0 / 96000.0;
            for (size_t c = 0; c < 8; ++c) chunk[i * 8 + c] = (c == 0 || c == 6) ? v : 0.0f;
        }
        in.write(chunk.data(), chunk.size());
        now += 10'000;
        core.step(now);
        renderAcc += 480.0 * (1.0 + 150e-6);  // render clock slightly fast
        const size_t frames = size_t(renderAcc);
        renderAcc -= double(frames);
        const size_t got = out.read(sink.data(), frames * 4);
        det.feed(reinterpret_cast<const int16_t*>(sink.data()), got / 2);
        if (s > 500) {
            const double fill = core.outputFillFrames();
            minFill = std::min(minFill, fill);
            maxFill = std::max(maxFill, fill);
        }
        if (s >= steps - 1000) { ppmSum += core.stats.driftPpm.load(); ++ppmCount; }
    }
    const double ppmAvg = ppmSum / std::max(ppmCount, 1);
    std::printf("         drift: avg %.1f ppm (last 10 s), fill %.0f..%.0f frames, target %.0f, inserts %u\n",
                ppmAvg, minFill, maxFill, core.driftTargetFrames(), core.stats.silenceInserts.load());
    CHECK(det.locked());
    CHECK(det.type() == IecType::AC3);
    CHECK(core.stats.blocks.load() > 900);
    // v1.2.0: the ring is primed to the full target, so start-up no longer needs a
    // rescue silence block (v1.1 primed one block short and failed here).
    CHECK_EQ(core.stats.silenceInserts.load(), 0u);
    CHECK(minFill > 0.0);                          // never ran dry after settling
    CHECK(maxFill < 1536.0 * 3);                   // and never accumulated latency
    // The controller is speeding the resampler up to match the +150 ppm render clock.
    // Averaged over 10 s: the loop is still settling at 30 s (12 s time constant), so a
    // single instantaneous sample was a fragile thing to test.
    CHECK(ppmAvg > 50.0);
    CHECK(ppmAvg < 600.0);
    CHECK((core.stats.contentMask.load() & SPK_SL) != 0);
}

// Feeds an IEC 61937 AC-3 stream through the core and reports whether the output carries
// the source bursts bit-exact. v1.4.0: shared by the plain test and the custom-EQ test.
static bool passthroughIsBitExact(LiveParams& params) {
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    in.allocate(2 * 48000);
    out.allocate(4 * 48000);
    CoreConfig cfg;
    cfg.inRate = 48000;
    cfg.inChannels = 2;
    cfg.codec = Codec::AC3;
    ProcessingCore core;
    std::string err;
    CHECK(core.configure(cfg, params, in, out, err));

    // Build an IEC 61937 AC-3 stream and present it as float "PCM" (what a bit-transparent
    // Windows mixer path delivers to loopback).
    AudioEncoder e;
    CHECK(e.open(Codec::AC3, 448000, 6, err));
    Planes p;
    double ph = 0;
    std::vector<uint8_t> burst(6144);
    std::vector<int16_t> words;
    for (int b = 0; b < 20; ++b) {
        p.tone(FC, 500, ph);
        Collected c;
        e.encodeBlock(p.ptr.data(), &Collected::sink, &c);
        iecPackBurst(IecType::AC3, c.packets[0].data(), c.packets[0].size(), burst.data(), burst.size());
        const auto* w = reinterpret_cast<const int16_t*>(burst.data());
        words.insert(words.end(), w, w + 3072);
    }
    std::vector<float> f(words.size());
    for (size_t i = 0; i < words.size(); ++i) f[i] = float(words[i]) / 32768.0f;

    uint64_t now = 0;
    std::vector<uint8_t> got;
    for (size_t off = 0; off < f.size(); off += 960) {
        in.write(f.data() + off, std::min<size_t>(960, f.size() - off));
        now += 10'000;
        core.step(now);
        std::vector<uint8_t> tmp(out.readAvailable());
        out.read(tmp.data(), tmp.size());
        got.insert(got.end(), tmp.begin(), tmp.end());
    }
    CHECK(core.stats.passthrough.load());
    CHECK_EQ(core.stats.requestedIec.load(), uint16_t(IecType::AC3));
    // After lock, the forwarded words must equal the source words exactly.
    const auto* gw = reinterpret_cast<const int16_t*>(got.data());
    const size_t gn = got.size() / 2;
    CHECK(gn > 3072 * 10);
    // find the last full burst in the output and compare with the source's last burst
    const int16_t* srcLast = words.data() + words.size() - 3072;
    bool found = false;
    for (size_t i = 0; i + 3072 <= gn; ++i) {
        if (uint16_t(gw[i]) == kIecPa && std::equal(srcLast, srcLast + 3072, gw + i)) { found = true; break; }
    }
    return found;
}

TEST(Pipeline, BitstreamPassthroughIsBitExact) {
    LiveParams params;
    CHECK(passthroughIsBitExact(params));
}

// v1.4.0: the custom EQ (and audition) never touch a pre-encoded bitstream.
TEST(Eq, PassthroughStaysBitExactWithEqOn) {
    LiveParams params;
    auto eq = eqDefaultConfig();
    eq.enabled = true;
    for (size_t ch = 0; ch < kBusChannels; ++ch)
        for (size_t b = 0; b < kEqBands; ++b) eq.bands[ch][b].gainDb = (b % 2 == 0) ? 6.0f : -6.0f;
    params.setEq(eq);
    params.bassDb = 9.0f;
    CHECK(passthroughIsBitExact(params));
}

// v1.1.0: "Lossless PCM 2.0 for stereo" asks the host to switch the output to PCM 2.0
// after 1.5 s of stereo-only content, and back to the bitstream format as soon as
// multichannel content appears.
TEST(Pipeline, PcmDirectRequestsOutputSwitch) {
    LiveParams params;
    params.upmixMode = static_cast<uint32_t>(UpmixMode::PcmDirect);
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    in.allocate(8 * 48000);
    out.allocate(16 * 48000);
    CoreConfig cfg;
    cfg.inRate = 48000;
    cfg.inChannels = 8;
    cfg.inMask = kMask71;
    cfg.codec = Codec::AC3;
    ProcessingCore core;
    std::string err;
    CHECK(core.configure(cfg, params, in, out, err));

    std::vector<float> chunk(8 * 480);
    std::vector<uint8_t> drain(out.capacity());
    double ph = 0;
    uint64_t now = 0;
    auto feed = [&](bool multichannel) {
        for (size_t i = 0; i < 480; ++i) {
            const float v = 0.3f * float(std::sin(ph));
            ph += 2 * std::numbers::pi * 440.0 / 48000.0;
            for (size_t c = 0; c < 8; ++c)
                chunk[i * 8 + c] = (c < 2 || (multichannel && c == 6)) ? v : 0.0f;
        }
        in.write(chunk.data(), chunk.size());
        now += 10'000;
        core.step(now);
        out.read(drain.data(), out.readAvailable());
    };

    for (int s = 0; s < 100; ++s) feed(false);          // 1 s of stereo: still inside the hold time
    CHECK_EQ(core.stats.outputRequest.load(), 0u);
    for (int s = 0; s < 100; ++s) feed(false);          // 2 s: switch to PCM requested
    CHECK_EQ(core.stats.outputRequest.load(), 1u);

    CoreConfig pcm = cfg;                                // host re-opens as PCM 2.0
    pcm.codec = Codec::PCM;
    pcm.pcmChannels = 2;
    pcm.stereoPcmOverride = true;
    CHECK(core.configure(pcm, params, in, out, err));
    for (int s = 0; s < 20; ++s) feed(false);
    CHECK_EQ(core.stats.outputRequest.load(), 0u);       // stereo keeps PCM
    for (int s = 0; s < 5; ++s) feed(true);
    CHECK_EQ(core.stats.outputRequest.load(), 2u);       // surround content -> back to bitstream
}

// ===========================================================================
// v1.2.0 - "Lossless PCM 2.0 for stereo", end to end.
//
// SimHost mirrors EngineHost exactly: planOutput() decides what to open, the core is
// (re)configured with that plan, effectiveStatus() gives the dashboard badge, and a
// StereoPcmSwitch ticked every 250 ms (the supervisor period) orders the re-open.
// The simulated renderer then checks what really leaves the engine: IEC 61937 bursts
// (Dolby Digital) or plain float PCM 2.0.
namespace {
struct SimHost {
    LiveParams& params;
    SpscRing<float>& in;
    SpscRing<uint8_t>& out;
    ProcessingCore& core;
    OutputRequest req{};
    StereoPcmSwitch sw{};
    bool stereoPcmActive = false;
    OutputStatus status = OutputStatus::Match;
    int reopens = 0;
    uint64_t lastReopenUs = 0;
    std::string err;

    bool open(uint64_t nowUs) {
        req.stereoPcmActive = stereoPcmActive;
        const OutputPlan plan = planOutput(req);
        if (stereoPcmActive && !plan.stereoPcm) stereoPcmActive = false;
        CoreConfig cc;
        cc.inRate = 48000;
        cc.inChannels = 8;
        cc.inMask = kMask71;
        cc.codec = plan.codec;
        cc.bitrate = 640000;
        cc.outputMode = req.mode;
        cc.pcmChannels = plan.pcmChannels;
        cc.stereoPcmOverride = plan.stereoPcm;
        cc.dedicatedSub = req.dedicatedSub;
        out.allocate(16 * 48000);   // the host allocates a fresh output ring per start
        out.reset();
        if (!core.configure(cc, params, in, out, err)) return false;
        status = effectiveStatus(true, false, false, req.passthroughIec, plan.stereoPcm);
        ++reopens;
        lastReopenUs = nowUs;
        return true;
    }
    void tick(uint64_t nowUs) {
        const auto act = sw.update(core.stats.outputRequest.load(), stereoPcmActive, nowUs);
        if (act == StereoPcmSwitch::Action::None) return;
        stereoPcmActive = act == StereoPcmSwitch::Action::SwitchToPcm;
        CHECK(open(nowUs));
    }
};

enum class Content { Stereo, Surround, StereoWithLfeLeak, Silence };

struct PcmSwitchRig {
    LiveParams params;
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    ProcessingCore core;
    SimHost host{params, in, out, core};
    std::vector<float> chunk = std::vector<float>(8 * 480);
    std::vector<uint8_t> rendered;
    double ph = 0;
    uint64_t now = 0;
    Iec61937Detector det;

    PcmSwitchRig() {
        params.upmixMode = static_cast<uint32_t>(UpmixMode::PcmDirect);
        in.allocate(8 * 48000);
        host.req.codec = Codec::AC3;
        host.req.mode = OutputMode::Exclusive;
        host.req.upmix = UpmixMode::PcmDirect;
        host.req.devicePcmChannels = 8;   // typical HDMI mix format
    }

    // Runs `ms` of audio in 10 ms capture packets / 10 ms render reads.
    void run(int ms, Content c) {
        for (int t = 0; t < ms; t += 10) {
            for (size_t i = 0; i < 480; ++i) {
                const float v = c == Content::Silence ? 0.0f : 0.3f * float(std::sin(ph));
                ph += 2 * std::numbers::pi * 440.0 / 48000.0;
                for (size_t ch = 0; ch < 8; ++ch) {
                    float x = 0.0f;
                    if (ch < 2) x = v;                                            // FL FR
                    if (c == Content::Surround && (ch == 2 || ch == 6)) x = v;   // C + SL
                    if (c == Content::StereoWithLfeLeak && ch == 3) x = v * 0.0018f;  // LFE at -55 dB
                    chunk[i * 8 + ch] = x;
                }
            }
            in.write(chunk.data(), chunk.size());
            now += 10'000;
            core.step(now);
            const size_t bytes = 480 * core.outputBytesPerBusFrame();
            rendered.resize(bytes);
            const size_t got = out.read(rendered.data(), bytes);
            rendered.resize(got);
            if (core.config().bitstream())
                det.feed(reinterpret_cast<const int16_t*>(rendered.data()), got / 2);
            if (now % 250'000 == 0) host.tick(now);
        }
    }
    // RMS of the left channel of the last PCM 2.0 render period.
    double pcmLeftRms() const {
        const auto* f = reinterpret_cast<const float*>(rendered.data());
        const size_t frames = rendered.size() / 8;
        double acc = 0;
        for (size_t i = 0; i < frames; ++i) acc += double(f[2 * i]) * f[2 * i];
        return frames ? std::sqrt(acc / double(frames)) : 0.0;
    }
};
} // namespace

TEST(Pipeline, PcmDirectEndToEndSwitchesOutput) {
    PcmSwitchRig rig;
    CHECK(rig.host.open(0));
    CHECK(rig.core.config().bitstream());
    CHECK(rig.host.status == OutputStatus::Match);

    // 1) Plain stereo: the output must actually be re-opened as PCM 2.0 within the
    //    transition window (1.5 s stable stereo + 2 supervisor ticks / 300 ms confirm).
    rig.run(3000, Content::Stereo);
    CHECK(rig.host.stereoPcmActive);
    CHECK_EQ(rig.host.reopens, 2);
    CHECK(rig.host.lastReopenUs <= 2'500'000);
    CHECK(rig.host.status == OutputStatus::StereoPcmDirect);           // dashboard badge
    CHECK(!rig.core.config().bitstream());                             // effective output...
    CHECK(rig.core.config().effectiveCodec() == Codec::PCM);
    CHECK_EQ(rig.core.config().pcmChannels, uint16_t(2));               // ...is PCM 2.0
    CHECK_EQ(rig.core.outputBytesPerBusFrame(), size_t(8));
    CHECK_EQ(rig.core.stats.emitChannels.load(), 2u);
    CHECK_EQ(rig.core.stats.pcmSwitchStatus.load(), uint32_t(PcmSwitchStatus::Active));
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::StereoPcm));
    CHECK_NEAR(rig.pcmLeftRms(), 0.3 / std::sqrt(2.0), 0.02);         // the music, unencoded

    // 2) Surround starts: back to Dolby Digital 5.1, and real IEC 61937 leaves the engine.
    rig.det.reset();
    rig.run(1500, Content::Surround);
    CHECK(!rig.host.stereoPcmActive);
    CHECK_EQ(rig.host.reopens, 3);
    CHECK(rig.host.status == OutputStatus::Match);
    CHECK(rig.core.config().bitstream());
    CHECK(rig.det.locked());
    CHECK(rig.det.type() == IecType::AC3);
    CHECK_EQ(rig.core.stats.encoderChannels.load(), 6u);

    // 3) Stereo with a little residue on LFE (-55 dB under the fronts) still counts as
    //    stereo (v1.1 treated any residue above -70 dBFS as surround and never switched).
    // (the surround content above is still inside the 1.5 s layout hold for a while)
    rig.run(4500, Content::StereoWithLfeLeak);
    CHECK(rig.host.stereoPcmActive);
    CHECK(rig.host.status == OutputStatus::StereoPcmDirect);
    CHECK(rig.core.config().effectiveCodec() == Codec::PCM);

    // 4) A game starts: back to the bitstream format and it stays there while gaming.
    rig.params.gamingActive = true;
    rig.run(3000, Content::Stereo);
    CHECK(!rig.host.stereoPcmActive);
    CHECK(rig.core.config().bitstream());
    CHECK_EQ(rig.core.stats.pcmSwitchStatus.load(), uint32_t(PcmSwitchStatus::BlockedGaming));
    const int reopens = rig.host.reopens;
    rig.run(3000, Content::Stereo);
    CHECK_EQ(rig.host.reopens, reopens);                               // no churn

    // 5) Silence does not flip the output either way (let the layout hold expire first).
    rig.run(2000, Content::Silence);
    rig.params.gamingActive = false;
    rig.run(3000, Content::Silence);
    CHECK_EQ(rig.host.reopens, reopens);
    CHECK_EQ(rig.core.stats.pcmSwitchStatus.load(), uint32_t(PcmSwitchStatus::NoSignal));
}

TEST(Pipeline, PcmDirectReportsWhySurroundBlocksTheSwitch) {
    PcmSwitchRig rig;
    CHECK(rig.host.open(0));
    rig.run(3000, Content::Surround);
    CHECK_EQ(rig.host.reopens, 1);
    CHECK_EQ(rig.core.stats.pcmSwitchStatus.load(), uint32_t(PcmSwitchStatus::BlockedSurround));
    const uint32_t blockers = rig.core.stats.pcmBlockMask.load();
    CHECK((blockers & SPK_FC) != 0);
    CHECK((blockers & SPK_SL) != 0);
    CHECK((blockers & (SPK_FL | SPK_FR)) == 0);
}

TEST(Pipeline, PcmDirectFallsBackCleanlyInSharedMode) {
    // Shared output cannot bitstream at all: the policy must never plan a PCM switch.
    OutputRequest r;
    r.codec = Codec::AC3;
    r.mode = OutputMode::Shared;
    r.upmix = UpmixMode::PcmDirect;
    r.stereoPcmActive = true;
    const auto p = planOutput(r);
    CHECK(!p.stereoPcm);
    CHECK(p.iec == IecType::None);
}

// ---------------------------------------------------------------------------
// v1.2.0 latency: the reported stages must match an impulse measured through the
// real core, the queue must be smaller than v1.1's for the same setting, and the
// smaller queue must not cost underruns or rescue blocks.
TEST(Pipeline, LatencyMatchesReportedStages) {
    struct Case { uint32_t bufferMs; uint32_t period; };
    for (const Case c : {Case{0, 240}, Case{15, 360}, Case{20, 480}}) {
        const auto r = avrsim::measurePipelineLatency(c.bufferMs, c.period, 20.0);
        std::printf("         margin %2u ms, period %4.1f ms: measured %.1f ms (%.1f..%.1f), reported %.1f ms, "
                    "target %.1f ms (floor %.1f)\n",
                    c.bufferMs, c.period / 48.0, r.measuredMs, r.measuredMinMs, r.measuredMaxMs, r.reportedMs,
                    r.targetMs, r.floorMs);
        CHECK(r.ok);
        CHECK(r.impulses > 40);
        CHECK_EQ(r.silenceInserts, 0u);
        CHECK_EQ(r.underruns, 0u);
        // Measured to the moment the sample plays: within half a render period of the
        // model (frames leave the queue a period at a time).
        CHECK(r.measuredMs >= r.reportedMs - 2.0);
        CHECK(r.measuredMs <= r.reportedMs + c.period / 48.0 + 2.0);
        // Smaller than the v1.1 queue (block + period + margin) for the same setting.
        const double oldTargetMs = 32.0 + c.period / 48.0 + (c.bufferMs ? c.bufferMs : 20);
        CHECK(r.targetMs < oldTargetMs);
        CHECK(r.marginMs >= r.floorMs);
    }
}

TEST(Pipeline, LatencySurvivesClockDrift) {
    // Render clock 200 ppm fast with the automatic (smallest) margin.
    const auto r = avrsim::measurePipelineLatency(0, 240, 40.0, 200.0);
    CHECK(r.ok);
    CHECK_EQ(r.underruns, 0u);
    CHECK_EQ(r.silenceInserts, 0u);
}

// ---------------------------------------------------------------------------
// v1.2.0: 5.0 encoding for setups without a subwoofer.
TEST(Mapping, Ac3FiveChannelLayout) {
    AudioEncoder e;
    std::string err;
    CHECK(e.open(Codec::AC3, 640000, 5, err));
    CHECK_EQ(e.channels(), 5);
    const AVCodec* dc = avcodec_find_decoder_by_name("ac3");
    AVCodecContext* d = avcodec_alloc_context3(dc);
    CHECK(avcodec_open2(d, dc, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    Planes p;
    double ph = 0;
    std::array<double, 8> energy{};
    int decodedChannels = 0;
    for (int blk = 0; blk < 10; ++blk) {
        p.tone(SL, 1000, ph);            // bus SL must become decoded channel 3 (FL FR FC SL SR)
        p.buf[LFE].assign(kBlockFrames, 0.5f);   // LFE plane must be ignored entirely
        Collected c;
        CHECK(e.encodeBlock(p.ptr.data(), &Collected::sink, &c));
        for (auto& data : c.packets) {
            av_new_packet(pkt, int(data.size()));
            std::copy(data.begin(), data.end(), pkt->data);
            if (avcodec_send_packet(d, pkt) == 0) {
                while (avcodec_receive_frame(d, fr) == 0) {
                    decodedChannels = fr->ch_layout.nb_channels;
                    if (blk < 3) continue;
                    for (int k = 0; k < decodedChannels && k < 8; ++k) {
                        const auto* s = reinterpret_cast<const float*>(fr->extended_data[k]);
                        for (int i = 0; i < fr->nb_samples; ++i) energy[size_t(k)] += double(s[i]) * s[i];
                    }
                }
            }
            av_packet_unref(pkt);
        }
    }
    CHECK_EQ(decodedChannels, 5);
    size_t best = 0;
    for (size_t k = 1; k < energy.size(); ++k) if (energy[k] > energy[best]) best = k;
    CHECK_EQ(best, size_t(3));
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&d);
}

namespace {
// Small rig for feature tests: 7.1 input at 48 kHz, AC-3 out, 10 ms steps.
struct FeatureRig {
    LiveParams params;
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    ProcessingCore core;
    std::vector<float> chunk = std::vector<float>(8 * 480);
    std::vector<uint8_t> drain;
    double ph = 0, phBass = 0;
    uint64_t now = 0;

    bool configure(bool dedicatedSub = true, Codec codec = Codec::AC3) {
        in.allocate(8 * 48000);
        out.allocate(16 * 48000);
        drain.assign(out.capacity(), 0);
        CoreConfig cfg;
        cfg.inRate = 48000;
        cfg.inChannels = 8;
        cfg.inMask = kMask71;
        cfg.codec = codec;
        cfg.pcmChannels = 6;
        cfg.dedicatedSub = dedicatedSub;
        std::string err;
        return core.configure(cfg, params, in, out, err);
    }
    // channelMask bit c => capture channel c (FL FR FC LFE BL BR SL SR) carries a
    // 1 kHz tone; bassMask => carries a 50 Hz tone.
    void run(int ms, uint32_t channelMask, uint32_t bassMask = 0) {
        for (int t = 0; t < ms; t += 10) {
            for (size_t i = 0; i < 480; ++i) {
                const float v = 0.25f * float(std::sin(ph));
                const float b = 0.25f * float(std::sin(phBass));
                ph += 2 * std::numbers::pi * 1000.0 / 48000.0;
                phBass += 2 * std::numbers::pi * 50.0 / 48000.0;
                for (size_t ch = 0; ch < 8; ++ch)
                    chunk[i * 8 + ch] = ((channelMask >> ch) & 1 ? v : 0.0f) + ((bassMask >> ch) & 1 ? b : 0.0f);
            }
            in.write(chunk.data(), chunk.size());
            now += 10'000;
            core.step(now);
            out.read(drain.data(), 480 * core.outputBytesPerBusFrame());   // 10 ms "renderer"
        }
    }
    float outRmsDb(size_t busCh) const { return linToDb(core.outputMeters.rms(busCh)); }
};
} // namespace

TEST(Features, NoSubwoofer_EncodesFiveChannelsAndFoldsLfe) {
    FeatureRig rig;
    rig.params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    CHECK(rig.configure(false));
    // Discrete 5.1 with LFE content (50 Hz on the LFE capture channel 3).
    rig.run(2000, 0b01000111, 0b00001000);
    CHECK_EQ(rig.core.stats.encoderChannels.load(), 5u);
    CHECK_EQ(rig.core.stats.emitChannels.load(), 5u);
    CHECK((rig.core.stats.emitMask.load() & SPK_LFE) == 0);
    CHECK(rig.outRmsDb(LFE) < -90.0f);                     // nothing left on the LFE plane
    CHECK(linToDb(rig.core.lfeFoldMeter.rms(0)) > -30.0f);  // ...because it went to the fronts
}

TEST(Features, LfeGeneration_AutoWaitsForConfirmed50Source) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    CHECK(rig.configure(true));
    // 5.0 source (FL FR C SL SR, no LFE) with bass in the centre.
    const uint32_t five0 = 0b11000111;
    rig.run(1000, five0, 0b00000100);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::Discrete));   // not confirmed yet
    rig.run(4000, five0, 0b00000100);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::LfeGen));
    CHECK(rig.core.stats.lfeGenGain.load() > 0.99f);
    CHECK(rig.outRmsDb(LFE) > -40.0f);
    // The source gains its own LFE: generation fades out and stops.
    rig.run(1500, five0 | 0b1000, 0b00000100);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::Discrete));
    CHECK(rig.core.stats.lfeGenGain.load() == 0.0f);
}

TEST(Features, LfeGeneration_OffNeverGenerates) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    CHECK(rig.configure(true));
    rig.run(5000, 0b11000111, 0b00000100);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::Discrete));
    CHECK(rig.outRmsDb(LFE) < -90.0f);
}

TEST(Features, LfeGeneration_AlwaysCoversKeptStereo) {
    FeatureRig rig;
    rig.params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Always);
    CHECK(rig.configure(true));
    rig.run(2000, 0b11, 0b11);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::StereoLfeGen));
    CHECK_EQ(rig.core.stats.encoderChannels.load(), 6u);   // 2.1 needs the 5.1 container
    CHECK(rig.outRmsDb(LFE) > -40.0f);
}

TEST(Features, GameMode_KeepsStereoUnprocessed) {
    FeatureRig rig;
    rig.params.upmixMode = static_cast<uint32_t>(UpmixMode::Upmix51);
    rig.params.gamingActive = true;
    CHECK(rig.configure(true));
    rig.run(1500, 0b11);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::StereoKept));
    CHECK_EQ(rig.core.stats.encoderChannels.load(), 6u);   // stable 5.1 container, no 2.0 <-> 5.1 flips
    CHECK(rig.outRmsDb(FC) < -90.0f);                      // nothing upmixed
    rig.params.gamingActive = false;
    rig.run(500, 0b11);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::StereoUpmix51));
    CHECK(rig.outRmsDb(FC) > -40.0f);
}

TEST(Features, ProcessingStateReflectsTheSource) {
    FeatureRig rig;
    rig.params.upmixMode = static_cast<uint32_t>(UpmixMode::Upmix51);
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    CHECK(rig.configure(true));
    rig.run(800, 0b11);
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::StereoUpmix51));
    rig.run(800, 0xFF);                                    // real 7.1
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::FoldDown71));
    rig.run(2000, 0b00001111);                             // 3.1: discrete, never upmixed
    CHECK_EQ(rig.core.stats.procState.load(), uint32_t(ProcessingState::Discrete));
}

TEST(Features, BassControlBoostsOnlyLowFrequencies) {
    FeatureRig flat, boosted;
    flat.params.upmixMode = boosted.params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    flat.params.stereoAsTwoChannel = boosted.params.stereoAsTwoChannel = false;
    boosted.params.bassDb = 6.0f;
    CHECK(flat.configure(true));
    CHECK(boosted.configure(true));
    flat.run(1500, 0, 0b11);                               // 50 Hz only
    boosted.run(1500, 0, 0b11);
    CHECK_NEAR(boosted.outRmsDb(FL) - flat.outRmsDb(FL), 5.7, 0.8);
    FeatureRig flatHi, boostedHi;
    flatHi.params.upmixMode = boostedHi.params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    boostedHi.params.bassDb = 6.0f;
    CHECK(flatHi.configure(true));
    CHECK(boostedHi.configure(true));
    flatHi.run(1500, 0b11);                                // 1 kHz only
    boostedHi.run(1500, 0b11);
    CHECK_NEAR(boostedHi.outRmsDb(FL) - flatHi.outRmsDb(FL), 0.0, 0.3);
}

TEST(Features, FrequencyRangeFollowsTheInput) {
    FeatureRig rig;
    rig.params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    CHECK(rig.configure(true));
    rig.run(3000, 0, 0b11);                                // 50 Hz on FL/FR
    const float lo = rig.core.frequencyRange.lowHz(), hi = rig.core.frequencyRange.highHz();
    std::printf("         50 Hz tone -> range %.0f .. %.0f Hz\n", lo, hi);
    CHECK(lo >= 20.0f && lo <= 50.0f);
    CHECK(hi >= 50.0f && hi <= 160.0f);
    rig.run(8000, 0b11, 0b11);                             // add 1 kHz
    CHECK(rig.core.frequencyRange.highHz() > 900.0f);
    CHECK(rig.core.frequencyRange.highHz() < 1200.0f);
}

// ===========================================================================
// v1.3.0 - native-LFE inhibit and Game Mode's LFE toggle.
// Capture channels (7.1 endpoint): FL FR FC LFE BL BR SL SR = bits 0..7.
namespace {
constexpr uint32_t k51QuietLfe = 0b11000111;   // FL FR FC SL SR: a 5.1 source whose LFE is silent
constexpr uint32_t k51WithLfe = 0b11001111;    // ...and with its LFE active
LfeStatus lfeStatusOf(const FeatureRig& r) { return static_cast<LfeStatus>(r.core.stats.lfeStatus.load()); }
ProcessingState procOf(const FeatureRig& r) { return static_cast<ProcessingState>(r.core.stats.procState.load()); }
} // namespace

TEST(LfeInhibit, NativeLfeStopsAutoGenerationForTheSession) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    CHECK(rig.configure(true));
    // A 5.1 source with a quiet LFE looks like 5.0 by activity: Auto generates (v1.2 behaviour).
    rig.run(4000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::LfeGen);
    CHECK(lfeStatusOf(rig) == LfeStatus::Generated);
    // Its own LFE becomes active: generation stops and the latch is set.
    rig.run(1000, k51WithLfe);
    CHECK(rig.params.lfeInhibited.load());
    CHECK(lfeStatusOf(rig) == LfeStatus::Source);
    // The native LFE goes quiet again while the film continues: generation must NOT come
    // back (v1.2 resumed it after the 1.5 s layout hold + 3 s confirmation).
    rig.run(8000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::Discrete);
    CHECK(rig.core.stats.lfeGenGain.load() == 0.0f);
    CHECK(lfeStatusOf(rig) == LfeStatus::Inhibited);
    CHECK(rig.outRmsDb(LFE) < -90.0f);
}

TEST(LfeInhibit, ResumesAfterDefaultSilenceInterval) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    CHECK(rig.configure(true));
    CHECK_NEAR(rig.params.lfeResetSeconds.load(), 10.0, 1e-6);   // default
    rig.run(1000, k51WithLfe);
    CHECK(rig.params.lfeInhibited.load());
    rig.run(9000, 0);                                            // 9 s of digital silence
    CHECK(rig.params.lfeInhibited.load());                       // not yet
    rig.run(2000, 0);                                            // 11 s
    CHECK(!rig.params.lfeInhibited.load());
    // Auto detection resumes for a genuine 5.0 source (3 s confirmation + 0.5 s fade).
    rig.run(4000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::LfeGen);
    CHECK(rig.core.stats.lfeGenGain.load() > 0.99f);
}

TEST(LfeInhibit, AnySoundRestartsTheSilenceInterval) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    CHECK(rig.configure(true));
    rig.run(1000, k51WithLfe);
    rig.run(8000, 0);
    rig.run(100, 0b1);                                           // a short sound on FL only
    rig.run(8000, 0);                                            // 8 s since that sound
    CHECK(rig.params.lfeInhibited.load());
    rig.run(2500, 0);
    CHECK(!rig.params.lfeInhibited.load());
}

TEST(LfeInhibit, ConfiguredIntervalIsApplied) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    rig.params.lfeResetSeconds = 20.0f;
    CHECK(rig.configure(true));
    rig.run(1000, k51WithLfe);
    rig.run(15000, 0);
    CHECK(rig.params.lfeInhibited.load());
    rig.run(6000, 0);                                            // 21 s
    CHECK(!rig.params.lfeInhibited.load());
    rig.params.lfeResetSeconds = 0.2f;                           // v1.4.0: below the 1 s minimum -> 1 s
    rig.run(1000, k51WithLfe);
    rig.run(500, 0);
    CHECK(rig.params.lfeInhibited.load());
    rig.run(1000, 0);                                            // 1.5 s of silence
    CHECK(!rig.params.lfeInhibited.load());
}

// v1.4.0: whole seconds 1..300, default (and recommendation) 10.
TEST(LfeInhibit, ResetIntervalBoundaries) {
    CHECK_NEAR(clampLfeResetSeconds(0.0f), 1.0, 1e-6);
    CHECK_NEAR(clampLfeResetSeconds(1.0f), 1.0, 1e-6);
    CHECK_NEAR(clampLfeResetSeconds(10.0f), 10.0, 1e-6);
    CHECK_NEAR(clampLfeResetSeconds(10.4f), 10.0, 1e-6);
    CHECK_NEAR(clampLfeResetSeconds(300.0f), 300.0, 1e-6);
    CHECK_NEAR(clampLfeResetSeconds(301.0f), 300.0, 1e-6);
    CHECK_NEAR(clampLfeResetSeconds(600.0f), 300.0, 1e-6);      // 1.3 allowed up to 600
    CHECK_NEAR(clampLfeResetSeconds(std::nanf("")), 10.0, 1e-6);
    CHECK_NEAR(LiveParams{}.lfeResetSeconds.load(), 10.0, 1e-6);

    // 1 s: released after just over a second of silence.
    FeatureRig one;
    one.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    one.params.lfeResetSeconds = 1.0f;
    CHECK(one.configure(true));
    one.run(1000, k51WithLfe);
    one.run(700, 0);
    CHECK(one.params.lfeInhibited.load());
    one.run(600, 0);
    CHECK(!one.params.lfeInhibited.load());

    // 300 s: the clock jumps with no input (as when Windows plays nothing at all).
    FeatureRig longest;
    longest.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    longest.params.lfeResetSeconds = 300.0f;
    CHECK(longest.configure(true));
    longest.run(1000, k51WithLfe);
    const uint64_t t0 = longest.now;
    longest.now = t0 + 299'000'000;
    longest.core.step(longest.now);
    CHECK(longest.params.lfeInhibited.load());
    longest.now = t0 + 301'000'000;
    longest.core.step(longest.now);
    CHECK(!longest.params.lfeInhibited.load());
}

TEST(LfeInhibit, SurvivesPipelineReconfiguration) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    CHECK(rig.configure(true));
    rig.run(1000, k51WithLfe);
    CHECK(rig.configure(true));                                  // e.g. format change / Lossless PCM re-open
    CHECK(rig.params.lfeInhibited.load());
    rig.run(6000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::Discrete);
    CHECK(rig.core.stats.lfeGenGain.load() == 0.0f);
}

TEST(LfeInhibit, OffAndAlwaysKeepTheirBehaviour) {
    FeatureRig off;
    off.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    CHECK(off.configure(true));
    off.run(5000, k51QuietLfe);
    CHECK(procOf(off) == ProcessingState::Discrete);
    CHECK(lfeStatusOf(off) == LfeStatus::None);
    // The user's general "Always" is not affected by the latch (unchanged v1.2 behaviour).
    FeatureRig always;
    always.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Always);
    CHECK(always.configure(true));
    always.run(1000, k51WithLfe);
    always.run(3000, k51QuietLfe);
    CHECK(procOf(always) == ProcessingState::LfeGen);
}

TEST(GameLfe, OverridesGeneralSettingOnlyWhileGaming) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    rig.params.gameLfeGen = true;
    CHECK(rig.configure(true));
    rig.run(3000, k51QuietLfe);                                  // no game: general "Off" applies
    CHECK(procOf(rig) == ProcessingState::Discrete);
    CHECK(!rig.core.stats.lfeGameOverride.load());
    rig.params.gamingActive = true;                              // game: generation forced on, immediately
    rig.run(1000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::LfeGen);
    CHECK(rig.core.stats.lfeGameOverride.load());
    CHECK(lfeStatusOf(rig) == LfeStatus::Generated);
    rig.run(2500, 0b11);                                         // stereo game audio (after the 1.5 s layout hold) -> 2.1
    CHECK(procOf(rig) == ProcessingState::StereoLfeGen);
    rig.params.gamingActive = false;                             // game over: back to "Off"
    rig.run(1500, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::Discrete);
    CHECK(rig.core.stats.lfeGenGain.load() == 0.0f);
    // With the toggle on but no game, the general Auto works exactly as before.
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Auto);
    rig.run(4000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::LfeGen);
    CHECK(!rig.core.stats.lfeGameOverride.load());
}

TEST(GameLfe, RespectsNativeLfeInhibit) {
    FeatureRig rig;
    rig.params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    rig.params.gameLfeGen = true;
    rig.params.gamingActive = true;
    CHECK(rig.configure(true));
    rig.run(1000, k51WithLfe);                                   // the game has its own LFE
    rig.run(3000, k51QuietLfe);
    CHECK(procOf(rig) == ProcessingState::Discrete);
    CHECK(lfeStatusOf(rig) == LfeStatus::Inhibited);
    rig.params.gameLfeGen = false;                               // toggle off: general setting (Off)
    rig.run(500, k51QuietLfe);
    CHECK(lfeStatusOf(rig) == LfeStatus::None);
}

// ===========================================================================
// v1.4.0 - custom per-channel EQ
namespace {
void setBand(LiveParams& p, size_t ch, size_t band, float gainDb, float hz = 0.0f, float q = 0.0f) {
    auto b = eqDefaultBand(ch, band);
    b.gainDb = gainDb;
    if (hz > 0) b.freqHz = hz;
    if (q > 0) b.q = q;
    p.setEqBand(ch, band, b);
    p.eqRevision.fetch_add(1);
}
struct EqBus {
    std::array<std::vector<float>, kBusChannels> buf;
    std::array<float*, kBusChannels> ptr{};
    double ph = 0;
    EqBus() { for (size_t c = 0; c < kBusChannels; ++c) { buf[c].assign(kBlockFrames, 0.0f); ptr[c] = buf[c].data(); } }
    void tone(double hz, uint32_t mask, float amp = 0.25f) {
        for (size_t i = 0; i < kBlockFrames; ++i) {
            const float v = amp * float(std::sin(ph));
            ph += 2 * std::numbers::pi * hz / 48000.0;
            for (size_t c = 0; c < kBusChannels; ++c) buf[c][i] = (mask >> c) & 1 ? v : 0.0f;
        }
    }
    double rmsDb(size_t c) const {
        double acc = 0;
        for (float v : buf[c]) acc += double(v) * v;
        return 10 * std::log10(acc / double(kBlockFrames) + 1e-30);
    }
};
} // namespace

TEST(Eq, DefaultsAreFlatAndLimitsHold) {
    const auto def = eqDefaultConfig();
    CHECK(!def.enabled);
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        CHECK(def.channelOn[ch]);
        for (size_t b = 0; b < kEqBands; ++b) {
            CHECK_NEAR(def.bands[ch][b].gainDb, 0.0, 1e-9);
            CHECK(def.bands[ch][b].freqHz >= kEqFreqMinHz && def.bands[ch][b].freqHz <= eqFreqMax(ch));
        }
    }
    CHECK(eqBandType(0) == EqBandType::LowShelf);
    CHECK(eqBandType(2) == EqBandType::Peak);
    CHECK(eqBandType(4) == EqBandType::HighShelf);
    const auto c = eqClampBand(LFE, 2, EqBand{5000.0f, 9.0f, 10.0f});
    CHECK_NEAR(c.freqHz, 200.0, 1e-6);                 // LFE limited to 200 Hz
    CHECK_NEAR(c.gainDb, 6.0, 1e-6);                   // +/-6 dB
    CHECK_NEAR(c.q, 4.0, 1e-6);                        // Q 0.5..4
    const auto n = eqClampBand(FL, 1, EqBand{std::nanf(""), -20.0f, 0.1f});
    CHECK_NEAR(n.freqHz, eqDefaultBand(FL, 1).freqHz, 1e-6);
    CHECK_NEAR(n.gainDb, -6.0, 1e-6);
    CHECK_NEAR(n.q, 0.5, 1e-6);
    // Every band type is exact unity at 0 dB.
    for (size_t b = 0; b < kEqBands; ++b)
        CHECK_NEAR(eqCoeffs(eqBandType(b), 48000, eqDefaultBand(FL, b)).magnitude(48000, 1234.0), 1.0, 1e-5);
    const auto bell = eqCoeffs(EqBandType::Peak, 48000, EqBand{1000.0f, 6.0f, 1.0f});
    CHECK_NEAR(20 * std::log10(bell.magnitude(48000, 1000)), 6.0, 0.05);
    const auto hs = eqCoeffs(EqBandType::HighShelf, 48000, EqBand{2000.0f, -6.0f, 1.0f});
    CHECK_NEAR(20 * std::log10(hs.magnitude(48000, 15000)), -6.0, 0.3);
    CHECK_NEAR(20 * std::log10(hs.magnitude(48000, 100)), 0.0, 0.1);
}

TEST(Eq, BypassIsBitExactAndKeepsSettings) {
    LiveParams p;
    setBand(p, FL, 2, 6.0f, 1000.0f);
    ChannelEqBank bank;
    bank.pull(p);                                    // master off: settings stored, nothing applied
    EqBus bus;
    for (int blk = 0; blk < 10; ++blk) {
        bus.tone(1000, 0b11);
        auto ref = bus.buf;
        bank.process(bus.ptr.data(), kBlockFrames);
        for (size_t c = 0; c < kBusChannels; ++c) CHECK(bus.buf[c] == ref[c]);
    }
    CHECK_EQ(bank.activeMask(), 0u);
    // Master on but every band flat: still untouched.
    LiveParams flat;
    flat.eqEnabled = true;
    flat.eqRevision.fetch_add(1);
    ChannelEqBank bank2;
    bank2.pull(flat);
    bus.tone(1000, 0x3F);
    auto ref = bus.buf;
    bank2.process(bus.ptr.data(), kBlockFrames);
    for (size_t c = 0; c < kBusChannels; ++c) CHECK(bus.buf[c] == ref[c]);
    // Turning the master on applies the stored band; off again glides back and bypasses,
    // and the stored target survives the bypass.
    p.eqEnabled = true;
    bank.pull(p);
    for (int blk = 0; blk < 20; ++blk) { bus.tone(1000, 0b11); bank.process(bus.ptr.data(), kBlockFrames); }
    CHECK_NEAR(bank.current(FL, 2).gainDb, 6.0, 1e-6);
    CHECK((bank.activeMask() & 1u) != 0);
    p.eqEnabled = false;
    bank.pull(p);
    for (int blk = 0; blk < 30; ++blk) { bus.tone(1000, 0b11); bank.process(bus.ptr.data(), kBlockFrames); }
    CHECK_EQ(bank.activeMask(), 0u);
    CHECK_NEAR(bank.current(FL, 2).gainDb, 0.0, 1e-9);
    CHECK_NEAR(p.eqBand[LiveParams::eqIndex(FL, 2, 1)].load(), 6.0, 1e-6);   // setting kept
    // Per-channel bypass works the same way.
    p.eqEnabled = true;
    p.eqChannelEnabled[FL] = false;
    bank.pull(p);
    for (int blk = 0; blk < 10; ++blk) { bus.tone(1000, 0b11); bank.process(bus.ptr.data(), kBlockFrames); }
    CHECK_EQ(bank.activeMask(), 0u);
}

TEST(Eq, BoostIsCompensatedByHeadroomAndCutIsExact) {
    LiveParams p;
    p.eqEnabled = true;
    setBand(p, FL, 2, 6.0f, 1000.0f, 1.0f);
    ChannelEqBank bank;
    bank.pull(p);
    EqBus bus;
    for (int blk = 0; blk < 30; ++blk) { bus.tone(1000, 0b11); bank.process(bus.ptr.data(), kBlockFrames); }
    // FL is boosted 6 dB relative to FR, and the common headroom keeps FL at its input level.
    CHECK_NEAR(bus.rmsDb(FL) - bus.rmsDb(FR), 6.0, 0.3);
    CHECK_NEAR(bank.headroomDb(), -6.0, 0.3);
    const double inDb = 10 * std::log10(0.25 * 0.25 / 2);
    CHECK_NEAR(bus.rmsDb(FL), inDb, 0.4);
    CHECK_NEAR(bus.rmsDb(FR), inDb - 6.0, 0.4);
    // Cuts need no headroom.
    LiveParams c;
    c.eqEnabled = true;
    setBand(c, FR, 2, -6.0f, 1000.0f, 1.0f);
    ChannelEqBank cut;
    cut.pull(c);
    for (int blk = 0; blk < 30; ++blk) { bus.tone(1000, 0b11); cut.process(bus.ptr.data(), kBlockFrames); }
    CHECK_NEAR(cut.headroomDb(), 0.0, 1e-6);
    CHECK_NEAR(bus.rmsDb(FL), inDb, 0.1);
    CHECK_NEAR(bus.rmsDb(FR), inDb - 6.0, 0.3);
}

TEST(Eq, ChangesAreClickFree) {
    // A full +/-6 dB swing of a 60 Hz low shelf while a 60 Hz tone plays: the waveform must
    // never step - the largest sample-to-sample change stays close to the tone's own.
    LiveParams p;
    p.eqEnabled = true;
    ChannelEqBank bank;
    bank.pull(p);
    EqBus bus;
    float last = 0.0f;
    double worst = 0.0;
    auto run = [&](int blocks) {
        for (int blk = 0; blk < blocks; ++blk) {
            bus.tone(60, 0b1, 0.4f);
            bank.process(bus.ptr.data(), kBlockFrames);
            for (float v : bus.buf[FL]) { worst = std::max(worst, double(std::fabs(v - last))); last = v; }
        }
    };
    run(5);
    setBand(p, FL, 0, 6.0f, 60.0f);  bank.pull(p); run(20);
    setBand(p, FL, 0, -6.0f, 60.0f); bank.pull(p); run(30);
    p.eqEnabled = false;             bank.pull(p); run(20);
    const double toneStep = 0.4 * 2 * std::numbers::pi * 60.0 / 48000.0;   // max slope of the input
    std::printf("         worst step %.5f vs tone step %.5f\n", worst, toneStep);
    CHECK(worst < toneStep * 2.5);
}

// The EQ runs on the routed / upmixed output channels in every PCM mode: a -6 dB cut at
// 1 kHz on FL lowers FL by 6 dB whether stereo is kept, upmixed, or the source is 5.1.
TEST(Eq, AppliesInEveryProcessingMode) {
    struct Mode { UpmixMode upmix; uint32_t mask; ProcessingState expect; };
    const Mode modes[] = {
        {UpmixMode::Passthrough, 0b1, ProcessingState::StereoKept},
        {UpmixMode::Upmix51, 0b1, ProcessingState::StereoUpmix51},
        {UpmixMode::Passthrough, 0b11000111, ProcessingState::Discrete},   // real 5.1 (FL FR C SL SR)
    };
    for (const auto& m : modes) {
        FeatureRig flat, eq;
        for (FeatureRig* r : {&flat, &eq}) {
            r->params.upmixMode = static_cast<uint32_t>(m.upmix);
            r->params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
            r->params.stereoAsTwoChannel = false;
        }
        eq.params.eqEnabled = true;
        setBand(eq.params, FL, 2, -6.0f, 1000.0f, 1.0f);
        CHECK(flat.configure(true));
        CHECK(eq.configure(true));
        flat.run(2500, m.mask);
        eq.run(2500, m.mask);
        CHECK_EQ(eq.core.stats.procState.load(), uint32_t(m.expect));
        const double d = eq.outRmsDb(FL) - flat.outRmsDb(FL);
        std::printf("         mode %u: FL %.2f dB\n", unsigned(m.expect), d);
        CHECK_NEAR(d, -6.0, 0.6);
        CHECK_NEAR(eq.core.stats.eqHeadroomDb.load(), 0.0, 1e-6);
    }
}

TEST(Eq, ReplacesTheBassControl) {
    FeatureRig flat, bass, eqOn;
    for (FeatureRig* r : {&flat, &bass, &eqOn}) {
        r->params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
        r->params.stereoAsTwoChannel = false;
        r->params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    }
    bass.params.bassDb = eqOn.params.bassDb = 6.0f;
    eqOn.params.eqEnabled = true;          // flat EQ: the bass setting must have no effect
    eqOn.params.eqRevision.fetch_add(1);
    CHECK(flat.configure(true));
    CHECK(bass.configure(true));
    CHECK(eqOn.configure(true));
    flat.run(1500, 0, 0b11);               // 50 Hz
    bass.run(1500, 0, 0b11);
    eqOn.run(1500, 0, 0b11);
    CHECK_NEAR(bass.outRmsDb(FL) - flat.outRmsDb(FL), 5.7, 0.8);
    CHECK_NEAR(eqOn.outRmsDb(FL) - flat.outRmsDb(FL), 0.0, 0.2);
    CHECK_NEAR(eqOn.params.bassDb.load(), 6.0, 1e-6);   // the bass setting itself is kept
}

TEST(Eq, AuditionSolosTemporarilyWithoutTouchingMutes) {
    FeatureRig rig;
    rig.params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    rig.params.stereoAsTwoChannel = false;
    CHECK(rig.configure(true));
    rig.run(1000, 0b11);
    CHECK(rig.outRmsDb(FL) > -30.0f);
    rig.params.auditionChannel = FR;
    rig.run(3000, 0b11);                                 // the 300 ms RMS meter needs time to fall
    CHECK(rig.outRmsDb(FL) < -50.0f);
    CHECK(rig.outRmsDb(FR) > -30.0f);
    CHECK(!rig.params.mute[FL].load());                 // saved mutes untouched
    rig.params.auditionChannel = -1;
    rig.run(1500, 0b11);
    CHECK(rig.outRmsDb(FL) > -30.0f);
}
