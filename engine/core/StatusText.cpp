#include "StatusText.h"

#include "ChannelMatrix.h"

#include <format>

namespace avr {

std::string layoutShort(uint32_t mask) {
    std::string n = ChannelMatrix::layoutName(mask);
    if (n.rfind("PCM ", 0) == 0) n.erase(0, 4);
    return n;
}

std::string encodedLayoutName(int channels, bool lfeSilent) {
    switch (channels) {
        case 2: return "2.0";
        case 3: return "3.0";
        case 4: return lfeSilent ? "3.0 (3.1 frame, LFE silent)" : "3.1";
        case 5: return "5.0";
        case 6: return lfeSilent ? "5.0 (5.1 frame, LFE silent)" : "5.1";
        default: return std::to_string(channels) + " ch";
    }
}

std::string pcmLayoutName(int channels, bool lfeSilent) {
    switch (channels) {
        case 2: return "2.0";
        case 6: return lfeSilent ? "5.0 (5.1 format, LFE silent)" : "5.1";
        case 8: return lfeSilent ? "5.0 (7.1 format, LFE + back pair silent)" : "5.1 (7.1 format, back pair silent)";
        default: return std::to_string(channels) + " ch";
    }
}

// Live indicator text - built from what the DSP actually did with the last block.
// v1.6.0: a source LFE is passed through whenever the block has an LFE channel (also without
// a subwoofer - the AVR manages the bass); it is folded into front L/R when the block has no
// LFE channel or the experimental fold is on. The internal bus is 5.1, so 7.x sources are
// always folded to 5.x (no encoder here carries 7 discrete channels; DD+ 7.1 is not available).
std::string processingText(ProcessingState ps, uint32_t contentMask, const OutputShape& o, bool gaming) {
    std::string t;
    const std::string lay = layoutShort(contentMask);
    const bool srcLfe = (contentMask & SPK_LFE) != 0;
    const bool lfeCarried = o.lfeOut && !o.lfeFolded;            // an LFE channel with content in it
    const char* upBed = o.noSub ? "5.0" : "5.1";                  // the upmix creates no LFE without a sub
    const char* streamBed = (o.channels == 6 && lfeCarried) ? "5.1" : "5.0";
    bool multichannel = false;
    bool upmixed = false;
    switch (ps) {
        case ProcessingState::Idle: return "No input";
        case ProcessingState::Bitstream: return "Bitstream passthrough (no processing)";
        case ProcessingState::TestSignal: return "Test signal";
        case ProcessingState::StereoUpmix51: t = std::format("Stereo {} {} upmix", kArrow, upBed); upmixed = true; break;
        case ProcessingState::StereoUpmix31:
            t = std::format("Stereo {} {} upmix", kArrow, o.noSub ? "3.0" : "3.1");
            if (o.bitstream && o.channels >= 5) t += std::format(" (in a {} stream, surrounds silent)", streamBed);
            upmixed = true;
            break;
        case ProcessingState::StereoKept: t = gaming ? "No upmix (stereo, game mode)" : "No upmix (stereo 2.0)"; break;
        case ProcessingState::StereoPcm: return std::format("Stereo {} lossless PCM 2.0", kArrow);
        case ProcessingState::StereoLfeGen: t = std::format("2.0 {} 2.1 LFE generation (experimental)", kArrow); break;
        case ProcessingState::Discrete:
            t = (srcLfe && o.lfeFolded) ? std::format("{} {} {}", lay, kArrow, layoutShort(contentMask & ~SPK_LFE))
                                        : std::format("No upmix (discrete {})", lay);
            multichannel = true;
            break;
        case ProcessingState::LfeGen:
            t = std::format("{} {} {}.1 LFE generation (experimental)", lay, kArrow, lay.substr(0, lay.find('.')));
            multichannel = true;
            break;
        case ProcessingState::FoldDown71:
            t = std::format("{} {} {} fold-down", lay, kArrow, (srcLfe && lfeCarried) ? "5.1" : "5.0");
            multichannel = true;
            break;
        case ProcessingState::FoldDown71LfeGen:
            t = std::format("{} {} 5.1 fold-down + LFE generation (experimental)", lay, kArrow);
            multichannel = true;
            break;
    }
    if (multichannel && srcLfe) {
        if (o.lfeFolded) t += o.foldOptional ? std::format(" | LFE {} front L/R (experimental fold)", kArrow)
                                             : std::format(" | LFE {} front L/R (no LFE channel in this output)", kArrow);
        else if (o.noSub) t += " | LFE passed through";
    }
    if (!o.bitstream && o.channels == 2 && (multichannel || upmixed)) t += " | downmixed to PCM 2.0";
    return t;
}

// LFE status indicator (v1.3.0; v1.6.0: source / generated / passed through / folded / none).
std::string lfeStatusText(LfeStatus st, const OutputShape& o, bool latch, float silenceSec, float resetSec, bool gameOverride) {
    std::string t;
    switch (st) {
        case LfeStatus::None:
            t = !o.lfeOut ? "None - this output has no LFE channel"
              : o.noSub   ? "None (this source has no LFE; nothing is generated without a subwoofer)"
              : o.genEnabled ? "None (this source has no LFE)"
                             : "None (this source has no LFE; LFE generation is experimental and off)";
            break;
        case LfeStatus::Source:
            t = o.noSub ? std::format("From the source {} passed through (no subwoofer: the AVR manages the bass)", kArrow)
                        : "From the source";
            break;
        case LfeStatus::Folded:
            t = o.foldOptional ? std::format("Source LFE {} folded into front L/R (experimental fold, 120 Hz low-pass, -3 dB)", kArrow)
                               : std::format("Source LFE {} folded into front L/R (this output has no LFE channel)", kArrow);
            break;
        case LfeStatus::Dropped: t = "Source LFE discarded"; break;   // not produced since v1.6.0
        case LfeStatus::Generated:
            t = gameOverride ? "Generated (Game Mode, experimental)" : "Generated (LFE generation, experimental)";
            break;
        case LfeStatus::Upmixed: t = "Created by the stereo upmixer"; break;
        case LfeStatus::Inhibited:
            t = std::format("Generation paused - source used its own LFE; resumes after {:.0f} s of silence ({:.0f} s so far)",
                            resetSec, silenceSec);
            break;
        case LfeStatus::TestSignal: t = "Test signal"; break;
    }
    if (latch && st == LfeStatus::Source && o.genEnabled && !o.noSub) t += " (Auto generation off until silence)";
    return t;
}

} // namespace avr
