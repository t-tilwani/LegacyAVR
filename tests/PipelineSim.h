// LegacyAVR - repeatable latency measurement through the real ProcessingCore (v1.2.0).
//
// Simulates the Windows side with sample-accurate clocks:
//   capture : one 10 ms packet (480 frames, 7.1, 48 kHz) every 10 ms
//   render  : one period of `renderPeriodFrames` read every period
// and sends a one-sample impulse on FL every 250 ms. For every impulse it records the
// time from the arrival of its capture packet to the moment the renderer plays it,
// i.e. exactly the part of the chain that LegacyAVR controls (resampler, block
// assembly, output queue). Device latencies (Windows mixer, WASAPI buffers, HDMI,
// the AVR's decoder) are added by the dashboard from WASAPI's own figures.
//
// Used by Pipeline.LatencyMatchesReportedStages (test_codec.cpp) and by
// "LegacyAVR.Tests.exe --latency" (bench.cpp).
#pragma once

#include "../engine/core/ProcessingCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace avrsim {

struct LatencyResult {
    bool ok = false;
    std::string error;
    int impulses = 0;
    double measuredMs = 0;       // capture-packet arrival -> played (mean)
    double measuredMinMs = 0;
    double measuredMaxMs = 0;
    double reportedMs = 0;       // what the telemetry stages say for the same span
    double targetMs = 0;         // drift target (block + margin)
    double marginMs = 0;
    double floorMs = 0;
    uint32_t silenceInserts = 0;
    uint32_t underruns = 0;
};

inline LatencyResult measurePipelineLatency(uint32_t bufferMs, uint32_t renderPeriodFrames, double seconds = 20.0,
                                            double renderPpm = 0.0) {
    using namespace avr;
    LatencyResult r;
    constexpr uint32_t kPacket = 480;
    constexpr uint32_t kTick = 24;   // 0.5 ms simulation step (period must be a multiple)
    if (renderPeriodFrames == 0 || renderPeriodFrames % kTick != 0) {
        r.error = "render period must be a multiple of 24 frames";
        return r;
    }

    LiveParams params;
    params.upmixMode = static_cast<uint32_t>(UpmixMode::Passthrough);
    params.stereoAsTwoChannel = true;
    params.lfeGenMode = static_cast<uint32_t>(LfeGenMode::Off);
    SpscRing<float> in;
    SpscRing<uint8_t> out;
    in.allocate(8 * 48000);
    out.allocate(8 * 48000);
    CoreConfig cfg;
    cfg.inRate = 48000;
    cfg.inChannels = 8;
    cfg.inMask = kMask71;
    cfg.capturePacketFrames = kPacket;
    cfg.codec = Codec::PCM;              // PCM 2.0 out: the impulse can be found sample-exactly
    cfg.outputMode = OutputMode::Exclusive;
    cfg.pcmChannels = 2;
    cfg.renderPeriodFrames = renderPeriodFrames;
    cfg.bufferMs = bufferMs;
    ProcessingCore core;
    if (!core.configure(cfg, params, in, out, r.error)) return r;

    std::vector<float> packet(size_t(kPacket) * 8, 0.0f);
    std::vector<float> chunk(size_t(renderPeriodFrames) * 2, 0.0f);
    std::deque<uint64_t> pending;            // arrival time (frames) of impulses in flight
    std::vector<double> lat;
    const uint64_t total = uint64_t(seconds * 48000.0);
    const uint64_t settle = 5 * 48000;
    const uint64_t impulseEvery = 12000;     // 250 ms
    uint64_t inFrame = 0;                    // capture frames produced so far
    uint64_t refractoryUntil = 0;
    double renderAcc = 0.0;

    for (uint64_t t = kTick; t <= total; t += kTick) {
        const uint64_t nowUs = t * 1'000'000 / 48000;
        if (t % kPacket == 0) {
            std::fill(packet.begin(), packet.end(), 0.0f);
            for (uint32_t i = 0; i < kPacket; ++i, ++inFrame) {
                if (inFrame % impulseEvery == impulseEvery / 2) {
                    packet[size_t(i) * 8] = 0.9f;    // FL
                    pending.push_back(t);
                }
            }
            in.write(packet.data(), packet.size());
            core.step(nowUs);
        }
        if (t % renderPeriodFrames == 0) {
            renderAcc += double(renderPeriodFrames) * (1.0 + renderPpm * 1e-6);
            const size_t want = size_t(renderAcc);
            renderAcc -= double(want);
            const size_t avail = out.readAvailable() / 8;
            const size_t got = std::min(avail, std::min(want, size_t(renderPeriodFrames) * 2));
            if (got < want && t > 48000) ++r.underruns;
            if (chunk.size() < got * 2) chunk.resize(got * 2);
            out.read(reinterpret_cast<uint8_t*>(chunk.data()), got * 8);
            for (size_t j = 0; j < got; ++j) {
                const uint64_t playAt = t + j;
                if (playAt < refractoryUntil || pending.empty()) continue;
                if (std::fabs(chunk[2 * j]) > 0.3f) {
                    const uint64_t arrived = pending.front();
                    pending.pop_front();
                    if (arrived > settle && playAt >= arrived)
                        lat.push_back(double(playAt - arrived) * 1000.0 / 48000.0);
                    refractoryUntil = playAt + 2400;   // ignore resampler ringing (50 ms)
                }
            }
            core.step(nowUs);   // the render callback wakes the DSP thread too
        }
    }

    r.impulses = int(lat.size());
    if (lat.empty()) { r.error = "no impulse came through"; return r; }
    double sum = 0;
    r.measuredMinMs = 1e9;
    for (double v : lat) { sum += v; r.measuredMinMs = std::min(r.measuredMinMs, v); r.measuredMaxMs = std::max(r.measuredMaxMs, v); }
    r.measuredMs = sum / double(lat.size());
    // Same formula as EngineHost::publishTelemetry: resampler + assembly (16 ms) + queue.
    const double post = double(core.stats.postPushFill.load());
    r.reportedMs = core.resamplerDelayMs() + 0.5 * kBlockFrames / 48.0 + std::max(0.0, post - 0.5 * kBlockFrames) / 48.0;
    r.targetMs = core.sizing().targetFrames / 48.0;
    r.marginMs = core.sizing().marginFrames / 48.0;
    r.floorMs = core.sizing().floorFrames / 48.0;
    r.silenceInserts = core.stats.silenceInserts.load();
    r.ok = true;
    return r;
}

} // namespace avrsim
