// LegacyAVR real-time performance benchmark:  LegacyAVR.Tests.exe --bench
//
// Pushes 60 s of synthetic audio per scenario through the complete ProcessingCore
// (input metering, IEC detection, matrix, resampler, upmix, gains, limiter, encoder,
// IEC 61937 packing) as fast as possible and reports the cost relative to real time.
// "CPU %" is the share of ONE core needed to keep up in real time; Task Manager shows
// that number divided by your logical core count.

#include "../engine/core/ProcessingCore.h"
#include "PipelineSim.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace avr;

namespace {

struct Scenario {
    const char* name;
    uint32_t rate;
    uint16_t channels;
    uint32_t mask;
    Codec codec;
    uint32_t bitrate;
    UpmixMode upmix;
    double targetPercent;   // spec target (one core)
};

double runScenario(const Scenario& s, double& percent) {
    LiveParams params;
    params.upmixMode = static_cast<uint32_t>(s.upmix);
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    in.allocate(size_t(s.channels) * s.rate);
    out.allocate(16 * 48000);
    CoreConfig cfg;
    cfg.inRate = s.rate;
    cfg.inChannels = s.channels;
    cfg.inMask = s.mask;
    cfg.codec = s.codec;
    cfg.bitrate = s.bitrate;
    cfg.outputMode = s.codec == Codec::PCM ? OutputMode::Shared : OutputMode::Exclusive;
    cfg.pcmChannels = 2;
    ProcessingCore core;
    std::string err;
    if (!core.configure(cfg, params, in, out, err)) {
        std::printf("  %-34s configure failed: %s\n", s.name, err.c_str());
        return -1;
    }

    const double seconds = 60.0;
    const size_t chunk = s.rate / 100;   // 10 ms
    std::vector<float> buf(chunk * s.channels);
    std::vector<uint8_t> drain(out.capacity());
    double ph = 0;
    uint64_t now = 0;
    double busy = 0;
    const size_t steps = size_t(seconds * 100);
    for (size_t k = 0; k < steps; ++k) {
        for (size_t i = 0; i < chunk; ++i) {
            const float v = 0.25f * float(std::sin(ph));
            ph += 2 * std::numbers::pi * 440.0 / s.rate;
            for (size_t c = 0; c < s.channels; ++c)
                buf[i * s.channels + c] = (c & 1) ? -v * 0.7f : v;   // decorrelated-ish content
        }
        in.write(buf.data(), buf.size());
        now += 10'000;
        const auto t0 = std::chrono::steady_clock::now();
        core.step(now);
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        out.read(drain.data(), 480 * core.outputBytesPerBusFrame());   // 10 ms "renderer"
    }
    percent = busy / seconds * 100.0;
    return busy / double(core.stats.blocks.load()) * 1e6;   // us per 1536-frame block
}

} // namespace

int runBenchmarks() {
    const Scenario scenarios[] = {
        {"Stereo 48k -> 5.1 upmix -> AC-3 640k", 48000, 2, kMaskStereo, Codec::AC3, 640000, UpmixMode::Upmix51, 1.0},
        {"Stereo 48k -> passthrough -> AC-3 2.0", 48000, 2, kMaskStereo, Codec::AC3, 640000, UpmixMode::Passthrough, 1.0},
        {"5.1 48k -> AC-3 640k", 48000, 6, kMask51Side, Codec::AC3, 640000, UpmixMode::Upmix51, 3.0},
        {"7.1 48k -> fold-down -> AC-3 640k", 48000, 8, kMask71, Codec::AC3, 640000, UpmixMode::Upmix51, 5.0},
        {"7.1 96k -> resample -> AC-3 640k", 96000, 8, kMask71, Codec::AC3, 640000, UpmixMode::Upmix51, 5.0},
        {"7.1 48k -> DTS 1509.75k", 48000, 8, kMask71, Codec::DTS, 1509750, UpmixMode::Upmix51, 5.0},
        {"7.1 48k -> E-AC-3 1024k", 48000, 8, kMask71, Codec::EAC3, 1024000, UpmixMode::Upmix51, 5.0},
        {"7.1 48k -> PCM stereo (shared)", 48000, 8, kMask71, Codec::PCM, 0, UpmixMode::Upmix51, 5.0},
    };
    std::printf("LegacyAVR DSP/encode benchmark (60 s of audio per scenario)\n\n");
    std::printf("  %-38s %12s %12s %8s  %s\n", "scenario", "us/block", "CPU % (1c)", "target", "result");
    int fails = 0;
    for (const auto& s : scenarios) {
        double pct = 0;
        const double us = runScenario(s, pct);
        if (us < 0) { ++fails; continue; }
        const bool ok = pct <= s.targetPercent;
        if (!ok) ++fails;
        std::printf("  %-38s %12.1f %12.3f %7.1f%%  %s\n", s.name, us, pct, s.targetPercent, ok ? "PASS" : "OVER");
    }
    std::printf("\nBlock budget is 32000 us (1536 frames @ 48 kHz). Build Release|x64 for meaningful numbers.\n");
    return fails;
}

// ---------------------------------------------------------------------------
// LegacyAVR.Tests.exe --latency   (v1.2.0)
//
// Repeatable latency measurement: impulses through the real ProcessingCore with
// simulated 10 ms capture packets and the render period each margin setting uses
// (see EngineHost::startPipeline: period = max(device minimum, margin / 2), 5 ms for
// Automatic). "core" is capture-packet arrival -> sample played; the dashboard adds
// the device stages (Windows mixer + loopback, WASAPI buffer, driver) from WASAPI's
// own figures. "v1.1 queue" is what the same setting cost before v1.2.0.
int runLatencyReport() {
    struct Row { const char* label; uint32_t bufferMs; uint32_t period; };
    const Row rows[] = {
        {"Automatic (lowest safe)", 0, 240},
        {"15 ms", 15, 360},
        {"20 ms", 20, 480},
        {"30 ms", 30, 720},
        {"50 ms", 50, 1200},
    };
    std::printf("LegacyAVR pipeline latency (20 s per setting, 10 ms capture packets)\n\n");
    std::printf("  %-24s %8s %8s %8s %10s %10s %10s %8s\n", "margin setting", "period", "margin", "floor",
                "core (ms)", "min..max", "v1.1 queue", "glitches");
    int fails = 0;
    for (const auto& r : rows) {
        const auto m = avrsim::measurePipelineLatency(r.bufferMs, r.period, 20.0);
        if (!m.ok) {
            std::printf("  %-24s failed: %s\n", r.label, m.error.c_str());
            ++fails;
            continue;
        }
        const double oldQueue = 32.0 + r.period / 48.0 + (r.bufferMs ? r.bufferMs : 20);
        char range[32];
        std::snprintf(range, sizeof(range), "%.0f..%.0f", m.measuredMinMs, m.measuredMaxMs);
        std::printf("  %-24s %6.1f ms %6.1f ms %6.1f ms %10.1f %10s %8.1f ms %8u\n", r.label, r.period / 48.0,
                    m.marginMs, m.floorMs, m.measuredMs, range, oldQueue, m.silenceInserts + m.underruns);
        if (m.silenceInserts + m.underruns) ++fails;
    }
    std::printf("\nTypical end-to-end = core + ~10-15 ms Windows mixer/loopback + 2x render period + driver.\n"
                "The AVR's own decoding (often 20-60 ms) comes on top of everything shown in LegacyAVR.\n");
    return fails;
}
