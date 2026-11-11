// LegacyAVR - output policy shared by the Windows host and the unit tests (v1.2.0).
//
// Before v1.2.0 the "Lossless PCM 2.0 for stereo" decision lived inside EngineHost
// (Windows only), so the tests could only check that ProcessingCore *asked* for a
// switch, never that the output was actually re-opened as PCM 2.0 and reported as
// such. Everything the host decides is now here, OS-free:
//
//   planOutput()       what to open (codec, PCM channels, IEC type, encoder layout)
//   StereoPcmSwitch    debounces the core's request and says when to re-open
//   effectiveStatus()  what the dashboard's "Effective output" badge shows
//
// EngineHost calls exactly these functions, and test_codec.cpp drives the same
// sequence end-to-end (core request -> policy -> re-open -> effective output).
#pragma once

#include "Iec61937.h"
#include "Types.h"

#include <cstdint>

namespace avr {

[[nodiscard]] constexpr IecType iecForCodec(Codec c) noexcept {
    switch (c) {
        case Codec::AC3: return IecType::AC3;
        case Codec::DTS: return IecType::DTS1;
        case Codec::EAC3: return IecType::EAC3;
        default: return IecType::None;
    }
}
[[nodiscard]] inline Codec codecForIec(IecType t) noexcept {
    if (t == IecType::EAC3) return Codec::EAC3;
    if (iecIsDts(t)) return Codec::DTS;
    return Codec::AC3;
}
// All DTS burst types travel over the same DTS sub-format.
[[nodiscard]] inline IecType renderFamily(IecType t) noexcept { return iecIsDts(t) ? IecType::DTS1 : t; }

struct OutputRequest {
    Codec codec = Codec::AC3;               // user's format
    OutputMode mode = OutputMode::Exclusive;
    UpmixMode upmix = UpmixMode::Upmix51;
    IecType passthroughIec = IecType::None; // pre-encoded input currently forwarded
    bool stereoPcmActive = false;           // StereoPcmSwitch state
    bool dedicatedSub = true;
    bool inputHasLfe = true;                // v1.6.0: the capture format has an LFE channel
    uint16_t devicePcmChannels = 2;         // channels of the device mix format (2/6/8)
};

struct OutputPlan {
    Codec codec = Codec::AC3;       // codec to open (PCM for the lossless stereo path)
    uint16_t pcmChannels = 2;       // PCM path channel count
    bool stereoPcm = false;         // opened as PCM 2.0 because of PcmDirect
    IecType iec = IecType::None;    // renderer IEC sub-format (None => PCM)
    uint16_t encoderChannels = 6;   // main encoder: 6 = 5.1, 5 = 5.0 (v1.6.0: only without a
                                    // subwoofer AND without an input LFE - a source LFE is passed through)
};

[[nodiscard]] OutputPlan planOutput(const OutputRequest& r) noexcept;

// Debounces ProcessingCore's outputRequest (0 keep, 1 -> PCM 2.0, 2 -> bitstream).
// The supervisor calls update() every tick (250 ms); a request must persist for
// kConfirmUs before a re-open is ordered, so a single odd block cannot cause churn.
class StereoPcmSwitch {
public:
    enum class Action { None, SwitchToPcm, SwitchToBitstream };
    static constexpr uint64_t kConfirmUs = 300'000;

    Action update(uint32_t coreRequest, bool pcmActive, uint64_t nowUs) noexcept;
    void reset() noexcept { sinceUs_ = 0; pending_ = 0; }
    [[nodiscard]] uint64_t pendingSinceUs() const noexcept { return sinceUs_; }

private:
    uint64_t sinceUs_ = 0;
    uint32_t pending_ = 0;
};

[[nodiscard]] OutputStatus effectiveStatus(bool audioEnabled, bool startFailed, bool fellBack,
                                           IecType passthroughIec, bool stereoPcm) noexcept;

} // namespace avr
