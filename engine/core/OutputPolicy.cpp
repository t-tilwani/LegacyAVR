#include "OutputPolicy.h"

namespace avr {

OutputPlan planOutput(const OutputRequest& r) noexcept {
    OutputPlan p;
    p.codec = r.passthroughIec != IecType::None ? codecForIec(r.passthroughIec) : r.codec;
    p.pcmChannels = r.devicePcmChannels >= 8 ? 8 : r.devicePcmChannels >= 6 ? 6 : 2;

    // Lossless stereo only makes sense while the user asked for a bitstream format in
    // exclusive mode, the mode is PcmDirect and no pre-encoded input is being forwarded.
    p.stereoPcm = r.stereoPcmActive && r.upmix == UpmixMode::PcmDirect && r.passthroughIec == IecType::None &&
                  r.mode == OutputMode::Exclusive && r.codec != Codec::PCM;
    if (p.stereoPcm) {
        p.codec = Codec::PCM;
        p.pcmChannels = 2;
    }
    p.iec = (r.mode == OutputMode::Exclusive && p.codec != Codec::PCM) ? iecForCodec(p.codec) : IecType::None;
    p.encoderChannels = (r.dedicatedSub || r.inputHasLfe) ? 6 : 5;   // same rule as CoreConfig::mainEncoderChannels
    return p;
}

StereoPcmSwitch::Action StereoPcmSwitch::update(uint32_t coreRequest, bool pcmActive, uint64_t nowUs) noexcept {
    const bool wantPcm = coreRequest == 1 && !pcmActive;
    const bool wantBitstream = coreRequest == 2 && pcmActive;
    if (!wantPcm && !wantBitstream) {
        reset();
        return Action::None;
    }
    if (sinceUs_ == 0 || pending_ != coreRequest) {
        sinceUs_ = nowUs ? nowUs : 1;
        pending_ = coreRequest;
        return Action::None;
    }
    if (nowUs - sinceUs_ < kConfirmUs) return Action::None;
    reset();
    return wantPcm ? Action::SwitchToPcm : Action::SwitchToBitstream;
}

OutputStatus effectiveStatus(bool audioEnabled, bool startFailed, bool fellBack, IecType passthroughIec,
                             bool stereoPcm) noexcept {
    if (!audioEnabled) return OutputStatus::StoppedByUser;
    if (startFailed) return OutputStatus::Error;
    if (fellBack) return OutputStatus::FallbackShared;
    if (passthroughIec != IecType::None) return OutputStatus::BitstreamPassthrough;
    if (stereoPcm) return OutputStatus::StereoPcmDirect;
    return OutputStatus::Match;
}

} // namespace avr
