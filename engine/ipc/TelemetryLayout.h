// LegacyAVR - shared-memory telemetry block (engine -> UI), 2048 bytes, packed.
//
// Mapping name : "Local\\LegacyAVR.Telemetry.v1"   (AVREngine.exe, per-user)
//                "Global\\LegacyAVR.Telemetry.v1"  (AVREngineService.exe, session 0)
// Writer       : engine telemetry thread at ~30 Hz.
// Concurrency  : seqlock. Writer sets `sequence` odd, writes, sets it even (release).
//                Reader copies the block and retries if sequence was odd or changed.
//
// !!! This layout is mirrored in ui/LegacyAVR.UI/Interop/TelemetryBlock.cs.
// !!! Offsets are pinned by static_asserts here and by xUnit tests on the C# side.
#pragma once

#include <cstddef>
#include <cstdint>

namespace avr::ipc {

inline constexpr uint32_t kTelemetryMagic = 0x5256414C;   // "LAVR"
inline constexpr uint32_t kTelemetryVersion = 5;   // v5: LegacyAVR 1.4.0 (custom EQ, audition)
inline constexpr wchar_t kTelemetryNameLocal[] = L"Local\\LegacyAVR.Telemetry.v1";
inline constexpr wchar_t kTelemetryNameGlobal[] = L"Global\\LegacyAVR.Telemetry.v1";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\LegacyAVR.Control";

enum TelemetryFlags : uint32_t {
    TF_NightMode        = 1u << 0,
    TF_MuteAll          = 1u << 1,
    TF_Passthrough      = 1u << 2,
    TF_InputActive      = 1u << 3,
    TF_TestActive       = 1u << 4,
    TF_BitstreamBlocked = 1u << 5,
    TF_ServiceMode      = 1u << 6,
    TF_SpeechAvailable  = 1u << 7,
    TF_StereoAs20       = 1u << 8,
    TF_AudioStopped     = 1u << 9,   // pipeline stopped by the user (process still running)
    TF_StereoPcmActive  = 1u << 10,  // PcmDirect mode currently outputting PCM 2.0
};

// v3: second flag word.
enum TelemetryFlags2 : uint32_t {
    TF2_DedicatedSub    = 1u << 0,   // "Dedicated subwoofer" active
    TF2_GamingActive    = 1u << 1,   // game handling currently active (mode On, or Auto + game detected)
    TF2_LfeGenActive    = 1u << 2,   // LFE generator audible (ramp > 50 %)
    TF2_BufferAuto      = 1u << 3,   // safety margin = automatic (lowest safe)
    TF2_Encoder50       = 1u << 4,   // bitstream encoded as 5.0 (no LFE channel)
    TF2_GameLfeGen      = 1u << 5,   // v4: Game Mode's "LFE generation" toggle is on
    TF2_LfeGameOverride = 1u << 6,   // v4: ...and is currently overriding the general setting
    TF2_LfeInhibited    = 1u << 7,   // v4: native-LFE latch set (Auto generation paused)
    TF2_EqEnabled       = 1u << 8,   // v5: Custom EQ master switch is on (the bass control is replaced)
};

#pragma pack(push, 1)
struct TelemetryBlock {
    uint32_t magic;                 //   0
    uint32_t version;               //   4
    uint32_t sequence;              //   8  seqlock (odd = write in progress)
    uint32_t engineState;           //  12  avr::EngineState
    uint64_t updateCounter;         //  16
    uint64_t qpcTimestamp;          //  24
    // ---- input -------------------------------------------------------------------
    uint32_t inSampleRate;          //  32
    uint16_t inChannels;            //  36  endpoint channels
    uint16_t inBitsPerSample;       //  38
    uint32_t inChannelMask;         //  40  endpoint mask
    uint32_t inStreamKind;          //  44  avr::StreamKind
    uint32_t inContentMask;         //  48  speakers carrying signal (live layout)
    // ---- output ------------------------------------------------------------------
    uint32_t outCodec;              //  52  avr::Codec (effective)
    uint32_t outBitrate;            //  56
    uint32_t outSampleRate;         //  60  transport rate (192000 for E-AC-3)
    uint16_t outChannels;           //  64  encoded channels (2/6) or PCM channels
    uint16_t outMode;               //  66  avr::OutputMode
    uint32_t upmixMode;             //  68  avr::UpmixMode
    uint32_t flags;                 //  72  TelemetryFlags
    float masterDb;                 //  76
    float cpuPercent;               //  80  whole-process CPU (all cores = 100)
    float dspLoadPercent;           //  84  DSP thread time / block budget
    float latencyMs;                //  88  estimated end-to-end (excl. AVR decode)
    float captureBufferMs;          //  92
    float outputBufferMs;           //  96
    float driftPpm;                 // 100
    float correlation;              // 104
    float dialogActivity;           // 108
    float nightGainReductionDb;     // 112
    float limiterReductionDb;       // 116
    uint64_t framesEncoded;         // 120  blocks * 1536
    uint32_t underruns;             // 128  renderer
    uint32_t overruns;              // 132  output ring / latency guard
    uint32_t inputDrops;            // 136
    uint32_t testSignal;            // 140  avr::TestSignal
    int32_t testChannel;            // 144
    float testFrequencyHz;          // 148
    uint32_t testStep;              // 152
    uint32_t configRevision;        // 156  bumps on any config change
    float inPeakDb[8];              // 160
    float inRmsDb[8];               // 192
    float outPeakDb[6];             // 224
    float outRmsDb[6];              // 248
    float outGainDb[6];             // 272
    uint8_t outMuted[6];            // 296
    uint16_t reserved0;             // 302
    char inputDescription[64];      // 304  e.g. "PCM 5.1 @ 48 kHz"
    char outputDescription[96];     // 368  e.g. "Dolby Digital 5.1 @ 640 kbps 48 kHz"
    char inputDevice[128];          // 464
    char outputDevice[128];         // 592
    char lastError[160];            // 720
    // ---- v2: requested vs. effective output ------------------------------------------
    uint32_t requestedCodec;        // 880  avr::Codec the user selected
    uint32_t requestedBitrate;      // 884
    uint32_t requestedMode;         // 888  avr::OutputMode the user selected
    uint32_t outputStatus;          // 892  avr::OutputStatus (why effective != requested)
    char effectiveReason[128];      // 896  short human-readable reason
    // ---- v3 (LegacyAVR 1.2.0) ---------------------------------------------------------
    // Per-stage latency in ms. Sum = latencyMs. See docs/ARCHITECTURE.md "Latency".
    float latCaptureMs;             // 1024 Windows mixer -> loopback packet (stream latency + 1/2 packet)
    float latCaptureQueueMs;        // 1028 captured, not yet processed
    float latResamplerMs;           // 1032 libswresample filter delay
    float latAssemblyMs;            // 1036 average wait for the 1536-sample block (16 ms)
    float latOutputQueueMs;         // 1040 average output-ring fill
    float latEncoderMs;             // 1044 encoder look-ahead (AC-3 / E-AC-3: 256 samples)
    float latDeviceBufferMs;        // 1048 WASAPI buffer (one period in exclusive mode)
    float latDeviceStreamMs;        // 1052 WASAPI stream latency (driver / hardware FIFO)
    float latMarginMs;              // 1056 output safety margin in use
    float latFloorMs;               // 1060 lowest safe margin for this device pair
    float latRenderPeriodMs;        // 1064
    float latCapturePacketMs;       // 1068
    // Input analysis
    float freqLowHz;                // 1072 0 = not enough signal
    float freqHighHz;               // 1076
    // Processing (live, from the DSP - not inferred from settings)
    uint32_t procState;             // 1080 avr::ProcessingState
    uint32_t flags2;                // 1084 TelemetryFlags2
    float lfeGenGain;               // 1088 0..1
    float bassDb;                   // 1092
    uint32_t lfeGenMode;            // 1096 avr::LfeGenMode
    uint32_t gamingMode;            // 1100 avr::GamingMode
    uint32_t pcmSwitchStatus;       // 1104 avr::PcmSwitchStatus
    uint32_t pcmBlockMask;          // 1108 speakers blocking the lossless-stereo switch
    float pcmHoldSeconds;           // 1112
    uint32_t emitChannels;          // 1116 channels leaving the engine (0 = bit-exact passthrough)
    uint32_t emitMask;              // 1120 their speaker mask (ascending bit order = channel order)
    uint32_t bufferMsSetting;       // 1124 user setting, 0 = automatic
    float emitPeakDb[8];            // 1128
    float emitRmsDb[8];             // 1160
    float lfeFoldPeakDb;            // 1192 no subwoofer: LFE folded into the fronts
    float lfeFoldRmsDb;             // 1196
    char processingText[96];        // 1200 e.g. "Stereo -> 5.1 upmix"
    char pcmSwitchText[128];        // 1296 lossless-stereo switch status / reason
    char gameName[64];              // 1424 game reported by the dashboard
    // ---- v4 (LegacyAVR 1.3.0) ---------------------------------------------------------
    uint32_t lfeStatus;             // 1488 avr::LfeStatus
    float lfeResetSeconds;          // 1492 silence needed to release the native-LFE latch
    float lfeSilenceSeconds;        // 1496 silence so far while the latch is set
    char lfeStatusText[96];         // 1500 e.g. "From the source -> folded into FL/FR"
    // ---- v5 (LegacyAVR 1.4.0) ---------------------------------------------------------
    float eqHeadroomDb;             // 1596 level taken off every channel so EQ boosts cannot clip (<= 0)
    int32_t auditionChannel;        // 1600 EQ audition: soloed bus channel, -1 = off
    uint32_t eqActiveMask;          // 1604 bus channels the custom EQ is filtering right now
    uint8_t reserved5[440];         // 1608 -> 2048
};
#pragma pack(pop)

static_assert(sizeof(TelemetryBlock) == 2048, "TelemetryBlock must be 2048 bytes");
static_assert(offsetof(TelemetryBlock, inSampleRate) == 32);
static_assert(offsetof(TelemetryBlock, outCodec) == 52);
static_assert(offsetof(TelemetryBlock, framesEncoded) == 120);
static_assert(offsetof(TelemetryBlock, inPeakDb) == 160);
static_assert(offsetof(TelemetryBlock, outMuted) == 296);
static_assert(offsetof(TelemetryBlock, inputDescription) == 304);
static_assert(offsetof(TelemetryBlock, lastError) == 720);
static_assert(offsetof(TelemetryBlock, requestedCodec) == 880);
static_assert(offsetof(TelemetryBlock, effectiveReason) == 896);
static_assert(offsetof(TelemetryBlock, latCaptureMs) == 1024);
static_assert(offsetof(TelemetryBlock, freqLowHz) == 1072);
static_assert(offsetof(TelemetryBlock, procState) == 1080);
static_assert(offsetof(TelemetryBlock, pcmSwitchStatus) == 1104);
static_assert(offsetof(TelemetryBlock, emitChannels) == 1116);
static_assert(offsetof(TelemetryBlock, emitPeakDb) == 1128);
static_assert(offsetof(TelemetryBlock, emitRmsDb) == 1160);
static_assert(offsetof(TelemetryBlock, lfeFoldPeakDb) == 1192);
static_assert(offsetof(TelemetryBlock, processingText) == 1200);
static_assert(offsetof(TelemetryBlock, pcmSwitchText) == 1296);
static_assert(offsetof(TelemetryBlock, gameName) == 1424);
static_assert(offsetof(TelemetryBlock, lfeStatus) == 1488);
static_assert(offsetof(TelemetryBlock, lfeStatusText) == 1500);
static_assert(offsetof(TelemetryBlock, eqHeadroomDb) == 1596);
static_assert(offsetof(TelemetryBlock, auditionChannel) == 1600);
static_assert(offsetof(TelemetryBlock, eqActiveMask) == 1604);
static_assert(offsetof(TelemetryBlock, reserved5) == 1608);

} // namespace avr::ipc
