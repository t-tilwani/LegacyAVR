#include "ControlProtocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../core/LiveParams.h"

namespace avr::ipc {

namespace {
std::string upper(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return r;
}
std::string sanitiseLine(std::string s) {
    // Data lines must never contain a bare newline (it would break framing).
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::replace(s.begin(), s.end(), '\r', ' ');
    return s;
}
} // namespace

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= line.size()) break;
        std::string tok;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') tok.push_back(line[i++]);
            if (i < line.size()) ++i;   // closing quote
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') tok.push_back(line[i++]);
        }
        out.push_back(std::move(tok));
    }
    return out;
}

Request parseRequest(std::string_view line) {
    Request r;
    auto toks = tokenize(line);
    if (toks.empty()) return r;
    r.verb = upper(toks.front());
    r.args.assign(toks.begin() + 1, toks.end());

    struct Entry { std::string_view name; Cmd cmd; };
    static constexpr Entry table[] = {
        {"PING", Cmd::Ping}, {"GET_STATE", Cmd::GetState}, {"LIST_DEVICES", Cmd::ListDevices},
        {"SET_INPUT_DEVICE", Cmd::SetInputDevice}, {"SET_OUTPUT_DEVICE", Cmd::SetOutputDevice},
        {"SET_CODEC", Cmd::SetCodec}, {"SET_BITRATE", Cmd::SetBitrate},
        {"SET_OUTPUT_MODE", Cmd::SetOutputMode}, {"SET_BUFFER_MS", Cmd::SetBufferMs},
        {"SET_UPMIX", Cmd::SetUpmix}, {"SET_UPMIX_PARAM", Cmd::SetUpmixParam},
        {"SET_STEREO_AS_20", Cmd::SetStereoAs20}, {"SET_NIGHT_MODE", Cmd::SetNightMode},
        {"SET_MASTER", Cmd::SetMaster}, {"SET_GAIN", Cmd::SetGain}, {"SET_MUTE", Cmd::SetMute},
        {"SET_MUTE_ALL", Cmd::SetMuteAll}, {"TEST", Cmd::Test}, {"RESTART", Cmd::Restart},
        {"SHUTDOWN", Cmd::Shutdown}, {"STOP_AUDIO", Cmd::StopAudio}, {"START_AUDIO", Cmd::StartAudio},
        {"SET_BASS", Cmd::SetBass}, {"SET_SUB", Cmd::SetSub}, {"SET_LFE_GEN", Cmd::SetLfeGen},
        {"SET_GAMING", Cmd::SetGaming}, {"GAME_DETECTED", Cmd::GameDetected},
        {"SET_GAME_LFE", Cmd::SetGameLfe}, {"SET_LFE_RESET", Cmd::SetLfeReset},
        {"SET_LFE_FOLD", Cmd::SetLfeFold},
        {"SET_EQ", Cmd::SetEq}, {"SET_EQ_CHANNEL", Cmd::SetEqChannel}, {"SET_EQ_BAND", Cmd::SetEqBand},
        {"RESET_EQ_CHANNEL", Cmd::ResetEqChannel}, {"SET_AUDITION", Cmd::SetAudition},
    };
    for (const auto& e : table)
        if (e.name == r.verb) { r.cmd = e.cmd; break; }
    return r;
}

std::optional<Codec> parseCodec(std::string_view s) {
    const auto u = upper(s);
    if (u == "AC3" || u == "DD") return Codec::AC3;
    if (u == "DTS") return Codec::DTS;
    if (u == "EAC3" || u == "DDP") return Codec::EAC3;
    if (u == "PCM") return Codec::PCM;
    return std::nullopt;
}
std::optional<OutputMode> parseOutputMode(std::string_view s) {
    const auto u = upper(s);
    if (u == "EXCLUSIVE") return OutputMode::Exclusive;
    if (u == "SHARED") return OutputMode::Shared;
    return std::nullopt;
}
std::optional<UpmixMode> parseUpmixMode(std::string_view s) {
    const auto u = upper(s);
    if (u == "PASSTHROUGH") return UpmixMode::Passthrough;
    if (u == "UPMIX31") return UpmixMode::Upmix31;
    if (u == "UPMIX51") return UpmixMode::Upmix51;
    if (u == "PCM_DIRECT") return UpmixMode::PcmDirect;
    return std::nullopt;
}
std::optional<TestSignal> parseTestSignal(std::string_view s) {
    const auto u = upper(s);
    if (u == "OFF") return TestSignal::Off;
    if (u == "PINK") return TestSignal::PinkNoise;
    if (u == "CHANNEL_ID") return TestSignal::ChannelId;
    if (u == "CENTER_BLEED") return TestSignal::CenterBleed;
    if (u == "LFE_SWEEP") return TestSignal::LfeSweep;
    if (u == "PHASE") return TestSignal::PhaseCheck;
    return std::nullopt;
}
std::optional<LfeGenMode> parseLfeGenMode(std::string_view s) {
    const auto u = upper(s);
    if (u == "OFF") return LfeGenMode::Off;
    if (u == "AUTO") return LfeGenMode::Auto;
    if (u == "ALWAYS" || u == "ON") return LfeGenMode::Always;
    return std::nullopt;
}
std::optional<GamingMode> parseGamingMode(std::string_view s) {
    const auto u = upper(s);
    if (u == "AUTO") return GamingMode::Auto;
    if (u == "ON" || u == "ALWAYS") return GamingMode::On;
    if (u == "OFF") return GamingMode::Off;
    return std::nullopt;
}
std::optional<float> parseFloat(std::string_view s) {
    float v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}
std::optional<long long> parseInt(std::string_view s, int base) {
    if (base == 16 && s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    long long v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}
std::optional<bool> parseBool(std::string_view s) {
    const auto u = upper(s);
    if (u == "1" || u == "ON" || u == "TRUE") return true;
    if (u == "0" || u == "OFF" || u == "FALSE") return false;
    return std::nullopt;
}

std::optional<float> parseLfeResetSeconds(std::string_view s) {
    const auto v = parseFloat(s);
    if (!v || !(*v == *v)) return std::nullopt;
    const float r = std::round(*v);
    if (std::fabs(*v - r) > 1e-3f) return std::nullopt;          // whole seconds only
    if (r < kLfeResetMinSeconds || r > kLfeResetMaxSeconds) return std::nullopt;
    return r;
}

std::optional<EqBand> parseEqBand(std::string_view s) {
    float v[3];
    for (int i = 0; i < 3; ++i) {
        const size_t comma = s.find(',');
        const std::string_view part = i < 2 ? s.substr(0, comma) : s;
        if (i < 2 && comma == std::string_view::npos) return std::nullopt;
        const auto f = parseFloat(part);
        if (!f) return std::nullopt;
        v[i] = *f;
        if (i < 2) s.remove_prefix(comma + 1);
    }
    EqBand b;
    b.freqHz = v[0];
    b.gainDb = v[1];
    b.q = v[2];
    return b;
}

std::string eqBandToken(const EqBand& b) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f,%.2f,%.2f", b.freqHz, b.gainDb, b.q);
    return buf;
}

const char* codecToken(Codec c) noexcept {
    switch (c) { case Codec::AC3: return "AC3"; case Codec::DTS: return "DTS";
                 case Codec::EAC3: return "EAC3"; case Codec::PCM: return "PCM"; }
    return "AC3";
}
const char* outputModeToken(OutputMode m) noexcept { return m == OutputMode::Shared ? "SHARED" : "EXCLUSIVE"; }
const char* upmixToken(UpmixMode m) noexcept {
    switch (m) { case UpmixMode::Passthrough: return "PASSTHROUGH";
                 case UpmixMode::Upmix31: return "UPMIX31"; case UpmixMode::Upmix51: return "UPMIX51";
                 case UpmixMode::PcmDirect: return "PCM_DIRECT"; }
    return "UPMIX51";
}
const char* testToken(TestSignal t) noexcept {
    switch (t) {
        case TestSignal::Off: return "OFF"; case TestSignal::PinkNoise: return "PINK";
        case TestSignal::ChannelId: return "CHANNEL_ID"; case TestSignal::CenterBleed: return "CENTER_BLEED";
        case TestSignal::LfeSweep: return "LFE_SWEEP"; case TestSignal::PhaseCheck: return "PHASE";
    }
    return "OFF";
}

const char* lfeGenToken(LfeGenMode m) noexcept {
    switch (m) { case LfeGenMode::Off: return "OFF"; case LfeGenMode::Auto: return "AUTO";
                 case LfeGenMode::Always: return "ALWAYS"; }
    return "AUTO";
}
const char* gamingToken(GamingMode m) noexcept {
    switch (m) { case GamingMode::Auto: return "AUTO"; case GamingMode::On: return "ON";
                 case GamingMode::Off: return "OFF"; }
    return "AUTO";
}

Response Response::ok(std::string message) {
    Response r;
    r.ok_ = true;
    r.status_ = message.empty() ? "OK" : "OK " + sanitiseLine(std::move(message));
    return r;
}
Response Response::error(std::string message) {
    Response r;
    r.ok_ = false;
    r.status_ = "ERR " + sanitiseLine(std::move(message));
    return r;
}
Response& Response::line(std::string text) {
    lines_.push_back(sanitiseLine(std::move(text)));
    return *this;
}
Response& Response::kv(std::string_view key, std::string_view value) {
    std::string s(key);
    s.push_back('=');
    s.append(value);
    return line(std::move(s));
}
std::string Response::serialise() const {
    std::string out = status_;
    out.push_back('\n');
    for (const auto& l : lines_) {
        if (l == "END") { out += " END\n"; continue; }   // never emit a premature terminator
        out += l;
        out.push_back('\n');
    }
    out += "END\n";
    return out;
}

} // namespace avr::ipc
