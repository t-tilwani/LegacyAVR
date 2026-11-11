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
    GamingMode gaming = GamingMode::Off;   // v1.5.0 default for new installs (was Auto); saved files keep theirs
    // v1.3.0
    bool gameLfeGen = true;         // Game Mode's own LFE generation toggle (v1.5.0 default on; was off)
    float lfeResetSeconds = 10.0f;  // native-LFE latch: silence needed before Auto generation may resume
                                    // (v1.4.0: whole seconds 1..300, default 10)
    // v1.4.0
    EqConfig eq = eqDefaultConfig();   // custom per-channel EQ (master off, every band flat)
    // v1.5.0: no subwoofer - fold the source's own LFE into the front speakers (true) or not.
    // v1.6.0: only honoured while expLfeFold is on; false now passes the LFE through (1.5
    // discarded it). An output without an LFE channel (PCM 2.0) always folds.
    bool lfeFold = true;
    // v1.6.0 experimental gates: off for new installs AND after every upgrade - the file stamps
    // them with the version that saved them (exp_gates_version) and any other version loads them
    // as off. The settings they guard (lfeGen, gameLfeGen, lfeFold) are loaded and saved
    // unchanged but not honoured while off.
    bool expLfeGen = false;
    bool expLfeFold = false;
};

// Defaults a file written by an older version implies for keys it does not contain (v1.5.0
// changed these defaults for new installs only; an existing configuration keeps behaving as before).
inline constexpr GamingMode kLegacyGamingDefault = GamingMode::Auto;
inline constexpr bool kLegacyGameLfeGenDefault = false;

std::filesystem::path defaultDataDir(bool serviceMode);
bool loadConfig(const std::filesystem::path& file, EngineConfig& cfg);
bool saveConfig(const std::filesystem::path& file, const EngineConfig& cfg);

} // namespace avr::win
