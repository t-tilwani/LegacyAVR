// LegacyAVR - core shared types and constants (no OS headers; Windows-only project,
// but the DSP core is kept free of <windows.h> so it is unit-testable in isolation).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace avr {

// ---------------------------------------------------------------------------
// Fixed pipeline geometry
// ---------------------------------------------------------------------------
inline constexpr uint32_t kBusRate        = 48000;  // all DSP + encoding runs at 48 kHz
inline constexpr size_t   kBlockFrames    = 1536;   // one AC-3 / E-AC-3 frame (6 x 256 samples)
inline constexpr size_t   kMaxInChannels  = 8;      // 7.1 input maximum
inline constexpr size_t   kBusChannels    = 6;      // internal bus is always 5.1 (FL FR FC LFE SL SR)
inline constexpr size_t   kMaxInputChunk  = 2048;   // max frames pulled from the capture ring per step

// Internal 5.1 bus order == FFmpeg AV_CH_LAYOUT_5POINT1 native order (side surrounds).
enum BusChannel : uint8_t { FL = 0, FR = 1, FC = 2, LFE = 3, SL = 4, SR = 5 };

inline constexpr std::array<std::string_view, kBusChannels> kBusChannelNames{
    "FL", "FR", "C", "LFE", "SL", "SR"};

// WAVEFORMATEXTENSIBLE dwChannelMask bits (duplicated so the core stays OS-free).
enum SpeakerBit : uint32_t {
    SPK_FL = 0x1,   SPK_FR = 0x2,   SPK_FC = 0x4,   SPK_LFE = 0x8,
    SPK_BL = 0x10,  SPK_BR = 0x20,  SPK_FLC = 0x40, SPK_FRC = 0x80,
    SPK_BC = 0x100, SPK_SL = 0x200, SPK_SR = 0x400,
};
inline constexpr uint32_t kMaskStereo = SPK_FL | SPK_FR;
inline constexpr uint32_t kMask51Back = SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_BL | SPK_BR;
inline constexpr uint32_t kMask51Side = SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_SL | SPK_SR;
inline constexpr uint32_t kMask71     = kMask51Back | SPK_SL | SPK_SR;

// ---------------------------------------------------------------------------
// Modes (numeric values are part of the IPC contract - never renumber)
// ---------------------------------------------------------------------------
enum class Codec : uint32_t { AC3 = 0, DTS = 1, EAC3 = 2, PCM = 3 };
enum class OutputMode : uint32_t { Exclusive = 0, Shared = 1 };
// Stereo-source handling.
//  Passthrough : keep the chosen format; stereo is encoded as 2.0 (or 5.1 with only FL/FR)
//  Upmix31/51  : dynamic upmix, encoded in the chosen format
//  PcmDirect   : stereo is sent as lossless PCM 2.0; the output switches back to the
//                chosen bitstream format automatically when multichannel content appears
enum class UpmixMode : uint32_t { Passthrough = 0, Upmix31 = 1, Upmix51 = 2, PcmDirect = 3 };

// Why the opened output differs from the requested one (telemetry, IPC contract).
enum class OutputStatus : uint32_t {
    Match = 0,               // opened exactly what was requested
    FallbackShared = 1,      // exclusive/bitstream refused -> shared PCM
    BitstreamPassthrough = 2,// pre-encoded input forwarded; renderer opened for its codec
    StereoPcmDirect = 3,     // stereo content -> lossless PCM 2.0 (PcmDirect mode)
    StoppedByUser = 4,       // audio pipeline stopped from the UI
    Error = 5,               // could not open input/output (see reason)
};

// v1.2.0: 5.0 -> 5.1 LFE generation (separate from the stereo upmixer).
//  Off    : never generate an LFE channel outside the stereo upmixer
//  Auto   : generate one only for multichannel sources whose own LFE is silent (5.0, 7.0,
//           3.0 ...), confirmed for the content hold time, with smooth fades
//  Always : like Auto, and also for stereo sources that are not upmixed (2.0 -> 2.1)
// v1.5.0: only with a dedicated subwoofer. Without one no LFE is ever generated; the
// "LFE fold" switch (LiveParams::lfeFold) decides what happens to the source's own LFE.
enum class LfeGenMode : uint32_t { Off = 0, Auto = 1, Always = 2 };

// v1.2.0: game handling. The engine only sees "gaming active"; Auto detection (a
// full-screen game in the foreground) runs in the dashboard and is pushed over IPC.
enum class GamingMode : uint32_t { Auto = 0, On = 1, Off = 2 };

// v1.2.0: what the DSP actually did with the last block (live indicator, telemetry).
enum class ProcessingState : uint32_t {
    Idle = 0,             // no input (keep-alive silence)
    StereoUpmix51 = 1,    // stereo -> 5.1 upmix
    StereoUpmix31 = 2,    // stereo -> 3.1 upmix
    StereoKept = 3,       // stereo kept as 2.0 (encoded 2.0 or 5.1 with fronts only)
    StereoPcm = 4,        // stereo sent as lossless PCM 2.0
    StereoLfeGen = 5,     // stereo kept + generated LFE (2.0 -> 2.1)
    Discrete = 6,         // multichannel source, no processing besides routing
    LfeGen = 7,           // multichannel without LFE -> generated LFE (5.0 -> 5.1)
    FoldDown71 = 8,       // 7.1 source folded to the 5.1 bus
    FoldDown71LfeGen = 9, // 7.0 source folded + generated LFE
    Bitstream = 10,       // pre-encoded input forwarded untouched
    TestSignal = 11,      // diagnostics test signal
};

// v1.3.0: where the LFE output comes from (LFE status indicator).
enum class LfeStatus : uint32_t {
    None = 0,        // no LFE content (source has none, generation off or not needed)
    Source = 1,      // the source's own LFE channel
    Generated = 2,   // LFE generation (5.0 -> 5.1, 2.0 -> 2.1, game mode)
    Upmixed = 3,     // created by the stereo upmixer
    Inhibited = 4,   // generation would run, but the source used its own LFE this session
    TestSignal = 5,
    Dropped = 6,     // v1.5.0: no subwoofer and "LFE fold" off - the source's LFE is discarded
};

enum class StreamKind : uint32_t {
    None = 0, Pcm = 1, BitstreamAC3 = 2, BitstreamDTS = 3, BitstreamEAC3 = 4,
};

enum class EngineState : uint32_t {
    Stopped = 0, Starting = 1, Running = 2, Reconfiguring = 3, Error = 4, Idle = 5,
};

enum class TestSignal : uint32_t {
    Off = 0,
    PinkNoise = 1,     // per-speaker mask
    ChannelId = 2,     // speech (SAPI) or beep-count identification, cycles the channels
    CenterBleed = 3,   // alternates phantom centre (L+R) with discrete C
    LfeSweep = 4,      // 20 -> 200 Hz log sweep on LFE only
    PhaseCheck = 5,    // speaker pairs alternate in-phase / inverted
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline constexpr float kMinDb = -96.0f;

inline float dbToLin(float db) noexcept { return db <= -144.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline float linToDb(float lin) noexcept {
    return lin <= 1.5849e-5f ? kMinDb : 20.0f * std::log10(lin);   // floored at -96 dB
}

constexpr const char* codecName(Codec c) noexcept {
    switch (c) {
        case Codec::AC3:  return "Dolby Digital";
        case Codec::DTS:  return "DTS";
        case Codec::EAC3: return "Dolby Digital Plus";
        case Codec::PCM:  return "PCM";
    }
    return "?";
}

constexpr const char* lfeGenName(LfeGenMode m) noexcept {
    switch (m) {
        case LfeGenMode::Off: return "off";
        case LfeGenMode::Auto: return "auto";
        case LfeGenMode::Always: return "always";
    }
    return "?";
}

constexpr uint32_t defaultBitrate(Codec c) noexcept {
    switch (c) {
        case Codec::AC3:  return 640000;
        case Codec::DTS:  return 1509750;
        case Codec::EAC3: return 1024000;
        case Codec::PCM:  return 0;
    }
    return 0;
}

constexpr int popcount32(uint32_t v) noexcept {
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

} // namespace avr
