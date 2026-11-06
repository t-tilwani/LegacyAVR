// LegacyAVR - synthesises the channel-identification voice prompts ("Front Left" ...)
// with the built-in Windows SAPI 5 voice, rendered offline to 48 kHz mono float.
// No audio device is touched; works in a service (session 0) too.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace avr::win {

// Returns one clip per bus channel (FL FR C LFE SL SR). Empty vectors on failure
// (the generator then falls back to beep codes).
std::array<std::vector<float>, 6> synthesiseChannelNames(std::string& error);

} // namespace avr::win
