#include "ConfigStore.h"

#include "WinUtil.h"

#include "../core/LiveParams.h"
#include "../core/Version.h"
#include "../ipc/ControlProtocol.h"

#include <shlobj.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <map>

#pragma comment(lib, "shell32.lib")

namespace avr::win {

std::filesystem::path defaultDataDir(bool serviceMode) {
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(serviceMode ? FOLDERID_ProgramData : FOLDERID_LocalAppData, 0, nullptr, &p)))
        dir = std::filesystem::path(p) / L"LegacyAVR";
    CoTaskMemFree(p);
    if (dir.empty()) dir = std::filesystem::temp_directory_path() / L"LegacyAVR";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool loadConfig(const std::filesystem::path& file, EngineConfig& cfg) {
    std::ifstream in(file);
    if (!in) return false;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto f = [&](const char* k, float& v) { if (auto it = kv.find(k); it != kv.end()) if (auto x = ipc::parseFloat(it->second)) v = *x; };
    auto b = [&](const char* k, bool& v) { if (auto it = kv.find(k); it != kv.end()) if (auto x = ipc::parseBool(it->second)) v = *x; };

    if (auto it = kv.find("input_device"); it != kv.end()) cfg.inputDeviceId = toWide(it->second);
    if (auto it = kv.find("output_device"); it != kv.end()) cfg.outputDeviceId = toWide(it->second);
    if (auto it = kv.find("codec"); it != kv.end()) if (auto c = ipc::parseCodec(it->second)) cfg.codec = *c;
    if (auto it = kv.find("bitrate"); it != kv.end()) if (auto v = ipc::parseInt(it->second)) cfg.bitrate = uint32_t(*v);
    if (auto it = kv.find("output_mode"); it != kv.end()) if (auto m = ipc::parseOutputMode(it->second)) cfg.outputMode = *m;
    if (auto it = kv.find("buffer_ms"); it != kv.end()) if (auto v = ipc::parseInt(it->second)) cfg.bufferMs = uint32_t(*v);
    if (auto it = kv.find("upmix"); it != kv.end()) if (auto m = ipc::parseUpmixMode(it->second)) cfg.upmix = *m;
    b("stereo_as_20", cfg.stereoAs20);
    b("night_mode", cfg.nightMode);
    f("master_db", cfg.masterDb);
    for (size_t c = 0; c < kBusChannels; ++c) {
        f(std::format("gain{}", c).c_str(), cfg.gainDb[c]);
        bool m = cfg.mute[c];
        b(std::format("mute{}", c).c_str(), m);
        cfg.mute[c] = m;
    }
    f("centre_level", cfg.centreLevel);
    f("centre_floor", cfg.centreFloor);
    f("dialog_boost", cfg.dialogBoost);
    f("surround_level", cfg.surroundLevel);
    f("surround_delay_ms", cfg.surroundDelayMs);
    f("lfe_crossover_hz", cfg.lfeCrossoverHz);
    f("lfe_level_db", cfg.lfeLevelDb);
    f("bass_db", cfg.bassDb);
    b("dedicated_sub", cfg.dedicatedSub);
    if (auto it = kv.find("lfe_gen"); it != kv.end()) if (auto m = ipc::parseLfeGenMode(it->second)) cfg.lfeGen = *m;
    // A file from before v1.2 / v1.3 has no gaming / game_lfe_gen key: keep the defaults of the
    // version that wrote it, not the v1.5.0 new-install defaults.
    if (!kv.contains("gaming")) cfg.gaming = kLegacyGamingDefault;
    if (!kv.contains("game_lfe_gen")) cfg.gameLfeGen = kLegacyGameLfeGenDefault;
    if (auto it = kv.find("gaming"); it != kv.end()) if (auto m = ipc::parseGamingMode(it->second)) cfg.gaming = *m;
    b("game_lfe_gen", cfg.gameLfeGen);
    b("lfe_fold", cfg.lfeFold);
    // v1.6.0: experimental gates are honoured only by the version that saved them, so they are
    // off for new installs AND after every upgrade (or downgrade). Absent (any file before 1.6.0)
    // = off. The LFE settings they guard (lfe_gen, lfe_fold, game_lfe_gen) are never changed.
    cfg.expLfeGen = cfg.expLfeFold = false;
    if (auto it = kv.find("exp_gates_version"); it != kv.end() && it->second == kVersionString) {
        b("exp_lfe_gen", cfg.expLfeGen);
        b("exp_lfe_fold", cfg.expLfeFold);
    }
    f("lfe_reset_s", cfg.lfeResetSeconds);
    cfg.lfeResetSeconds = clampLfeResetSeconds(cfg.lfeResetSeconds);   // older files may hold up to 600
    b("eq_enabled", cfg.eq.enabled);
    for (size_t c = 0; c < kBusChannels; ++c) {
        bool on = cfg.eq.channelOn[c];
        b(std::format("eq{}_on", c).c_str(), on);
        cfg.eq.channelOn[c] = on;
        for (size_t band = 0; band < kEqBands; ++band)
            if (auto it = kv.find(std::format("eq{}_b{}", c, band)); it != kv.end())
                if (auto v = ipc::parseEqBand(it->second)) cfg.eq.bands[c][band] = eqClampBand(c, band, *v);
    }
    if (cfg.bufferMs != 0) cfg.bufferMs = std::clamp<uint32_t>(cfg.bufferMs, 5, 200);
    return true;
}

bool saveConfig(const std::filesystem::path& file, const EngineConfig& cfg) {
    const auto tmp = std::filesystem::path(file).concat(L".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "# LegacyAVR engine configuration (written by AVREngine)\n";
        out << "input_device=" << toUtf8(cfg.inputDeviceId) << '\n';
        out << "output_device=" << toUtf8(cfg.outputDeviceId) << '\n';
        out << "codec=" << ipc::codecToken(cfg.codec) << '\n';
        out << "bitrate=" << cfg.bitrate << '\n';
        out << "output_mode=" << ipc::outputModeToken(cfg.outputMode) << '\n';
        out << "buffer_ms=" << cfg.bufferMs << '\n';
        out << "upmix=" << ipc::upmixToken(cfg.upmix) << '\n';
        out << "stereo_as_20=" << (cfg.stereoAs20 ? 1 : 0) << '\n';
        out << "night_mode=" << (cfg.nightMode ? 1 : 0) << '\n';
        out << "master_db=" << cfg.masterDb << '\n';
        for (size_t c = 0; c < kBusChannels; ++c) {
            out << "gain" << c << '=' << cfg.gainDb[c] << '\n';
            out << "mute" << c << '=' << (cfg.mute[c] ? 1 : 0) << '\n';
        }
        out << "centre_level=" << cfg.centreLevel << '\n';
        out << "centre_floor=" << cfg.centreFloor << '\n';
        out << "dialog_boost=" << cfg.dialogBoost << '\n';
        out << "surround_level=" << cfg.surroundLevel << '\n';
        out << "surround_delay_ms=" << cfg.surroundDelayMs << '\n';
        out << "lfe_crossover_hz=" << cfg.lfeCrossoverHz << '\n';
        out << "lfe_level_db=" << cfg.lfeLevelDb << '\n';
        out << "bass_db=" << cfg.bassDb << '\n';
        out << "dedicated_sub=" << (cfg.dedicatedSub ? 1 : 0) << '\n';
        out << "lfe_gen=" << ipc::lfeGenToken(cfg.lfeGen) << '\n';
        out << "gaming=" << ipc::gamingToken(cfg.gaming) << '\n';
        out << "game_lfe_gen=" << (cfg.gameLfeGen ? 1 : 0) << '\n';
        out << "lfe_fold=" << (cfg.lfeFold ? 1 : 0) << '\n';
        out << "exp_gates_version=" << kVersionString << '\n';
        out << "exp_lfe_gen=" << (cfg.expLfeGen ? 1 : 0) << '\n';
        out << "exp_lfe_fold=" << (cfg.expLfeFold ? 1 : 0) << '\n';
        out << "lfe_reset_s=" << cfg.lfeResetSeconds << '\n';
        out << "eq_enabled=" << (cfg.eq.enabled ? 1 : 0) << '\n';
        for (size_t c = 0; c < kBusChannels; ++c) {
            out << "eq" << c << "_on=" << (cfg.eq.channelOn[c] ? 1 : 0) << '\n';
            for (size_t band = 0; band < kEqBands; ++band)
                out << "eq" << c << "_b" << band << '=' << ipc::eqBandToken(cfg.eq.bands[c][band]) << '\n';
        }
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);   // atomic replace on NTFS
    return !ec;
}

} // namespace avr::win
