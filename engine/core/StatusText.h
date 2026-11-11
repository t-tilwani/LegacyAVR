// LegacyAVR - human-readable live-processing / LFE status / output layout text (v1.5.0).
//
// Portable (no OS calls) so the unit tests can check the wording: the dashboard must never
// claim a layout that is not really sent (e.g. "3.1" when the AVR receives 5.1 with silent
// surrounds, or "7.1" when the bus is 5.1).
#pragma once

#include "Types.h"

#include <cstdint>
#include <string>

namespace avr {

inline constexpr const char* kArrow = "\xE2\x86\x92";   // U+2192 (UTF-8)

// What the running output really carries (v1.6.0: per block).
struct OutputShape {
    bool bitstream = true;   // AC-3 / E-AC-3 / DTS (false: PCM)
    int channels = 6;        // channels actually encoded (bitstream) or the PCM device channels
    bool lfeOut = true;      // the block left with an LFE channel (5.1 / 3.1 / PCM 5.1 / 7.1)
    bool lfeFolded = false;  // the source LFE was folded into FL/FR (required or experimental)
    bool noSub = false;      // "Dedicated subwoofer" off
    bool genEnabled = false; // experimental LFE generation gate on
    bool foldOptional = false; // the fold is the user's experimental choice (the output could carry the LFE)
};

// "5.0" from a speaker mask ("PCM 5.0" without the prefix).
[[nodiscard]] std::string layoutShort(uint32_t mask);
// Bitstream layout by encoded channel count: 2 -> 2.0, 3 -> 3.0, 4 -> 3.1, 5 -> 5.0, 6 -> 5.1.
// lfeSilent: the LFE slot carries nothing because the LFE is folded ("5.0 (5.1 frame, LFE silent)").
[[nodiscard]] std::string encodedLayoutName(int channels, bool lfeSilent);
// PCM device layout; the bus is 5.1, so a 7.1 device carries 5.1 with a silent back pair.
[[nodiscard]] std::string pcmLayoutName(int channels, bool lfeSilent);

[[nodiscard]] std::string processingText(ProcessingState ps, uint32_t contentMask, const OutputShape& o, bool gaming);
[[nodiscard]] std::string lfeStatusText(LfeStatus st, const OutputShape& o, bool latch, float silenceSec, float resetSec,
                                        bool gameOverride);

} // namespace avr
