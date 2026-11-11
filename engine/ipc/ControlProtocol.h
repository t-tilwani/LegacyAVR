// LegacyAVR - control protocol over the named pipe \\.\pipe\LegacyAVR.Control
//
// UTF-8, one request per line ("\n" terminated). Tokens are separated by spaces;
// a token containing spaces may be wrapped in double quotes.
// Every response is one status line, optional data lines, then a line "END":
//     OK [message]          |   ERR <message>
//     <data lines...>
//     END
//
// Requests
//   PING                                  -> OK pong
//   GET_STATE                             -> key=value lines (full configuration)
//   LIST_DEVICES                          -> DEV<TAB>flow<TAB>id<TAB>name<TAB>default<TAB>channels<TAB>rate<TAB>iecCaps
//                                            flow: render|capture ; iecCaps bit0 AC3, bit1 DTS, bit2 E-AC-3
//   SET_INPUT_DEVICE <id|default>         (render endpoint => loopback; capture endpoint => direct)
//   SET_OUTPUT_DEVICE <id|default>
//   SET_CODEC AC3|DTS|EAC3|PCM
//   SET_BITRATE <bits per second>
//   SET_OUTPUT_MODE EXCLUSIVE|SHARED
//   SET_BUFFER_MS 0|<5..200>              output safety margin; 0 = automatic (lowest safe)
//   SET_UPMIX PASSTHROUGH|UPMIX31|UPMIX51|PCM_DIRECT
//   SET_UPMIX_PARAM CENTRE|CENTRE_FLOOR|DIALOG|SURROUND|DELAY|LFE_XOVER|LFE_LEVEL <value>
//   SET_STEREO_AS_20 0|1
//   SET_NIGHT_MODE 0|1
//   SET_MASTER <dB>
//   SET_GAIN <channel 0-5> <dB>           (<= -48.5 dB means muted)
//   SET_MUTE <channel 0-5> 0|1
//   SET_MUTE_ALL 0|1
//   SET_BASS <dB>                         bass shelf on every channel, -12..+12      (v1.2.0)
//   SET_SUB 0|1                           dedicated subwoofer active (1) / inactive (0) (v1.2.0)
//   SET_LFE_GEN OFF|AUTO|ALWAYS           5.0 -> 5.1 LFE generation                   (v1.2.0)
//                                         (v1.5.0: only with a dedicated subwoofer)
//   SET_LFE_FOLD 0|1                      no subwoofer + experimental fold on: fold the source's
//                                         LFE into FL/FR (1) or keep it in the LFE channel (0)
//                                         (v1.5.0; v1.6.0 meaning)
//   SET_EXP_LFE_GEN 0|1                   experimental gate for LFE generation (v1.6.0)
//   SET_EXP_LFE_FOLD 0|1                  experimental gate for the optional LFE fold (v1.6.0)
//   PLAY_CHIME                            play the activation chime once through the output;
//                                         ERR with the reason when it cannot (v1.6.0)
//   SET_GAMING AUTO|ON|OFF                game handling mode                          (v1.2.0)
//   SET_GAME_LFE 0|1                      Game Mode's own LFE generation toggle       (v1.3.0)
//   SET_LFE_RESET <seconds>               native-LFE latch reset interval: whole seconds
//                                         1..300 (default 10); anything else is refused (v1.4.0)
//   SET_EQ 0|1                            custom EQ master enable / bypass              (v1.4.0)
//   SET_EQ_CHANNEL <ch 0-5> 0|1           per-channel EQ enable / bypass               (v1.4.0)
//   SET_EQ_BAND <ch 0-5> <band 0-4> <Hz> <dB> <Q>
//                                         band 0 low shelf, 1-3 bells, 4 high shelf; gain
//                                         +/-6 dB, Q 0.5..4, 20 Hz..20 kHz (LFE 20..200 Hz).
//                                         Values are clamped; the reply carries them  (v1.4.0)
//   RESET_EQ_CHANNEL <ch 0-5>             that channel's five bands back to flat       (v1.4.0)
//   SET_AUDITION <ch 0-5>|OFF             solo one channel while tuning the EQ. Temporary:
//                                         not saved, saved mutes untouched, cleared 6 s after
//                                         the last refresh                              (v1.4.0)
//   GAME_DETECTED 0|1 ["name"]            dashboard heartbeat while a game is in the
//                                         foreground (Auto mode); expires after 6 s   (v1.2.0)
//   TEST OFF|PINK|CHANNEL_ID|CENTER_BLEED|LFE_SWEEP|PHASE [mask hex, pink only]
//   STOP_AUDIO                            stop the audio pipeline (engine process keeps running)
//   START_AUDIO                           start it again
//   RESTART
//   SHUTDOWN                              exit the engine process (refused in service mode)
#pragma once

#include "../core/ChannelEq.h"
#include "../core/Types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace avr::ipc {

enum class Cmd {
    Unknown, Ping, GetState, ListDevices,
    SetInputDevice, SetOutputDevice, SetCodec, SetBitrate, SetOutputMode, SetBufferMs,
    SetUpmix, SetUpmixParam, SetStereoAs20, SetNightMode, SetMaster, SetGain, SetMute, SetMuteAll,
    Test, Restart, Shutdown, StopAudio, StartAudio,
    SetBass, SetSub, SetLfeGen, SetGaming, GameDetected, SetGameLfe, SetLfeReset, SetLfeFold,
    SetExpLfeGen, SetExpLfeFold, PlayChime,
    SetEq, SetEqChannel, SetEqBand, ResetEqChannel, SetAudition,
};

struct Request {
    Cmd cmd = Cmd::Unknown;
    std::string verb;
    std::vector<std::string> args;
};

Request parseRequest(std::string_view line);
std::vector<std::string> tokenize(std::string_view line);

std::optional<Codec> parseCodec(std::string_view s);
std::optional<OutputMode> parseOutputMode(std::string_view s);
std::optional<UpmixMode> parseUpmixMode(std::string_view s);
std::optional<TestSignal> parseTestSignal(std::string_view s);
std::optional<LfeGenMode> parseLfeGenMode(std::string_view s);
std::optional<GamingMode> parseGamingMode(std::string_view s);
std::optional<float> parseFloat(std::string_view s);
std::optional<long long> parseInt(std::string_view s, int base = 10);
std::optional<bool> parseBool(std::string_view s);
// v1.4.0: whole seconds 1..300 (accepts "10", "10.0"); nullopt otherwise.
std::optional<float> parseLfeResetSeconds(std::string_view s);
// v1.4.0: "freq,gain,q" (configuration file form of one EQ band).
std::optional<EqBand> parseEqBand(std::string_view s);
std::string eqBandToken(const EqBand& b);

const char* codecToken(Codec c) noexcept;
const char* outputModeToken(OutputMode m) noexcept;
const char* upmixToken(UpmixMode m) noexcept;
const char* testToken(TestSignal t) noexcept;
const char* lfeGenToken(LfeGenMode m) noexcept;
const char* gamingToken(GamingMode m) noexcept;

class Response {
public:
    static Response ok(std::string message = {});
    static Response error(std::string message);
    Response& line(std::string text);
    Response& kv(std::string_view key, std::string_view value);
    [[nodiscard]] std::string serialise() const;
    [[nodiscard]] bool isOk() const noexcept { return ok_; }

private:
    bool ok_ = true;
    std::string status_;
    std::vector<std::string> lines_;
};

} // namespace avr::ipc
