// Unit tests: lock-free ring, IEC 61937, DSP blocks, IPC protocol + layout.
#include "TestFramework.h"

#include "../engine/core/BassManager.h"
#include "../engine/core/Biquad.h"
#include "../engine/core/ChannelMatrix.h"
#include "../engine/core/Dynamics.h"
#include "../engine/core/FrequencyRange.h"
#include "../engine/core/Iec61937.h"
#include "../engine/core/OutputPolicy.h"
#include "../engine/core/SpscRing.h"
#include "../engine/core/StatusText.h"
#include "../engine/core/TestSignals.h"
#include "../engine/core/Upmixer.h"
#include "../engine/ipc/ControlProtocol.h"
#include "../engine/ipc/TelemetryLayout.h"
#include "../engine/win/ConfigStore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <random>
#include <thread>
#include <utility>
#include <vector>

using namespace avr;

namespace {
constexpr double kPi = std::numbers::pi;

struct Bus {
    std::array<std::vector<float>, kBusChannels> buf;
    std::array<float*, kBusChannels> ptr{};
    explicit Bus(size_t n) {
        for (size_t c = 0; c < kBusChannels; ++c) { buf[c].assign(n, 0.0f); ptr[c] = buf[c].data(); }
    }
    double rms(size_t c, size_t from = 0) const {
        double acc = 0;
        for (size_t i = from; i < buf[c].size(); ++i) acc += double(buf[c][i]) * buf[c][i];
        return std::sqrt(acc / double(buf[c].size() - from));
    }
};
} // namespace

// ---------------------------------------------------------------------------
TEST(SpscRing, WrapAround) {
    SpscRing<int> r;
    r.allocate(8);
    CHECK_EQ(r.capacity(), size_t(8));
    int in[6] = {1, 2, 3, 4, 5, 6}, out[6] = {};
    CHECK_EQ(r.write(in, 6), size_t(6));
    CHECK_EQ(r.read(out, 4), size_t(4));
    CHECK_EQ(r.write(in, 6), size_t(6));   // wraps
    CHECK_EQ(r.writeAvailable(), size_t(0));
    CHECK_EQ(r.write(in, 1), size_t(0));   // full
    CHECK_EQ(r.read(out, 2), size_t(2));
    CHECK_EQ(out[0], 5);
    CHECK_EQ(out[1], 6);
    CHECK_EQ(r.read(out, 6), size_t(6));
    for (int i = 0; i < 6; ++i) CHECK_EQ(out[i], in[i]);
}

TEST(SpscRing, ConcurrentStress) {
    SpscRing<uint32_t> r;
    r.allocate(4096);
    constexpr uint32_t kTotal = 5'000'000;
    bool ok = true;
    std::thread consumer([&] {
        uint32_t expect = 0;
        std::array<uint32_t, 333> buf{};
        while (expect < kTotal) {
            const size_t n = r.read(buf.data(), buf.size());
            for (size_t i = 0; i < n; ++i) if (buf[i] != expect++) ok = false;
        }
    });
    std::array<uint32_t, 257> chunk{};
    uint32_t next = 0;
    while (next < kTotal) {
        const size_t want = std::min<size_t>(chunk.size(), kTotal - next);
        for (size_t i = 0; i < want; ++i) chunk[i] = next + uint32_t(i);
        size_t done = 0;
        while (done < want) done += r.write(chunk.data() + done, want - done);
        next += uint32_t(want);
    }
    consumer.join();
    CHECK(ok);
}

// ---------------------------------------------------------------------------
TEST(Iec61937, PackAc3Burst) {
    std::vector<uint8_t> frame(2560);
    for (size_t i = 0; i < frame.size(); ++i) frame[i] = uint8_t(i * 7);
    frame[0] = 0x0B; frame[1] = 0x77;
    std::vector<uint8_t> out(kIecMaxBurstBytes);
    const size_t n = iecPackBurst(IecType::AC3, frame.data(), frame.size(), out.data(), out.size(), 0);
    CHECK_EQ(n, size_t(6144));
    CHECK_EQ(out[0], 0x72); CHECK_EQ(out[1], 0xF8);   // Pa 0xF872 LE
    CHECK_EQ(out[2], 0x1F); CHECK_EQ(out[3], 0x4E);   // Pb 0x4E1F LE
    CHECK_EQ(out[4], 0x01); CHECK_EQ(out[5], 0x00);   // Pc AC-3, bsmod 0
    const uint16_t pd = uint16_t(out[6] | out[7] << 8);
    CHECK_EQ(pd, uint16_t(2560 * 8));                 // Pd in bits
    CHECK_EQ(out[8], 0x77); CHECK_EQ(out[9], 0x0B);   // sync word byte-swapped
    for (size_t i = 8 + 2560; i < 6144; ++i) CHECK_EQ(out[i], 0);
}

TEST(Iec61937, PackEac3UsesBytesAndDtsRaw) {
    std::vector<uint8_t> f(4000, 0x55), out(kIecMaxBurstBytes);
    CHECK_EQ(iecPackBurst(IecType::EAC3, f.data(), f.size(), out.data(), out.size()), size_t(24576));
    CHECK_EQ(uint16_t(out[6] | out[7] << 8), uint16_t(4000));            // Pd in bytes for E-AC-3
    std::vector<uint8_t> dts(2048, 0x11);
    dts[0] = 0x7F; dts[1] = 0xFE; dts[2] = 0x80; dts[3] = 0x01;
    CHECK_EQ(iecPackBurst(IecType::DTS1, dts.data(), dts.size(), out.data(), out.size()), size_t(2048));
    CHECK_EQ(out[0], 0xFE); CHECK_EQ(out[1], 0x7F);                         // raw: no Pa, swapped sync
    std::vector<uint8_t> tooBig(2050);
    CHECK_EQ(iecPackBurst(IecType::DTS1, tooBig.data(), tooBig.size(), out.data(), out.size()), size_t(0));
}

TEST(Iec61937, DetectorRoundTripAndLock) {
    std::mt19937 rng(42);
    std::vector<uint8_t> frame(1792);
    std::vector<int16_t> stream;
    std::vector<uint8_t> burst(6144);
    for (int b = 0; b < 4; ++b) {
        for (auto& x : frame) x = uint8_t(rng());
        frame[0] = 0x0B; frame[1] = 0x77;
        iecPackBurst(IecType::AC3, frame.data(), frame.size(), burst.data(), burst.size());
        const auto* w = reinterpret_cast<const int16_t*>(burst.data());
        stream.insert(stream.end(), w, w + burst.size() / 2);
    }
    Iec61937Detector d;
    size_t bursts = 0;
    bool payloadOk = false;
    for (size_t i = 0; i < stream.size(); i += 500) {
        bursts += d.feed(stream.data() + i, std::min<size_t>(500, stream.size() - i));
        if (d.frameReady()) {
            payloadOk = d.frameBytes() == frame.size() && d.frameData()[0] == 0x0B && d.frameData()[1] == 0x77;
            d.consumeFrame();
        }
    }
    CHECK_EQ(bursts, size_t(4));
    CHECK(payloadOk);
    CHECK(d.locked());
    CHECK(d.type() == IecType::AC3);
    // last frame must match exactly
    CHECK(std::equal(frame.begin(), frame.end(), d.frameData()));
    // Silence for > 4 periods drops the lock.
    std::vector<int16_t> zeros(3072 * 5, 0);
    d.feed(zeros.data(), zeros.size());
    CHECK(!d.locked());
}

TEST(Iec61937, NoFalseLockOnNoise) {
    std::mt19937 rng(7);
    std::vector<int16_t> noise(48000 * 2 * 5);
    for (auto& s : noise) s = int16_t(rng());
    Iec61937Detector d;
    d.feed(noise.data(), noise.size());
    CHECK(!d.locked());
}

// ---------------------------------------------------------------------------
TEST(Dsp, LinkwitzRileyResponse) {
    const auto c = BiquadCoeffs::lowpass(48000, 80, std::numbers::sqrt2 / 2);
    const double at80 = 20 * std::log10(c.magnitude(48000, 80) * c.magnitude(48000, 80));
    const double at20 = 20 * std::log10(c.magnitude(48000, 20) * c.magnitude(48000, 20));
    const double at640 = 20 * std::log10(c.magnitude(48000, 640) * c.magnitude(48000, 640));
    CHECK_NEAR(at80, -6.02, 0.1);    // LR4 is -6 dB at the crossover
    CHECK_NEAR(at20, 0.0, 0.1);
    CHECK(at640 < -70.0);             // 24 dB/oct, 3 octaves above
}

TEST(Dsp, Matrix71FoldDown) {
    ChannelMatrix m;
    m.configure(8, kMask71);
    CHECK(m.is71());
    CHECK(!m.isStereoEndpoint());
    // feed 1.0 on BL (index 4) and SL (index 6) only
    std::vector<float> in(8 * 4, 0.0f);
    for (int i = 0; i < 4; ++i) { in[i * 8 + 4] = 1.0f; in[i * 8 + 6] = 1.0f; in[i * 8 + 0] = 0.5f; }
    Bus bus(4);
    m.apply(in.data(), 4, bus.ptr.data());
    CHECK_NEAR(bus.buf[SL][0], 2 * 0.70710678, 1e-5);
    CHECK_NEAR(bus.buf[SR][0], 0.0, 1e-9);
    CHECK_NEAR(bus.buf[FL][0], 0.5, 1e-9);
    CHECK_EQ(std::string(ChannelMatrix::layoutName(kMask71)), std::string("PCM 7.1"));
    CHECK_EQ(std::string(ChannelMatrix::layoutName(kMaskStereo)), std::string("PCM 2.0"));
    CHECK_EQ(std::string(ChannelMatrix::layoutName(kMask51Side)), std::string("PCM 5.1"));
}

TEST(Dsp, Matrix51BackMapsToSurround) {
    ChannelMatrix m;
    m.configure(6, kMask51Back);
    std::vector<float> in(6, 0.0f);
    in[4] = 0.25f; in[5] = -0.25f;   // BL, BR
    Bus bus(1);
    m.apply(in.data(), 1, bus.ptr.data());
    CHECK_NEAR(bus.buf[SL][0], 0.25, 1e-9);
    CHECK_NEAR(bus.buf[SR][0], -0.25, 1e-9);
}

static void fillTone(Bus& b, double freq, float l, float r) {
    for (size_t i = 0; i < b.buf[0].size(); ++i) {
        const float s = float(std::sin(2 * kPi * freq * double(i) / 48000.0));
        b.buf[FL][i] = l * s;
        b.buf[FR][i] = r * s;
    }
}

TEST(Upmixer, CentredContentGoesToCentre) {
    Upmixer u;
    u.configure(UpmixSettings{});
    Bus b(48000);
    fillTone(b, 1000, 0.5f, 0.5f);
    u.process(b.ptr.data(), b.buf[0].size(), UpmixMode::Upmix51);
    const size_t settle = 24000;   // steering is deliberately slow (anti-pumping)
    CHECK(b.rms(FC, settle) > 0.3);
    CHECK(b.rms(FL, settle) < 0.1);           // phantom image moved into C
    CHECK(b.rms(SL, settle) < 0.01);          // nothing centred leaks to surrounds
    CHECK(u.correlation() > 0.95f);
}

TEST(Upmixer, AntiPhaseGoesToSurrounds) {
    Upmixer u;
    u.configure(UpmixSettings{});
    Bus b(48000);
    fillTone(b, 1000, 0.5f, -0.5f);
    u.process(b.ptr.data(), b.buf[0].size(), UpmixMode::Upmix51);
    const size_t settle = 24000;   // steering is deliberately slow (anti-pumping)
    CHECK(b.rms(FC, settle) < 0.01);
    CHECK(b.rms(SL, settle) > 0.2);
    CHECK(b.rms(SR, settle) > 0.2);
    CHECK(u.correlation() < -0.95f);
}

TEST(Upmixer, LfeOnlyCarriesBass) {
    Upmixer u;
    u.configure(UpmixSettings{});
    Bus lo(48000), hi(48000);
    fillTone(lo, 40, 0.5f, 0.5f);
    fillTone(hi, 2000, 0.5f, 0.5f);
    u.process(lo.ptr.data(), 48000, UpmixMode::Upmix31);
    u.reset();
    u.process(hi.ptr.data(), 48000, UpmixMode::Upmix31);
    CHECK(lo.rms(LFE, 12000) > 0.08);         // ~-10 dB of 0.35 rms
    CHECK(hi.rms(LFE, 12000) < 0.001);
    CHECK(hi.rms(SL, 0) == 0.0);              // 3.1 mode: no surrounds
}

TEST(Dynamics, CompressorCurveAndLimiterCeiling) {
    NightModeCompressor c;
    c.configure(CompressorSettings{});
    CHECK_NEAR(c.staticCurveDb(-60.0f), 0.0, 1e-4);                 // below knee: unity
    CHECK_NEAR(c.staticCurveDb(-6.0f), -(24.0 - 24.0 / 3.0), 1e-3);   // 3:1 above threshold
    SafetyLimiter l;
    l.configure(-1.0f, 100.0f);
    Bus b(4800);
    fillTone(b, 500, 1.6f, 1.6f);
    l.process(b.ptr.data(), kBusChannels, 4800);
    float peak = 0;
    for (float v : b.buf[FL]) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= dbToLin(-1.0f) + 1e-5f);
}

TEST(TestSignals, PinkLevelAndChannelId) {
    TestSignalGenerator g;
    Bus b(kBlockFrames);
    double acc = 0;
    const int blocks = 60;
    for (int k = 0; k < blocks; ++k) {
        g.render(TestSignal::PinkNoise, 1u << FL, b.ptr.data(), kBlockFrames);
        acc += b.rms(FL) * b.rms(FL);
        CHECK(b.rms(FR) == 0.0);
    }
    const double db = 10 * std::log10(acc / blocks);
    CHECK_NEAR(db, -20.0, 3.0);
    g.render(TestSignal::ChannelId, 0x3F, b.ptr.data(), kBlockFrames);
    CHECK_EQ(g.currentChannel.load(), int32_t(FL));
}

// ---------------------------------------------------------------------------
TEST(Ipc, ProtocolParsing) {
    using namespace avr::ipc;
    auto r = parseRequest("set_gain 3 -4.5");
    CHECK(r.cmd == Cmd::SetGain);
    CHECK_EQ(r.args.size(), size_t(2));
    CHECK_NEAR(*parseFloat(r.args[1]), -4.5, 1e-6);
    r = parseRequest("SET_OUTPUT_DEVICE \"{0.0.0.00000000}.{abc def}\"");
    CHECK(r.cmd == Cmd::SetOutputDevice);
    CHECK_EQ(r.args[0], std::string("{0.0.0.00000000}.{abc def}"));
    CHECK(parseCodec("dts") == Codec::DTS);
    CHECK(!parseCodec("mp3").has_value());
    CHECK_EQ(*parseInt("0x3F", 16), 63LL);
    const auto s = Response::ok("x").kv("a", "1").line("END").serialise();
    CHECK_EQ(s, std::string("OK x\na=1\n END\nEND\n"));   // embedded END is escaped
}

TEST(Ipc, TelemetryLayoutPinned) {
    using avr::ipc::TelemetryBlock;
    CHECK_EQ(sizeof(TelemetryBlock), size_t(2048));
    CHECK_EQ(avr::ipc::kTelemetryVersion, 5u);
    CHECK_EQ(offsetof(TelemetryBlock, eqHeadroomDb), size_t(1596));
    CHECK_EQ(offsetof(TelemetryBlock, auditionChannel), size_t(1600));
    CHECK_EQ(offsetof(TelemetryBlock, eqActiveMask), size_t(1604));
    CHECK_EQ(offsetof(TelemetryBlock, lfeStatus), size_t(1488));
    CHECK_EQ(offsetof(TelemetryBlock, lfeStatusText), size_t(1500));
    CHECK_EQ(offsetof(TelemetryBlock, outGainDb), size_t(272));
    CHECK_EQ(offsetof(TelemetryBlock, outputDescription), size_t(368));
    CHECK_EQ(offsetof(TelemetryBlock, outputDevice), size_t(592));
    CHECK_EQ(offsetof(TelemetryBlock, latCaptureMs), size_t(1024));
    CHECK_EQ(offsetof(TelemetryBlock, procState), size_t(1080));
    CHECK_EQ(offsetof(TelemetryBlock, bufferMsSetting), size_t(1124));
    CHECK_EQ(offsetof(TelemetryBlock, processingText), size_t(1200));
    CHECK_EQ(offsetof(TelemetryBlock, gameName), size_t(1424));
}

TEST(Ipc, V12Commands) {
    using namespace avr::ipc;
    CHECK(parseRequest("SET_BASS -3.5").cmd == Cmd::SetBass);
    CHECK(parseRequest("set_sub 0").cmd == Cmd::SetSub);
    CHECK(parseRequest("SET_LFE_GEN always").cmd == Cmd::SetLfeGen);
    CHECK(parseRequest("SET_GAMING AUTO").cmd == Cmd::SetGaming);
    CHECK(parseRequest("SET_GAME_LFE 1").cmd == Cmd::SetGameLfe);
    CHECK(parseRequest("set_lfe_fold 0").cmd == Cmd::SetLfeFold);    // v1.5.0
    CHECK(parseRequest("set_lfe_reset 30").cmd == Cmd::SetLfeReset);
    const auto g = parseRequest("GAME_DETECTED 1 \"Some Game.exe\"");
    CHECK(g.cmd == Cmd::GameDetected);
    CHECK_EQ(g.args.size(), size_t(2));
    CHECK_EQ(g.args[1], std::string("Some Game.exe"));
    CHECK(parseLfeGenMode("auto") == LfeGenMode::Auto);
    CHECK(parseLfeGenMode("ALWAYS") == LfeGenMode::Always);
    CHECK(!parseLfeGenMode("sometimes").has_value());
    CHECK(parseGamingMode("on") == GamingMode::On);
    CHECK_EQ(std::string(lfeGenToken(LfeGenMode::Off)), std::string("OFF"));
    CHECK_EQ(std::string(gamingToken(GamingMode::Auto)), std::string("AUTO"));
}

// v1.4.0: custom EQ / audition commands and the 1..300 s reset validation.
TEST(Ipc, V14Commands) {
    using namespace avr::ipc;
    CHECK(parseRequest("SET_EQ 1").cmd == Cmd::SetEq);
    CHECK(parseRequest("set_eq_channel 3 0").cmd == Cmd::SetEqChannel);
    const auto b = parseRequest("SET_EQ_BAND 0 2 1000 -3.5 1.4");
    CHECK(b.cmd == Cmd::SetEqBand);
    CHECK_EQ(b.args.size(), size_t(5));
    CHECK(parseRequest("RESET_EQ_CHANNEL 4").cmd == Cmd::ResetEqChannel);
    CHECK(parseRequest("SET_AUDITION OFF").cmd == Cmd::SetAudition);
    // Resume after silence: whole seconds from 1 to 300 only.
    CHECK(parseLfeResetSeconds("1").has_value());
    CHECK_NEAR(*parseLfeResetSeconds("1"), 1.0, 1e-6);
    CHECK_NEAR(*parseLfeResetSeconds("10"), 10.0, 1e-6);
    CHECK_NEAR(*parseLfeResetSeconds("10.0"), 10.0, 1e-6);
    CHECK_NEAR(*parseLfeResetSeconds("300"), 300.0, 1e-6);
    CHECK(!parseLfeResetSeconds("0").has_value());
    CHECK(!parseLfeResetSeconds("301").has_value());
    CHECK(!parseLfeResetSeconds("10.5").has_value());
    CHECK(!parseLfeResetSeconds("ten").has_value());
    // EQ band config form
    const auto band = parseEqBand("1000.0,-3.50,1.40");
    CHECK(band.has_value());
    CHECK_NEAR(band->freqHz, 1000.0, 1e-3);
    CHECK_NEAR(band->gainDb, -3.5, 1e-3);
    CHECK_NEAR(band->q, 1.4, 1e-3);
    CHECK(!parseEqBand("1000,2").has_value());
    CHECK(!parseEqBand("a,b,c").has_value());
    const auto round = parseEqBand(eqBandToken(avr::EqBand{63.0f, 4.5f, 2.25f}));
    CHECK(round.has_value());
    CHECK_NEAR(round->freqHz, 63.0, 0.05);
    CHECK_NEAR(round->gainDb, 4.5, 0.005);
    CHECK_NEAR(round->q, 2.25, 0.005);
}

// ---------------------------------------------------------------------------
// v1.2.0 building blocks
TEST(Dsp, LowShelfBassControl) {
    const auto c = BiquadCoeffs::lowShelf(48000, 120, 6.0);
    CHECK_NEAR(20 * std::log10(c.magnitude(48000, 25)), 6.0, 0.2);
    CHECK_NEAR(20 * std::log10(c.magnitude(48000, 120)), 3.0, 0.2);
    CHECK_NEAR(20 * std::log10(c.magnitude(48000, 2000)), 0.0, 0.05);
    const auto flat = BiquadCoeffs::lowShelf(48000, 120, 0.0);
    CHECK_NEAR(flat.magnitude(48000, 50), 1.0, 1e-4);
}

// v1.5.0: the fold low-passes the LFE (120 Hz), adds it to FL/FR at -3 dB each and ramps.
TEST(Bass, FoldLfeIntoFronts) {
    BassManager bm;
    const size_t n = kBlockFrames;
    double ph = 0, ph2 = 0;
    auto block = [&](Bus& b, bool enable) {
        for (size_t i = 0; i < n; ++i) {
            // 31.25 Hz (exactly one period per block) + 2 kHz out-of-band junk
            b.buf[LFE][i] = 0.4f * float(std::sin(ph)) + 0.2f * float(std::sin(ph2));
            b.buf[FL][i] = 0.0f;
            b.buf[FR][i] = 0.0f;
            ph += 2 * kPi * 31.25 / 48000.0;
            ph2 += 2 * kPi * 2000.0 / 48000.0;
        }
        bm.foldLfeIntoFronts(b.ptr.data(), n, enable);
    };
    Bus b(n);
    for (int k = 0; k < 10; ++k) block(b, true);
    CHECK_NEAR(bm.foldGain(), BassManager::kFoldGain, 1e-6);
    // 31 Hz passes (LR4 @ 120 Hz: ~ -0.04 dB), 2 kHz is gone: FL rms ~= 0.4/sqrt2 * 0.707
    CHECK_NEAR(b.rms(FL), 0.4 / std::sqrt(2.0) * 0.7071, 0.01);
    CHECK_NEAR(b.rms(FR), b.rms(FL), 1e-6);
    CHECK_NEAR(b.rms(LFE), 0.0, 1e-9);                       // LFE plane cleared
    // Equal power: FL + FR together carry the LFE power.
    CHECK_NEAR(b.rms(FL) * b.rms(FL) + b.rms(FR) * b.rms(FR), 0.4 * 0.4 / 2.0, 0.01);
    // Off: the LFE is discarded (plane still cleared), after a one-block ramp - no click.
    block(b, false);
    double maxStep = 0;
    for (size_t i = 1; i < n; ++i) maxStep = std::max(maxStep, double(std::fabs(b.buf[FL][i] - b.buf[FL][i - 1])));
    CHECK(maxStep < 0.01);
    block(b, false);
    CHECK_NEAR(b.rms(FL), 0.0, 1e-9);
    CHECK_NEAR(b.rms(LFE), 0.0, 1e-9);
    CHECK_NEAR(bm.foldGain(), 0.0, 1e-9);
}

TEST(Bass, GeneratorIsLowPassAndRamps) {
    BassManager bm;
    bm.setGenerator(80.0f, 0.0f);
    const size_t n = kBlockFrames;
    double ph40 = 0, ph2k = 0;
    auto block = [&](Bus& b, bool enable) {
        for (size_t i = 0; i < n; ++i) {
            const float lo = 0.25f * float(std::sin(ph40));
            const float hi = 0.25f * float(std::sin(ph2k));
            ph40 += 2 * kPi * 40.0 / 48000.0;
            ph2k += 2 * kPi * 2000.0 / 48000.0;
            b.buf[FC][i] = lo + hi;
            b.buf[LFE][i] = 0.0f;
        }
        bm.generateLfe(b.ptr.data(), n, enable);
    };
    Bus b(n);
    for (int k = 0; k < 8; ++k) block(b, true);              // ~0.26 s: ramping
    CHECK(bm.generatorGain() > 0.4f && bm.generatorGain() < 0.6f);
    for (int k = 0; k < 20; ++k) block(b, true);
    CHECK_NEAR(bm.generatorGain(), 1.0, 1e-6);
    // 40 Hz passes (LR4 at 80 Hz: about -1 dB), 2 kHz is gone: LFE rms ~= 0.35 * 0.177 * 0.89
    CHECK_NEAR(b.rms(LFE), 0.35 * 0.25 / std::sqrt(2.0) * 0.89, 0.01);
    for (int k = 0; k < 12; ++k) block(b, false);            // 0.38 s > 0.3 s fade-out
    CHECK_NEAR(bm.generatorGain(), 0.0, 1e-9);
}

TEST(Analysis, FrequencyRangeOfTwoTones) {
    FrequencyRange fr;
    fr.configure();
    Bus b(kBlockFrames);
    double p1 = 0, p2 = 0;
    for (int blk = 0; blk < 120; ++blk) {   // ~3.8 s
        for (size_t i = 0; i < kBlockFrames; ++i) {
            const float v = 0.2f * float(std::sin(p1)) + 0.2f * float(std::sin(p2));
            p1 += 2 * kPi * 200.0 / 48000.0;
            p2 += 2 * kPi * 4000.0 / 48000.0;
            b.buf[FL][i] = v;
            b.buf[FR][i] = -v;               // anti-phase: must not cancel
        }
        fr.analyse(b.ptr.data(), kBlockFrames);
    }
    CHECK(fr.lowHz() > 140.0f && fr.lowHz() < 200.0f);
    CHECK(fr.highHz() > 4000.0f && fr.highHz() < 4070.0f);
    fr.reset();
    CHECK_EQ(fr.lowHz(), 0.0f);
}

TEST(Policy, PlanOutput) {
    OutputRequest r;
    r.codec = Codec::DTS;
    r.mode = OutputMode::Exclusive;
    r.upmix = UpmixMode::PcmDirect;
    r.devicePcmChannels = 8;
    auto p = planOutput(r);
    CHECK(p.codec == Codec::DTS && p.iec == IecType::DTS1 && !p.stereoPcm);
    CHECK_EQ(p.encoderChannels, uint16_t(6));
    r.stereoPcmActive = true;
    p = planOutput(r);
    CHECK(p.stereoPcm && p.codec == Codec::PCM && p.iec == IecType::None);
    CHECK_EQ(p.pcmChannels, uint16_t(2));
    r.upmix = UpmixMode::Upmix51;            // mode changed while PCM was active
    CHECK(!planOutput(r).stereoPcm);
    r.upmix = UpmixMode::PcmDirect;
    r.passthroughIec = IecType::AC3;         // pre-encoded input wins
    p = planOutput(r);
    CHECK(!p.stereoPcm && p.codec == Codec::AC3 && p.iec == IecType::AC3);
    r.passthroughIec = IecType::None;
    r.dedicatedSub = false;
    r.stereoPcmActive = false;
    CHECK_EQ(planOutput(r).encoderChannels, uint16_t(5));
    r.codec = Codec::PCM;                    // PCM format: lossless stereo is meaningless
    r.stereoPcmActive = true;
    CHECK(!planOutput(r).stereoPcm);
}

TEST(Policy, StereoPcmSwitchDebounce) {
    StereoPcmSwitch sw;
    using A = StereoPcmSwitch::Action;
    CHECK(sw.update(1, false, 1'000'000) == A::None);     // first sight
    CHECK(sw.update(1, false, 1'250'000) == A::None);     // 250 ms < 300 ms
    CHECK(sw.update(1, false, 1'500'000) == A::SwitchToPcm);
    CHECK(sw.update(1, true, 1'750'000) == A::None);      // already PCM: nothing to do
    CHECK(sw.update(2, true, 2'000'000) == A::None);
    CHECK(sw.update(0, true, 2'250'000) == A::None);      // request withdrawn -> timer reset
    CHECK(sw.update(2, true, 2'500'000) == A::None);
    CHECK(sw.update(2, true, 2'750'000) == A::None);
    CHECK(sw.update(2, true, 2'800'001) == A::SwitchToBitstream);
}

TEST(Policy, EffectiveStatusPriority) {
    CHECK(effectiveStatus(false, true, true, IecType::AC3, true) == OutputStatus::StoppedByUser);
    CHECK(effectiveStatus(true, true, false, IecType::None, false) == OutputStatus::Error);
    CHECK(effectiveStatus(true, false, true, IecType::None, true) == OutputStatus::FallbackShared);
    CHECK(effectiveStatus(true, false, false, IecType::DTS1, false) == OutputStatus::BitstreamPassthrough);
    CHECK(effectiveStatus(true, false, false, IecType::None, true) == OutputStatus::StereoPcmDirect);
    CHECK(effectiveStatus(true, false, false, IecType::None, false) == OutputStatus::Match);
}

TEST(Config, LfeSettingsPersist) {
    namespace fs = std::filesystem;
    using avr::win::EngineConfig;
    const auto file = fs::temp_directory_path() / "legacyavr-test-engine.cfg";
    EngineConfig def;
    CHECK(def.gameLfeGen);                                   // v1.5.0 new-install defaults
    CHECK(def.gaming == GamingMode::Off);
    CHECK(def.lfeFold);
    CHECK_NEAR(def.lfeResetSeconds, 10.0, 1e-6);
    EngineConfig a;
    a.gameLfeGen = true;
    a.lfeResetSeconds = 25.0f;
    a.lfeGen = LfeGenMode::Off;
    a.gaming = GamingMode::On;
    a.dedicatedSub = false;
    a.bassDb = -3.5f;
    CHECK(avr::win::saveConfig(file, a));
    EngineConfig b;
    CHECK(avr::win::loadConfig(file, b));
    CHECK(b.gameLfeGen);
    CHECK_NEAR(b.lfeResetSeconds, 25.0, 1e-4);
    CHECK(b.lfeGen == LfeGenMode::Off);
    CHECK(b.gaming == GamingMode::On);
    CHECK(!b.dedicatedSub);
    CHECK_NEAR(b.bassDb, -3.5, 1e-4);
    CHECK(b.lfeFold);
    a.lfeFold = false;
    CHECK(avr::win::saveConfig(file, a));
    EngineConfig b2;
    CHECK(avr::win::loadConfig(file, b2));
    CHECK(!b2.lfeFold);
    a.lfeFold = true;
    // v1.4.0: 1..300 whole seconds. 3 s is now valid; out-of-range values are corrected on load.
    for (const auto& [saved, loaded] : {std::pair{3.0f, 3.0f}, std::pair{1.0f, 1.0f}, std::pair{300.0f, 300.0f},
                                         std::pair{0.0f, 1.0f}, std::pair{600.0f, 300.0f}, std::pair{12.4f, 12.0f}}) {
        a.lfeResetSeconds = saved;
        CHECK(avr::win::saveConfig(file, a));
        EngineConfig c;
        CHECK(avr::win::loadConfig(file, c));
        CHECK_NEAR(c.lfeResetSeconds, loaded, 1e-4);
    }
    std::error_code ec;
    fs::remove(file, ec);
}

// v1.5.0: new defaults (Game Mode off, Game Mode LFE on, LFE fold on) are for new installs only:
// a file written by an older version keeps behaving as it did.
TEST(Config, OlderFilesKeepTheirDefaults) {
    namespace fs = std::filesystem;
    using avr::win::EngineConfig;
    const auto file = fs::temp_directory_path() / "legacyavr-test-legacy.cfg";
    {
        std::ofstream out(file, std::ios::trunc);
        out << "codec=AC3\nupmix=UPMIX51\ndedicated_sub=0\n";   // v1.1-era file: no gaming / game_lfe_gen / lfe_fold
    }
    EngineConfig c;
    CHECK(avr::win::loadConfig(file, c));
    CHECK(c.gaming == avr::win::kLegacyGamingDefault);
    CHECK(c.gameLfeGen == avr::win::kLegacyGameLfeGenDefault);
    CHECK(c.lfeFold);                                        // every older version folded
    {
        std::ofstream out(file, std::ios::trunc);
        out << "gaming=OFF\ngame_lfe_gen=1\nlfe_fold=0\n";
    }
    EngineConfig d;
    CHECK(avr::win::loadConfig(file, d));
    CHECK(d.gaming == GamingMode::Off);
    CHECK(d.gameLfeGen);
    CHECK(!d.lfeFold);
    std::error_code ec;
    fs::remove(file, ec);
}

// v1.5.0: the dashboard text never claims a layout that is not really sent.
TEST(StatusText, LayoutsAreHonest) {
    const OutputShape sub51{true, 6, true, true, false};
    const OutputShape noSub50{true, 5, false, true, true};
    const OutputShape noSub30{true, 3, false, true, true};
    const OutputShape dtsNoSub{true, 5, false, true, true};
    const auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
    // Stereo -> 3.x
    CHECK(has(processingText(ProcessingState::StereoUpmix31, kMaskStereo, sub51, false), "3.1 upmix"));
    CHECK(has(processingText(ProcessingState::StereoUpmix31, kMaskStereo, sub51, false), "in a 5.1 stream"));
    const auto t30 = processingText(ProcessingState::StereoUpmix31, kMaskStereo, noSub30, false);
    CHECK(has(t30, "3.0 upmix") && !has(t30, "3.1") && !has(t30, "stream"));
    CHECK(has(processingText(ProcessingState::StereoUpmix31, kMaskStereo, dtsNoSub, false), "3.0 upmix (in a 5.0 stream"));
    CHECK(has(processingText(ProcessingState::StereoUpmix51, kMaskStereo, noSub50, false), "5.0 upmix"));
    // Discrete 5.1 without a subwoofer: 5.0 out, LFE folded / discarded
    const auto d51 = processingText(ProcessingState::Discrete, kMask51Side, noSub50, false);
    CHECK(has(d51, "5.1") && has(d51, "5.0") && has(d51, "front L/R"));
    OutputShape drop = noSub50;
    drop.lfeFold = false;
    CHECK(has(processingText(ProcessingState::Discrete, kMask51Side, drop, false), "discarded"));
    // 5.0 source without a subwoofer: nothing to fold, nothing generated
    const uint32_t m50 = kMask51Side & ~SPK_LFE;
    CHECK(processingText(ProcessingState::Discrete, m50, noSub50, false) == "No upmix (discrete 5.0)");
    // 7.1 is folded to 5.x - never claimed as 7.x
    const auto f71 = processingText(ProcessingState::FoldDown71, kMask71, noSub50, false);
    CHECK(has(f71, "5.0 fold-down") && has(f71, "front L/R"));
    CHECK(has(processingText(ProcessingState::FoldDown71, kMask71, sub51, false), "5.1 fold-down"));
    // PCM 2.0 says it is a downmix
    const OutputShape pcm20{false, 2, false, true, false};
    CHECK(has(processingText(ProcessingState::Discrete, kMask51Side, pcm20, false), "downmixed to PCM 2.0"));
    // Layout names
    CHECK(encodedLayoutName(3, false) == "3.0");
    CHECK(encodedLayoutName(4, true) == "3.1");
    CHECK(encodedLayoutName(5, false) == "5.0");
    CHECK(has(encodedLayoutName(6, false), "LFE silent"));
    CHECK(has(pcmLayoutName(8, true), "back pair silent"));
    // LFE status wording
    CHECK(has(lfeStatusText(LfeStatus::Source, noSub50, false, 0, 10, false), "folded into front L/R"));
    CHECK(has(lfeStatusText(LfeStatus::Dropped, drop, false, 0, 10, false), "discarded"));
    CHECK(!has(lfeStatusText(LfeStatus::None, noSub50, false, 0, 10, false), "Generat"));
    CHECK(lfeStatusText(LfeStatus::Source, sub51, false, 0, 10, false) == "From the source");
}

TEST(Config, EqSettingsPersist) {
    namespace fs = std::filesystem;
    using avr::win::EngineConfig;
    const auto file = fs::temp_directory_path() / "legacyavr-test-eq.cfg";
    EngineConfig def;
    CHECK(!def.eq.enabled);                                  // defaults: off, flat, every channel enabled
    CHECK(def.eq.channelOn[avr::LFE]);
    CHECK_NEAR(def.eq.bands[avr::FL][2].gainDb, 0.0, 1e-9);
    EngineConfig a;
    a.eq.enabled = true;
    a.eq.channelOn[avr::SR] = false;
    a.eq.bands[avr::FL][0] = avr::EqBand{90.0f, 4.5f, 1.0f};
    a.eq.bands[avr::LFE][3] = avr::EqBand{120.0f, -6.0f, 2.5f};
    CHECK(avr::win::saveConfig(file, a));
    EngineConfig b;
    CHECK(avr::win::loadConfig(file, b));
    CHECK(b.eq.enabled);
    CHECK(!b.eq.channelOn[avr::SR]);
    CHECK(b.eq.channelOn[avr::FL]);
    CHECK_NEAR(b.eq.bands[avr::FL][0].freqHz, 90.0, 0.05);
    CHECK_NEAR(b.eq.bands[avr::FL][0].gainDb, 4.5, 0.01);
    CHECK_NEAR(b.eq.bands[avr::LFE][3].q, 2.5, 0.01);
    CHECK_NEAR(b.eq.bands[avr::FR][2].gainDb, 0.0, 1e-9);
    // A hand-edited file with out-of-range values is corrected on load.
    {
        std::ofstream out(file, std::ios::app);
        out << "eq3_b4=900,12,9\n";                          // LFE high shelf: 900 Hz, +12 dB, Q 9
    }
    EngineConfig c;
    CHECK(avr::win::loadConfig(file, c));
    CHECK_NEAR(c.eq.bands[avr::LFE][4].freqHz, 200.0, 1e-3);
    CHECK_NEAR(c.eq.bands[avr::LFE][4].gainDb, 6.0, 1e-3);
    CHECK_NEAR(c.eq.bands[avr::LFE][4].q, 4.0, 1e-3);
    std::error_code ec;
    fs::remove(file, ec);
}
