// LegacyAVR - persistent engine configuration (simple key=value file).
//   per-user engine : %LOCALAPPDATA%\LegacyAVR\engine.cfg
//   service         : %ProgramData%\LegacyAVR\engine.cfg
#pragma once

#include "../core/ChannelEq.h"
#include "../core/Types.h"

#include <array>
#include <filesystem>
#include <string>

namespace avr::win {

struct EngineConfig {
    std::wstring inputDeviceId;    // empty = default render endpoint (loopback)
    std::wstring outputDeviceId;   // empty = default render endpoint
    Codec codec = Codec::AC3;
    uint32_t bitrate = 640000;
    OutputMode outputMode = OutputMode::Exclusive;
    uint32_t bufferMs = 0;          // output safety margin, 0 = automatic (lowest safe) - v1.2.0
    UpmixMode upmix = UpmixMode::Upmix51;
    bool stereoAs20 = true;
    bool nightMode = false;
    float masterDb = 0.0f;
    std::array<float, kBusChannels> gainDb{};
    std::array<bool, kBusChannels> mute{};
    float centreLevel = 0.85f;
    float centreFloor = 0.30f;
    float dialogBoost = 0.6f;
    float surroundLevel = 0.8f;
    float surroundDelayMs = 12.0f;
    float lfeCrossoverHz = 80.0f;
    float lfeLevelDb = -10.0f;
    // v1.2.0
    float bassDb = 0.0f;
    bool dedicatedSub = true;       // "Dedicated subwoofer": false = no subwoofer
    LfeGenMode lfeGen = LfeGenMode::Auto;
    GamingMode gaming = GamingMode::Auto;
    // v1.3.0
    bool gameLfeGen = false;        // Game Mode's own LFE generation toggle
    float lfeResetSeconds = 10.0f;  // native-LFE latch: silence needed before Auto generation may resume
                                    // (v1.4.0: whole seconds 1..300, default 10)
    // v1.4.0
    EqConfig eq = eqDefaultConfig();   // custom per-channel EQ (master off, every band flat)
};

std::filesystem::path defaultDataDir(bool serviceMode);
bool loadConfig(const std::filesystem::path& file, EngineConfig& cfg);
bool saveConfig(const std::filesystem::path& file, const EngineConfig& cfg);

} // namespace avr::win
