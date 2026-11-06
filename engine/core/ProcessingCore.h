// LegacyAVR - the DSP / encode stage (runs on the "AVR DSP" MMCSS thread).
//
//   capture ring (float, N ch, capture rate)
//      -> input meters + content-layout detector + IEC 61937 detector
//      -> [bitstream input]  forward bursts untouched to the output ring
//      -> [PCM input]        N->5.1 matrix -> resample to 48 kHz (+drift trim)
//                            -> 1536-frame staging -> frequency-range analysis
//                            -> processBlock():
//                               upmix (stereo content) | test signal
//                               -> LFE generation (5.0 -> 5.1) -> bass shelf
//                               -> custom per-channel EQ (v1.4.0, replaces the shelf)
//                               -> per-channel gain/mute/audition -> no-sub LFE fold
//                               -> night mode -> limiter -> output meters
//                               -> AC-3/DTS/E-AC-3 encode -> IEC 61937
//                               (or PCM interleave/downmix) -> output ring (bytes)
//
// Output-queue sizing (v1.2.0, see docs/ARCHITECTURE.md "Latency"):
//   target = 1536 + max(margin, floor)        (post-push ring fill, 48 kHz frames)
//   floor  = capture packet + render period / 2 + 2 ms
// A block can only complete when the capture packet holding its last sample arrives,
// so the queue must bridge one capture packet plus half a render period; anything the
// user adds on top is extra safety. The old formula (1536 + period + margin) counted
// the period twice and made a "15 ms" setting cost ~22 ms.
//
// No OS calls, no locks, no allocation after configure().
#pragma once

#include "BassManager.h"
#include "ChannelEq.h"
#include "ChannelMatrix.h"
#include "DriftController.h"
#include "Dynamics.h"
#include "Encoder.h"
#include "FrequencyRange.h"
#include "Iec61937.h"
#include "LiveParams.h"
#include "Meter.h"
#include "Resampler.h"
#include "SpscRing.h"
#include "TestSignals.h"
#include "Types.h"
#include "Upmixer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace avr {

struct CoreConfig {
    // Capture endpoint (the capture thread converts every sample format to float).
    uint32_t inRate = 48000;
    uint16_t inChannels = 8;
    uint32_t inMask = 0;
    uint32_t capturePacketFrames = 480;  // capture packet (engine period) in 48 kHz frames
    // Output
    Codec codec = Codec::AC3;
    uint32_t bitrate = 640000;
    OutputMode outputMode = OutputMode::Exclusive;
    uint16_t pcmChannels = 2;            // PCM path: 2, 6 or 8 (device-dependent)
    uint32_t renderPeriodFrames = 480;   // renderer period expressed in 48 kHz bus frames
    uint32_t bufferMs = 0;               // extra safety margin; 0 = automatic (lowest safe)
    bool stereoPcmOverride = false;      // opened as PCM 2.0 because UpmixMode::PcmDirect saw stereo
    bool dedicatedSub = true;            // false: LFE folded into FL/FR, bitstream encoded as 5.0

    [[nodiscard]] bool bitstream() const noexcept {
        return outputMode == OutputMode::Exclusive && codec != Codec::PCM;
    }
    [[nodiscard]] Codec effectiveCodec() const noexcept { return bitstream() ? codec : Codec::PCM; }
};

// Output-queue sizing (frames at 48 kHz). Public so the host, the UI text and the tests
// all use the same numbers.
struct QueueSizing {
    uint32_t floorFrames = 0;    // lowest safe cushion beyond the block
    uint32_t marginFrames = 0;   // cushion actually used = max(user margin, floor)
    uint32_t targetFrames = 0;   // post-push ring fill the drift loop holds
};
[[nodiscard]] QueueSizing queueSizing(uint32_t bufferMs, uint32_t renderPeriodFrames,
                                      uint32_t capturePacketFrames) noexcept;

// Why the lossless-stereo switch is (not) happening - shown in the dashboard.
enum class PcmSwitchStatus : uint32_t {
    NotApplicable = 0,      // another stereo mode
    WaitingForStereo = 1,   // stereo detected, hold time running
    Requested = 2,          // asked the host to re-open as PCM 2.0
    Active = 3,             // running as PCM 2.0
    BlockedSurround = 4,    // non-front channels carry signal (see pcmBlockMask)
    BlockedTest = 5,        // a diagnostics test signal is playing
    BlockedPassthrough = 6, // pre-encoded input is being forwarded
    BlockedOutput = 7,      // output is not exclusive bitstream (shared / PCM format)
    BlockedGaming = 8,      // game mode keeps the bitstream format
    NoSignal = 9,           // nothing playing
    ReturningToBitstream = 10,
};

struct CoreStats {
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint32_t> outputOverruns{0};   // block dropped: output ring full / latency guard
    std::atomic<uint32_t> inputDrops{0};       // capture backlog trimmed
    std::atomic<uint32_t> encoderErrors{0};
    std::atomic<uint32_t> silenceInserts{0};   // underrun recovery: silent block injected
    std::atomic<uint32_t> streamKind{static_cast<uint32_t>(StreamKind::None)};
    std::atomic<uint32_t> contentMask{0};      // speakers with signal in the last 1.5 s
    std::atomic<bool> inputActive{false};
    std::atomic<bool> passthrough{false};
    std::atomic<bool> bitstreamBlocked{false}; // bitstream input but output is PCM/shared
    std::atomic<uint16_t> requestedIec{0};     // IecType the renderer should be opened with
    // PcmDirect mode: 0 = keep current output, 1 = switch to PCM 2.0, 2 = switch back to bitstream
    std::atomic<uint32_t> outputRequest{0};
    std::atomic<uint32_t> pcmSwitchStatus{0};  // PcmSwitchStatus
    std::atomic<uint32_t> pcmBlockMask{0};     // speakers that block the PCM switch
    std::atomic<float> pcmHoldSeconds{0.0f};   // stable-stereo time so far
    std::atomic<uint32_t> encoderChannels{6};
    std::atomic<uint32_t> procState{0};        // ProcessingState of the last real block
    std::atomic<float> lfeGenGain{0.0f};       // 0..1 ramp of the LFE generator
    std::atomic<uint32_t> lfeStatus{0};        // LfeStatus of the last real block (v1.3.0)
    std::atomic<bool> lfeGameOverride{false};  // Game Mode's LFE toggle is overriding the general setting
    std::atomic<float> eqHeadroomDb{0.0f};     // v1.4.0: level taken off so EQ boosts cannot clip (<= 0)
    std::atomic<uint32_t> eqActiveMask{0};     // v1.4.0: bus channels the EQ is filtering
    std::atomic<uint32_t> emitChannels{0};     // channels actually leaving (0 = bit-exact passthrough)
    std::atomic<uint32_t> emitMask{0};         // their speaker mask (labels)
    std::atomic<float> postPushFill{0.0f};     // smoothed post-push output fill (frames)
    std::atomic<float> driftPpm{0.0f};
    std::atomic<float> correlation{0.0f};
    std::atomic<float> dialog{0.0f};
    std::atomic<float> nightGrDb{0.0f};
    std::atomic<float> limiterGrDb{0.0f};
    std::atomic<float> dspLoad{0.0f};          // fraction of the 32 ms block budget used
};

class ProcessingCore {
public:
    ProcessingCore() = default;
    ProcessingCore(const ProcessingCore&) = delete;
    ProcessingCore& operator=(const ProcessingCore&) = delete;

    // Not real-time: allocates every buffer, opens encoders and the resampler.
    bool configure(const CoreConfig& cfg, LiveParams& params,
                   SpscRing<float>& input, SpscRing<uint8_t>& output, std::string& error);

    // Real-time: consume everything available in the capture ring. nowUs is a
    // monotonic clock (QPC-derived). Returns true if any block was produced.
    bool step(uint64_t nowUs) noexcept;

    [[nodiscard]] const CoreConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] size_t outputBytesPerBusFrame() const noexcept { return outBytesPerFrame_; }
    [[nodiscard]] double outputFillFrames() const noexcept;
    [[nodiscard]] double captureFillFrames() const noexcept;
    [[nodiscard]] IecType encoderIecType() const noexcept { return encIec_; }
    [[nodiscard]] int encoderDelaySamples() const noexcept { return encMain_.delaySamples(); }
    [[nodiscard]] int encoderMainChannels() const noexcept { return encMain_.channels(); }
    [[nodiscard]] double resamplerDelayMs() const noexcept { return resampler_.delayMs(); }
    [[nodiscard]] double driftTargetFrames() const noexcept { return drift_.target(); }
    [[nodiscard]] const QueueSizing& sizing() const noexcept { return sizing_; }
    [[nodiscard]] size_t stagedFrames() const noexcept { return stagingFrames_; }

    TestSignalGenerator& tests() noexcept { return tests_; }
    [[nodiscard]] const ChannelEqBank& eq() const noexcept { return eq_; }
    MeterBank<kMaxInChannels> inputMeters;
    MeterBank<kBusChannels> outputMeters;   // bus (FL FR C LFE SL SR) after the limiter
    MeterBank<8> emitMeters;                // what actually leaves, in output channel order
    MeterBank<1> lfeFoldMeter;              // no-subwoofer: LFE content folded into the fronts
    FrequencyRange frequencyRange;
    CoreStats stats;

private:
    void processBlock(float* const* planes, bool stereoContent, bool realInput) noexcept;
    void pushSilenceBlock() noexcept;
    void applyGains(float* const* planes) noexcept;
    void emitPcm(float* const* planes, bool twoChannelContent) noexcept;
    void forwardBitstream(size_t frames) noexcept;
    void updateContent(size_t frames, uint64_t nowUs) noexcept;
    void refreshUpmixSettings() noexcept;
    void updateOutputRequest(uint64_t nowUs) noexcept;
    void updateLfeInhibit(uint64_t nowUs) noexcept;
    static void onPacket(void* user, const uint8_t* data, size_t bytes);

    CoreConfig cfg_{};
    LiveParams* params_ = nullptr;
    SpscRing<float>* in_ = nullptr;
    SpscRing<uint8_t>* out_ = nullptr;

    ChannelMatrix matrix_;
    Upmixer upmixer_;
    BassManager bass_;
    ChannelEqBank eq_;
    NightModeCompressor night_;
    SafetyLimiter limiter_;
    Resampler resampler_;
    DriftController drift_;
    AudioEncoder encMain_, enc2_;   // encMain_: 5.1 (or 5.0 without a subwoofer)
    Iec61937Detector detector_;
    TestSignalGenerator tests_;
    IecType encIec_ = IecType::None;
    QueueSizing sizing_{};

    // scratch (allocated in configure)
    std::vector<float> inScratch_;
    std::vector<int16_t> words_;
    std::array<std::vector<float>, kBusChannels> matrixBuf_, stagingBuf_;
    std::array<float*, kBusChannels> matrixPtr_{}, stagingPtr_{}, silencePtr_{};
    std::vector<float> silence_;
    std::array<float*, kBusChannels> stageCursor_{};
    std::vector<uint8_t> burst_;
    std::vector<float> pcmOut_;
    size_t stagingCap_ = 0;
    size_t stagingFrames_ = 0;
    size_t outBytesPerFrame_ = 4;

    // state
    std::array<float, kBusChannels> gainLin_{};
    std::array<uint64_t, kMaxInChannels> lastActiveUs_{};
    uint64_t lastInputUs_ = 0;
    bool contentStereo_ = true;
    bool detectEligible_ = false;
    bool lastNight_ = false;
    uint32_t upmixRev_ = ~0u;
    uint32_t eqRev_ = ~0u;
    bool lastTestActive_ = false;
    double blockBudgetUs_ = 32000.0;
    bool blockDropped_ = false;
    bool primed_ = false;
    uint64_t startUs_ = 0;        // first step() after configure
    uint64_t lastMultiUs_ = 0;    // last time multichannel content was seen
    uint64_t lastSignalUs_ = 0;   // last time any channel carried signal
    uint64_t multiSinceUs_ = 0;   // start of the current multichannel stretch
    uint64_t nowUs_ = 0;          // clock of the current step()
};

} // namespace avr
