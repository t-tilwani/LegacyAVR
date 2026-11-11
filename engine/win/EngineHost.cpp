#include "EngineHost.h"

#include "Logger.h"
#include "SpeechClips.h"

#include "../core/ChannelMatrix.h"
#include "../core/StatusText.h"
#include "../ipc/ControlProtocol.h"
#include "../ipc/TelemetryLayout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace avr::win {

// ===========================================================================
// Device notifications (IMMNotificationClient) - called on an MMDevice thread.
// ===========================================================================
class DeviceWatcher final : public IMMNotificationClient {
public:
    explicit DeviceWatcher(EngineHost* host) : host_(host) {}

    bool attach() {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en_))))
            return false;
        return SUCCEEDED(en_->RegisterEndpointNotificationCallback(this));
    }
    void detach() {
        if (en_) en_->UnregisterEndpointNotificationCallback(this);
        en_.Reset();
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        host_->onDeviceTopologyChanged(id ? id : L"", false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        host_->onDeviceTopologyChanged(id ? id : L"", false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        host_->onDeviceTopologyChanged(id ? id : L"", false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        if (flow == eRender && role == eConsole) host_->onDeviceTopologyChanged(id ? id : L"", true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~DeviceWatcher() = default;
    EngineHost* host_;
    ComPtr<IMMDeviceEnumerator> en_;
    LONG ref_ = 1;
};

// ===========================================================================
namespace {

std::string rateText(uint32_t r) {
    return r % 1000 == 0 ? std::format("{} kHz", r / 1000) : std::format("{:.1f} kHz", r / 1000.0);
}
std::string bitrateText(uint32_t b) {
    return b % 1000 == 0 ? std::format("{} kbps", b / 1000) : std::format("{:.2f} kbps", b / 1000.0);
}
float toDb(float lin) { return linToDb(lin); }

constexpr uint64_t kGameHeartbeatUs = 6'000'000;   // GAME_DETECTED expires without a refresh
constexpr uint64_t kAuditionHeartbeatUs = 6'000'000; // v1.4.0: SET_AUDITION expires without a refresh

// "C, LFE, SL" for a speaker mask.
std::string speakerList(uint32_t mask) {
    static constexpr std::pair<uint32_t, const char*> names[] = {
        {SPK_FL, "FL"}, {SPK_FR, "FR"}, {SPK_FC, "C"}, {SPK_LFE, "LFE"}, {SPK_BL, "BL"}, {SPK_BR, "BR"},
        {SPK_FLC, "FLC"}, {SPK_FRC, "FRC"}, {SPK_BC, "BC"}, {SPK_SL, "SL"}, {SPK_SR, "SR"}};
    std::string out;
    for (const auto& [bit, name] : names) {
        if (!(mask & bit)) continue;
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? std::string("-") : out;
}



std::string pcmSwitchText(PcmSwitchStatus st, uint32_t blockMask, float holdSec, Codec requested) {
    const char* fmtName = codecName(requested);
    switch (st) {
        case PcmSwitchStatus::NotApplicable: return {};
        case PcmSwitchStatus::WaitingForStereo:
            return std::format("Stereo for {:.1f} s - switching to lossless PCM 2.0 after 1.5 s", holdSec);
        case PcmSwitchStatus::Requested: return "Stereo is stable - re-opening the output as lossless PCM 2.0";
        case PcmSwitchStatus::Active:
            return std::format("Lossless PCM 2.0 - returns to {} when surround, a game or a test signal starts", fmtName);
        case PcmSwitchStatus::BlockedSurround:
            return std::format("Staying on {}: the source has signal on {} (not plain stereo)", fmtName, speakerList(blockMask));
        case PcmSwitchStatus::BlockedTest: return std::format("Staying on {}: a test signal is playing", fmtName);
        case PcmSwitchStatus::BlockedPassthrough: return std::format("Staying on {}: pre-encoded input is forwarded", fmtName);
        case PcmSwitchStatus::BlockedOutput:
            return "Lossless PCM 2.0 needs WASAPI Exclusive with a Dolby/DTS format selected";
        case PcmSwitchStatus::BlockedGaming: return std::format("Staying on {}: game mode is active", fmtName);
        case PcmSwitchStatus::NoSignal: return "Waiting for stereo audio";
        case PcmSwitchStatus::ReturningToBitstream: return std::format("Multichannel / game / test audio - returning to {}", fmtName);
    }
    return {};
}

} // namespace

// ===========================================================================
EngineHost::EngineHost(HostOptions opts) : opts_(std::move(opts)) {
    for (auto& g : params_.gainDb) g.store(0.0f);
    for (auto& m : params_.mute) m.store(false);
}

EngineHost::~EngineHost() {
    stopPipeline();
    pipe_.stop();
    if (telemetryThread_.joinable()) {
        SetEvent(telemetryStop_.get());
        telemetryThread_.join();
    }
    if (watcher_) { watcher_->detach(); watcher_->Release(); watcher_ = nullptr; }
    telemetry_.close();
}

void EngineHost::setError(std::string msg) {
    AVR_LOGE("{}", msg);
    std::lock_guard lk(errMutex_);
    lastError_ = std::move(msg);
}

void EngineHost::setEffective(OutputStatus status, std::string reason) {
    std::lock_guard lk(errMutex_);
    outputStatus_ = status;
    effectiveReason_ = std::move(reason);
}

void EngineHost::markDirty() noexcept {
    configDirty_ = true;
    configRevision_.fetch_add(1, std::memory_order_relaxed);
}

bool EngineHost::initialise(std::string& error) {
    instanceMutex_.reset(CreateMutexW(nullptr, TRUE, L"Global\\LegacyAVR.EngineInstance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        error = "another AVREngine / AVREngineService instance is already running";
        return false;
    }

    const auto dir = opts_.dataDir.empty() ? defaultDataDir(opts_.serviceMode) : opts_.dataDir;
    configFile_ = dir / L"engine.cfg";
    Logger::instance().open((dir / L"AVREngine.log").wstring(), opts_.console);
    AVR_LOGI("LegacyAVR engine starting ({} mode), data dir {}", opts_.serviceMode ? "service" : "user",
             toUtf8(dir.wstring()));

    AudioEncoder::installLogHandler([](int level, const char* msg) {
        std::string s(msg);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (level <= 16 /*AV_LOG_ERROR*/) AVR_LOGE("ffmpeg: {}", s);
        else AVR_LOGW("ffmpeg: {}", s);
    });

    if (!loadConfig(configFile_, cfg_)) AVR_LOGI("no saved configuration, using defaults");
    applyLiveParams();

    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    restartEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    dspWake_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    dspStop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    telemetryStop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    if (!telemetry_.open(opts_.serviceMode, error)) return false;

    std::string speechErr;
    speech_ = synthesiseChannelNames(speechErr);
    speechAvailable_ = std::all_of(speech_.begin(), speech_.end(), [](const auto& v) { return !v.empty(); });
    if (!speechAvailable_) AVR_LOGW("channel-ID speech unavailable ({}), using beep codes", speechErr);

    watcher_ = new DeviceWatcher(this);
    if (!watcher_->attach()) AVR_LOGW("device notifications unavailable");

    if (!pipe_.start(opts_.serviceMode, [this](const std::string& l) { return handleCommand(l); }, error))
        return false;

    telemetryThread_ = std::thread([this] { telemetryLoop(); });
    return true;
}

void EngineHost::applyLiveParams() noexcept {
    std::lock_guard lk(cfgMutex_);
    params_.masterDb = cfg_.masterDb;
    for (size_t c = 0; c < kBusChannels; ++c) {
        params_.gainDb[c] = cfg_.gainDb[c];
        params_.mute[c] = cfg_.mute[c];
    }
    params_.nightMode = cfg_.nightMode;
    params_.upmixMode = static_cast<uint32_t>(cfg_.upmix);
    params_.stereoAsTwoChannel = cfg_.stereoAs20;
    params_.centreLevel = cfg_.centreLevel;
    params_.centreFloor = cfg_.centreFloor;
    params_.dialogBoost = cfg_.dialogBoost;
    params_.surroundLevel = cfg_.surroundLevel;
    params_.surroundDelayMs = cfg_.surroundDelayMs;
    params_.lfeCrossoverHz = cfg_.lfeCrossoverHz;
    params_.lfeLevelDb = cfg_.lfeLevelDb;
    params_.bassDb = cfg_.bassDb;
    params_.lfeGenMode = static_cast<uint32_t>(cfg_.lfeGen);
    params_.gameLfeGen = cfg_.gameLfeGen;
    params_.lfeFold = cfg_.lfeFold;
    params_.expLfeGen = cfg_.expLfeGen;
    params_.expLfeFold = cfg_.expLfeFold;
    params_.lfeResetSeconds = clampLfeResetSeconds(cfg_.lfeResetSeconds);
    params_.setEq(cfg_.eq);
    params_.upmixRevision.fetch_add(1, std::memory_order_release);
    params_.muteAll = muteAll_.load();
}

void EngineHost::expireAudition(uint64_t nowUs) noexcept {
    // The dashboard refreshes SET_AUDITION every 2 s while the EQ dialog auditions a channel;
    // if it stops (dialog closed, dashboard crashed) the solo must not stay on.
    const uint64_t seen = auditionSeenUs_.load();
    if (params_.auditionChannel.load() >= 0 && (seen == 0 || nowUs - seen > kAuditionHeartbeatUs)) {
        params_.auditionChannel = -1;
        AVR_LOGI("EQ audition ended (no refresh from the dashboard)");
    }
}

void EngineHost::updateGaming(uint64_t nowUs) noexcept {
    GamingMode mode;
    { std::lock_guard lk(cfgMutex_); mode = cfg_.gaming; }
    const uint64_t seen = gameSeenUs_.load();
    const bool detected = seen != 0 && nowUs - seen < kGameHeartbeatUs;
    const bool active = mode == GamingMode::On || (mode == GamingMode::Auto && detected);
    if (params_.gamingActive.exchange(active) != active)
        AVR_LOGI("game mode {}", active ? "active: stereo is not upmixed and the output format is kept" : "off");
}

// ===========================================================================
// Pipeline lifecycle (supervisor thread only)
// ===========================================================================
bool EngineHost::startPipeline(std::string& error) {
    std::lock_guard pl(pipelineMutex_);
    state_ = EngineState::Starting;
    EngineConfig cfg;
    { std::lock_guard lk(cfgMutex_); cfg = cfg_; }

    if (cfg.outputDeviceId.empty()) {
        error = "No output device selected. Pick your HDMI ARC / S/PDIF device in LegacyAVR (Output tab).";
        return false;
    }

    // ---- capture -------------------------------------------------------------------
    if (!capture_.open(cfg.inputDeviceId, 20, error)) { error = "Input: " + error; return false; }
    const auto& inFmt = capture_.format();
    AVR_LOGI("input  '{}' {} ({})", capture_.deviceName(), capture_.formatText(),
             capture_.isLoopback() ? "loopback" : "capture");

    // Feedback guard: never loop back the device we are rendering to.
    {
        std::string e2;
        auto outDev = openDevice(cfg.outputDeviceId, eRender, e2);
        if (outDev && deviceId(outDev.Get()) == capture_.resolvedId()) {
            error = "Input and output are the same endpoint. Set the virtual cable as the Windows default "
                    "device / LegacyAVR input, and the HDMI/S-PDIF device as output.";
            return false;
        }
    }

    // ---- renderer ------------------------------------------------------------------
    // v1.2.0: the decision of what to open lives in core/OutputPolicy (unit-tested).
    OutputRequest oreq;
    oreq.codec = cfg.codec;
    oreq.mode = cfg.outputMode;
    oreq.upmix = cfg.upmix;
    oreq.passthroughIec = passthroughIec_;
    oreq.stereoPcmActive = stereoPcmActive_.load();
    oreq.dedicatedSub = cfg.dedicatedSub;
    oreq.inputHasLfe = (inFmt.mask & SPK_LFE) != 0 || (inFmt.mask == 0 && inFmt.channels >= 6);
    oreq.devicePcmChannels = WasapiRenderer::queryMixChannels(cfg.outputDeviceId);
    const OutputPlan plan = planOutput(oreq);
    if (stereoPcmActive_ && !plan.stereoPcm) stereoPcmActive_ = false;   // mode / format changed meanwhile
    Codec codec = plan.codec;
    pcmChannels_ = plan.pcmChannels;
    OutputMode mode = cfg.outputMode;
    const bool stereoPcm = plan.stereoPcm;

    // Remember what the device can bitstream while it is still free to answer
    // (once we hold it in exclusive mode it can no longer be probed).
    if (const uint32_t caps = probeIecCaps(cfg.outputDeviceId); caps & kIecCapsKnown) {
        std::lock_guard lk(capsMutex_);
        iecCapsCache_[cfg.outputDeviceId] = caps;
    }

    bool fellBack = false;
    std::string fallbackWhy;

    RenderRequest rr;
    rr.mode = mode;
    // Automatic margin: a 5 ms render period (or the device minimum).
    rr.bufferMs = cfg.bufferMs == 0 ? 10u : cfg.bufferMs;
    rr.pcmChannels = uint16_t(pcmChannels_);
    rr.iec = plan.iec;

    if (!renderer_.open(cfg.outputDeviceId, rr, error)) {
        if (mode == OutputMode::Exclusive) {
            const std::string why = error;
            rr.mode = OutputMode::Shared;
            rr.iec = IecType::None;
            if (!renderer_.open(cfg.outputDeviceId, rr, error)) { error = "Output: " + error; return false; }
            mode = OutputMode::Shared;
            fellBack = true;
            fallbackWhy = why;
            setError(std::string(stereoPcm ? "Exclusive PCM 2.0" : "Exclusive/bitstream") + " output failed (" + why +
                     "). Running in Shared PCM compatibility mode.");
        } else {
            error = "Output: " + error;
            return false;
        }
    }
    AVR_LOGI("output '{}' {} period {} frames, ~{:.1f} ms", renderer_.deviceName(), renderer_.formatText(),
             renderer_.periodBusFrames(), renderer_.latencyMs());

    // ---- rings + core ------------------------------------------------------------------
    captureRing_.allocate(size_t(inFmt.channels) * inFmt.rate / 4);          // 250 ms
    CoreConfig cc;
    cc.inRate = inFmt.rate;
    cc.inChannels = inFmt.channels;
    cc.inMask = inFmt.mask;
    cc.capturePacketFrames = uint32_t(std::lround(capture_.periodMs() * kBusRate / 1000.0));
    cc.codec = codec;
    cc.bitrate = passthroughIec_ != IecType::None ? defaultBitrate(codec) : cfg.bitrate;
    cc.outputMode = mode;
    cc.pcmChannels = uint16_t(pcmChannels_);
    cc.renderPeriodFrames = renderer_.periodBusFrames();
    cc.bufferMs = cfg.bufferMs;
    cc.stereoPcmOverride = stereoPcm;
    cc.dedicatedSub = cfg.dedicatedSub;
    const size_t bytesPerFrame = cc.bitstream()
        ? (codec == Codec::EAC3 ? 16u : 4u)
        : size_t(pcmChannels_) * sizeof(float);
    outputRing_.allocate(bytesPerFrame * kBusRate / 2);                        // 500 ms
    if (!core_.configure(cc, params_, captureRing_, outputRing_, error)) { error = "DSP: " + error; return false; }
    for (size_t c = 0; c < speech_.size(); ++c) core_.tests().setSpeechClip(c, speech_[c]);

    // ---- threads -----------------------------------------------------------------------
    ResetEvent(dspStop_.get());
    dspThread_ = std::thread([this] { dspLoop(); });
    if (!renderer_.start(&outputRing_, dspWake_.get(), error)) { error = "Output start: " + error; return false; }
    if (!capture_.start(&captureRing_, dspWake_.get(), error)) { error = "Input start: " + error; return false; }

    pipelineUp_ = true;
    state_ = EngineState::Running;
    iecMismatchSinceUs_ = 0;
    pcmSwitch_.reset();
    lastStartFellBack_ = fellBack;

    // ---- requested vs. effective (shown in the UI's "Effective output" line) -----------
    const OutputStatus status = effectiveStatus(true, false, fellBack, passthroughIec_, stereoPcm);
    std::string reason;
    switch (status) {
        case OutputStatus::FallbackShared: {
            const bool unsupported = fallbackWhy.find("0x88890008") != std::string::npos;
            const std::string what = stereoPcm ? std::string("Exclusive PCM 2.0")
                                   : cfg.codec == Codec::PCM ? std::string("Exclusive PCM")
                                                             : std::string(codecName(cfg.codec));
            reason = std::format("{} {} - playing PCM in shared mode instead", what,
                                 unsupported ? "is not supported by this output device (or the TV/AVR behind it)"
                                             : "could not be opened in exclusive mode");
            break;
        }
        case OutputStatus::BitstreamPassthrough:
            reason = std::format("Pre-encoded {} input is being forwarded untouched", iecTypeName(passthroughIec_));
            break;
        case OutputStatus::StereoPcmDirect:
            reason = std::format("Stereo source - lossless PCM 2.0; switches to {} when multichannel audio starts",
                                 codecName(cfg.codec));
            break;
        default: break;
    }
    setEffective(status, reason);
    const QueueSizing& qs = core_.sizing();
    AVR_LOGI("pipeline running: {} {} {}{} | {} | margin {:.1f} ms (floor {:.1f}, {}) period {:.1f} ms, packet {:.1f} ms",
             codecName(cc.effectiveCodec()),
             cc.bitstream() ? bitrateText(cc.bitrate) : std::format("{} ch", cc.pcmChannels),
             mode == OutputMode::Exclusive ? "exclusive" : "shared",
             stereoPcm ? " (lossless stereo PCM 2.0)" : "",
             cc.dedicatedSub ? (cfg.expLfeGen ? "subwoofer, LFE generation (experimental) on" : "subwoofer")
                             : (cfg.expLfeFold && cfg.lfeFold ? "no subwoofer: source LFE folded into the fronts (experimental)"
                                                               : "no subwoofer: source LFE passed through"),
             qs.marginFrames / 48.0, qs.floorFrames / 48.0, cfg.bufferMs == 0 ? "auto" : "user",
             cc.renderPeriodFrames / 48.0, cc.capturePacketFrames / 48.0);
    return true;
}

void EngineHost::stopPipeline() noexcept {
    std::lock_guard pl(pipelineMutex_);
    pipelineUp_ = false;
    capture_.stop();
    if (dspThread_.joinable()) {
        SetEvent(dspStop_.get());
        dspThread_.join();
    }
    renderer_.stop();
    state_ = EngineState::Stopped;
}

void EngineHost::dspLoop() noexcept {
    ComScope com;
    setThreadName(L"AVR DSP");
    MmcssScope mmcss(L"Pro Audio", AVRT_PRIORITY_HIGH);
    enableFlushToZero();
    HANDLE waits[2] = {dspStop_.get(), dspWake_.get()};
    for (;;) {
        // Woken by capture packets and by render callbacks; the 5 ms timeout keeps the
        // keep-alive/test generator ticking even if both are quiet.
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5);
        if (w == WAIT_OBJECT_0) break;
        core_.step(nowUs());
    }
}

void EngineHost::requestRestart(const char* reason) noexcept {
    AVR_LOGI("restart requested: {}", reason);
    SetEvent(restartEvent_.get());
}

void EngineHost::requestStop() noexcept { SetEvent(stopEvent_.get()); }

void EngineHost::onDeviceTopologyChanged(const std::wstring& id, bool defaultChanged) noexcept {
    bool relevant = false;
    {
        std::lock_guard lk(cfgMutex_);
        // Default-render change matters only when the input follows the default endpoint;
        // state/add/remove matters only for the two endpoints we actually use.
        if (defaultChanged) relevant = cfg_.inputDeviceId.empty();
        else relevant = !id.empty() && (id == cfg_.inputDeviceId || id == cfg_.outputDeviceId);
    }
    if (relevant) SetEvent(restartEvent_.get());
}

void EngineHost::healthCheck() noexcept {
    const uint64_t now = nowUs();
    updateGaming(now);
    if (!audioEnabled_) {                       // STOP_AUDIO: keep the process, drop the pipeline
        if (pipelineUp_) stopPipeline();
        state_ = EngineState::Stopped;
        setEffective(OutputStatus::StoppedByUser, "Audio stopped from the dashboard - press Start audio to resume");
        return;
    }
    if (!pipelineUp_) {
        if (now < nextRetryUs_) return;
        std::string err;
        stopPipeline();
        if (!startPipeline(err)) {
            stopPipeline();
            if (stereoPcmActive_) {
                // The lossless PCM 2.0 re-open failed outright: go back to the bitstream
                // format on the next attempt instead of retrying PCM forever.
                AVR_LOGW("lossless stereo: PCM 2.0 output could not be opened ({}) - back to the bitstream format", err);
                stereoPcmActive_ = false;
            }
            setError(err);
            setEffective(OutputStatus::Error, err.substr(0, 120));
            state_ = EngineState::Error;
            nextRetryUs_ = now + 2'000'000;   // retry every 2 s (device may be asleep / in use)
        } else if (!lastStartFellBack_) {
            // v1.1.0: clear any old error once a start succeeds as requested (v1.0.x kept
            // a stale "Exclusive/bitstream output failed" banner forever).
            std::lock_guard lk(errMutex_);
            lastError_.clear();
        }
        return;
    }

    if (capture_.failed || renderer_.failed) {
        const long hr = capture_.failed ? capture_.failHr.load() : renderer_.failHr.load();
        setError(std::format("{} stream failed: {} - restarting", capture_.failed ? "Input" : "Output",
                             hresultText(HRESULT(hr))));
        stopPipeline();
        nextRetryUs_ = now + 1'000'000;
        return;
    }

    // PcmDirect: switch between PCM 2.0 (stereo source) and the chosen bitstream format.
    // The debounce lives in core/OutputPolicy (StereoPcmSwitch) so the unit tests run the
    // exact same decision; the re-open itself is the normal stop/start path below.
    {
        const auto& st = core_.stats;
        const auto act = pcmSwitch_.update(st.outputRequest.load(), stereoPcmActive_, now);
        if (act != StereoPcmSwitch::Action::None) {
            const bool toPcm = act == StereoPcmSwitch::Action::SwitchToPcm;
            Codec requested;
            { std::lock_guard lk(cfgMutex_); requested = cfg_.codec; }
            AVR_LOGI("lossless stereo: re-opening the output as {} (content {}, {})",
                     toPcm ? "PCM 2.0" : codecName(requested), layoutShort(st.contentMask.load()),
                     pcmSwitchText(static_cast<PcmSwitchStatus>(st.pcmSwitchStatus.load()), st.pcmBlockMask.load(),
                                   st.pcmHoldSeconds.load(), requested));
            stereoPcmActive_ = toPcm;
            stopPipeline();
            nextRetryUs_ = 0;
            return;
        }
    }

    // Bitstream passthrough may need the renderer re-opened with another IEC sub-format.
    if (renderer_.bitstream()) {
        const auto req = static_cast<IecType>(core_.stats.requestedIec.load());
        const bool passthrough = core_.stats.passthrough.load();
        const bool mismatch = req != IecType::None && renderFamily(req) != renderFamily(renderer_.iecType());
        const bool overrideStale = passthroughIec_ != IecType::None && !passthrough;
        if (mismatch || overrideStale) {
            if (iecMismatchSinceUs_ == 0) iecMismatchSinceUs_ = now;
            const uint64_t hold = overrideStale ? 3'000'000 : 750'000;
            if (now - iecMismatchSinceUs_ > hold) {
                IecType configured;
                { std::lock_guard lk(cfgMutex_); configured = iecForCodec(cfg_.codec); }
                passthroughIec_ = (passthrough && renderFamily(req) != renderFamily(configured)) ? renderFamily(req)
                                                                                               : IecType::None;
                AVR_LOGI("bitstream input {} - reopening output as {}", iecTypeName(req),
                         iecTypeName(passthroughIec_ == IecType::None ? configured : passthroughIec_));
                stopPipeline();
                nextRetryUs_ = 0;
                iecMismatchSinceUs_ = 0;
            }
        } else {
            iecMismatchSinceUs_ = 0;
        }
    }
}

void EngineHost::run(HANDLE externalStop) {
    // WaitForMultipleObjects rejects duplicate handles, so build the list explicitly.
    HANDLE waits[3];
    DWORD count = 0;
    waits[count++] = restartEvent_.get();
    waits[count++] = stopEvent_.get();
    if (externalStop) waits[count++] = externalStop;
    uint64_t lastSave = nowUs();
    nextRetryUs_ = 0;
    healthCheck();   // initial start
    for (;;) {
        const DWORD w = WaitForMultipleObjects(count, waits, FALSE, 250);
        if (w >= WAIT_OBJECT_0 + 1 && w < WAIT_OBJECT_0 + count) break;   // stop / external stop
        if (w == WAIT_OBJECT_0) {
            Sleep(300);                              // debounce bursts of device notifications
            ResetEvent(restartEvent_.get());
            state_ = EngineState::Reconfiguring;
            passthroughIec_ = IecType::None;
            stopPipeline();
            nextRetryUs_ = 0;
        }
        healthCheck();
        const uint64_t now = nowUs();
        if (configDirty_.exchange(false) || now - lastSave > 60'000'000) {
            EngineConfig copy;
            { std::lock_guard lk(cfgMutex_); copy = cfg_; }
            if (!saveConfig(configFile_, copy)) AVR_LOGW("could not save {}", toUtf8(configFile_.wstring()));
            lastSave = now;
        }
    }
    AVR_LOGI("engine stopping");
    stopPipeline();
    EngineConfig copy;
    { std::lock_guard lk(cfgMutex_); copy = cfg_; }
    saveConfig(configFile_, copy);
}

// ===========================================================================
// Telemetry
// ===========================================================================
void EngineHost::telemetryLoop() noexcept {
    setThreadName(L"AVR Telemetry");
    while (WaitForSingleObject(telemetryStop_.get(), 33) == WAIT_TIMEOUT) publishTelemetry();
}

void EngineHost::publishTelemetry() noexcept {
    // CPU usage of the whole process, normalised to all logical processors.
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        const uint64_t cpu = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                             (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        const uint64_t wall = nowUs();
        if (lastWallUs_ != 0 && wall > lastWallUs_) {
            const double ncpu = double(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            const double pct = double(cpu - lastCpu100ns_) / 10.0 / double(wall - lastWallUs_) / ncpu * 100.0;
            cpuPercent_ = cpuPercent_ * 0.9f + float(pct) * 0.1f;
        }
        lastCpu100ns_ = cpu;
        lastWallUs_ = wall;
    }

    EngineConfig cfg;
    { std::lock_guard lk(cfgMutex_); cfg = cfg_; }
    std::string err, effReason, game;
    OutputStatus effStatus;
    {
        std::lock_guard lk(errMutex_);
        err = lastError_;
        effReason = effectiveReason_;
        effStatus = outputStatus_;
        game = gameName_;
    }
    const bool gaming = params_.gamingActive.load();

    std::unique_lock pl(pipelineMutex_, std::try_to_lock);
    const bool up = pl.owns_lock() && pipelineUp_.load();

    telemetry_.publish([&](ipc::TelemetryBlock& b) {
        auto st = state_.load();
        uint32_t flags = 0;
        if (cfg.nightMode) flags |= ipc::TF_NightMode;
        if (muteAll_) flags |= ipc::TF_MuteAll;
        if (opts_.serviceMode) flags |= ipc::TF_ServiceMode;
        if (speechAvailable_) flags |= ipc::TF_SpeechAvailable;
        if (cfg.stereoAs20) flags |= ipc::TF_StereoAs20;
        const auto test = static_cast<TestSignal>(params_.testSignal.load());
        if (test != TestSignal::Off) flags |= ipc::TF_TestActive;

        b.masterDb = cfg.masterDb;
        b.upmixMode = static_cast<uint32_t>(cfg.upmix);
        b.cpuPercent = cpuPercent_;
        b.testSignal = static_cast<uint32_t>(test);
        b.configRevision = configRevision_.load();
        b.requestedCodec = static_cast<uint32_t>(cfg.codec);
        b.requestedBitrate = cfg.codec == Codec::PCM ? 0 : normaliseBitrate(cfg.codec, cfg.bitrate, 6);
        b.requestedMode = static_cast<uint32_t>(cfg.outputMode);
        b.outputStatus = static_cast<uint32_t>(effStatus);
        copyFixed(b.effectiveReason, effReason);
        if (!audioEnabled_) flags |= ipc::TF_AudioStopped;
        if (stereoPcmActive_) flags |= ipc::TF_StereoPcmActive;
        for (size_t ch = 0; ch < kBusChannels; ++ch) {
            b.outGainDb[ch] = cfg.gainDb[ch];
            b.outMuted[ch] = cfg.mute[ch] ? 1 : 0;
        }
        copyFixed(b.lastError, err);

        // v3: settings echoed for the dashboard
        uint32_t flags2 = 0;
        if (cfg.dedicatedSub) flags2 |= ipc::TF2_DedicatedSub;
        if (gaming) flags2 |= ipc::TF2_GamingActive;
        if (cfg.bufferMs == 0) flags2 |= ipc::TF2_BufferAuto;
        if (cfg.gameLfeGen) flags2 |= ipc::TF2_GameLfeGen;
        const bool latch = params_.lfeInhibited.load();
        if (latch) flags2 |= ipc::TF2_LfeInhibited;
        b.lfeResetSeconds = cfg.lfeResetSeconds;
        if (cfg.eq.enabled) flags2 |= ipc::TF2_EqEnabled;
        if (cfg.lfeFold) flags2 |= ipc::TF2_LfeFold;
        if (cfg.expLfeGen) flags2 |= ipc::TF2_ExpLfeGen;
        if (cfg.expLfeFold) flags2 |= ipc::TF2_ExpLfeFold;
        expireAudition(nowUs());
        b.auditionChannel = params_.auditionChannel.load();
        b.eqHeadroomDb = 0.0f;
        b.eqActiveMask = 0;
        {
            const uint64_t last = params_.lastSoundUs.load(), now = nowUs();
            b.lfeSilenceSeconds = latch && now > last ? float(double(now - last) / 1e6) : 0.0f;
        }
        b.bassDb = cfg.bassDb;
        b.lfeGenMode = static_cast<uint32_t>(cfg.lfeGen);
        b.gamingMode = static_cast<uint32_t>(cfg.gaming);
        b.bufferMsSetting = cfg.bufferMs;
        copyFixed(b.gameName, gaming && cfg.gaming == GamingMode::Auto ? game : std::string());

        if (!up) {
            b.flags2 = flags2;
            b.procState = static_cast<uint32_t>(ProcessingState::Idle);
            copyFixed(b.processingText, !audioEnabled_ ? "Audio pipeline stopped" : "-");
            copyFixed(b.pcmSwitchText, "");
            b.emitChannels = 0;
            b.freqLowHz = b.freqHighHz = 0.0f;
            for (auto& v : b.emitPeakDb) v = kMinDb;
            for (auto& v : b.emitRmsDb) v = kMinDb;
            b.lfeFoldPeakDb = b.lfeFoldRmsDb = kMinDb;
            b.lfeStatus = static_cast<uint32_t>(LfeStatus::None);
            copyFixed(b.lfeStatusText, "-");
            b.engineState = static_cast<uint32_t>(st);
            b.flags = flags;
            for (auto& v : b.inPeakDb) v = kMinDb;
            for (auto& v : b.inRmsDb) v = kMinDb;
            for (auto& v : b.outPeakDb) v = kMinDb;
            for (auto& v : b.outRmsDb) v = kMinDb;
            copyFixed(b.inputDescription, !audioEnabled_          ? "Audio stopped"
                                          : st == EngineState::Error ? "Engine error"
                                                                     : "Starting...");
            copyFixed(b.outputDescription, !audioEnabled_ ? "Stopped" : "-");
            return;
        }

        const auto& s = core_.stats;
        const auto& cc = core_.config();
        const auto& inFmt = capture_.format();
        const bool active = s.inputActive.load();
        const bool passthrough = s.passthrough.load();
        if (active) flags |= ipc::TF_InputActive;
        if (passthrough) flags |= ipc::TF_Passthrough;
        if (s.bitstreamBlocked.load()) flags |= ipc::TF_BitstreamBlocked;
        if (st == EngineState::Running && !active) st = EngineState::Idle;
        b.engineState = static_cast<uint32_t>(st);
        b.flags = flags;

        b.inSampleRate = inFmt.rate;
        b.inChannels = inFmt.channels;
        b.inBitsPerSample = inFmt.validBits;
        b.inChannelMask = inFmt.mask;
        b.inStreamKind = s.streamKind.load();
        b.inContentMask = s.contentMask.load();

        const Codec eff = cc.effectiveCodec();
        b.outCodec = static_cast<uint32_t>(eff);
        b.outSampleRate = renderer_.sampleRate();
        b.outChannels = uint16_t(s.encoderChannels.load());
        b.outBitrate = cc.bitstream() ? normaliseBitrate(cc.codec, cc.bitrate, b.outChannels) : 0;
        const OutputShape shape{cc.bitstream(), cc.bitstream() ? int(b.outChannels) : int(cc.pcmChannels),
                                s.lfeChannelOut.load(), s.lfeFolded.load(), !cc.dedicatedSub, cfg.expLfeGen,
                                s.lfeFoldOptional.load()};
        if (!shape.lfeOut) flags2 |= ipc::TF2_NoLfeOutput;
        if (shape.lfeFolded) flags2 |= ipc::TF2_LfeFolded;
        if (s.chimeActive.load()) flags2 |= ipc::TF2_ChimePlaying;
        b.outMode = uint16_t(cc.outputMode);

        b.dspLoadPercent = s.dspLoad.load() * 100.0f;
        const double capMs = core_.captureFillFrames() * 1000.0 / std::max<uint32_t>(cc.inRate, 1);
        const double outMs = core_.outputFillFrames() * 1000.0 / kBusRate;
        b.captureBufferMs = float(capMs);
        b.outputBufferMs = float(outMs);

        // ---- per-stage latency (v1.2.0) -------------------------------------------------
        // A sample waits for its 1536-frame block to complete (16 ms on average) and then
        // for the queue in front of it. Measured over time the two add up to the smoothed
        // post-push fill, which the drift loop holds at the target; v1.1 added a full
        // 32 ms frame to the instantaneous fill and over-reported by ~16 ms.
        // Pipeline.LatencyMatchesReportedStages checks this model against an impulse.
        constexpr double kMsPerFrame = 1000.0 / kBusRate;
        const QueueSizing& qs = core_.sizing();
        b.latCaptureMs = float(capture_.latencyMs());
        b.latCaptureQueueMs = float(capMs);
        b.latResamplerMs = float(passthrough ? 0.0 : core_.resamplerDelayMs());
        b.latAssemblyMs = float(passthrough ? 0.0 : 0.5 * kBlockFrames * kMsPerFrame);
        b.latOutputQueueMs = float(passthrough ? outMs
                                               : std::max(0.0, (double(s.postPushFill.load()) - 0.5 * kBlockFrames) * kMsPerFrame));
        b.latEncoderMs = float(passthrough || !cc.bitstream() ? 0.0 : core_.encoderDelaySamples() * kMsPerFrame);
        b.latDeviceBufferMs = float(renderer_.bufferMs());
        b.latDeviceStreamMs = float(renderer_.streamLatencyMs());
        b.latMarginMs = float(qs.marginFrames * kMsPerFrame);
        b.latFloorMs = float(qs.floorFrames * kMsPerFrame);
        b.latRenderPeriodMs = float(cc.renderPeriodFrames * kMsPerFrame);
        b.latCapturePacketMs = float(cc.capturePacketFrames * kMsPerFrame);
        b.latencyMs = b.latCaptureMs + b.latCaptureQueueMs + b.latResamplerMs + b.latAssemblyMs + b.latOutputQueueMs +
                      b.latEncoderMs + b.latDeviceBufferMs + b.latDeviceStreamMs;

        // ---- analysis / live processing state -------------------------------------------
        b.freqLowHz = active ? core_.frequencyRange.lowHz() : 0.0f;
        b.freqHighHz = active ? core_.frequencyRange.highHz() : 0.0f;
        const auto ps = static_cast<ProcessingState>(s.procState.load());
        b.procState = static_cast<uint32_t>(ps);
        b.lfeGenGain = s.lfeGenGain.load();
        if (b.lfeGenGain > 0.5f) flags2 |= ipc::TF2_LfeGenActive;
        const bool gameOverride = s.lfeGameOverride.load();
        if (gameOverride) flags2 |= ipc::TF2_LfeGameOverride;
        const auto lst = static_cast<LfeStatus>(s.lfeStatus.load());
        b.lfeStatus = static_cast<uint32_t>(lst);
        copyFixed(b.lfeStatusText, passthrough ? std::string("Bitstream passthrough (untouched)")
                                               : lfeStatusText(lst, shape, latch, b.lfeSilenceSeconds,
                                                               cfg.lfeResetSeconds, gameOverride));
        if (cc.bitstream() && core_.encoderMainChannels() == 5) flags2 |= ipc::TF2_Encoder50;
        b.eqHeadroomDb = s.eqHeadroomDb.load();
        b.eqActiveMask = s.eqActiveMask.load();
        b.flags2 = flags2;
        copyFixed(b.processingText,
                  processingText(ps, s.contentMask.load(), shape, gaming));
        const auto pst = static_cast<PcmSwitchStatus>(s.pcmSwitchStatus.load());
        b.pcmSwitchStatus = static_cast<uint32_t>(pst);
        b.pcmBlockMask = s.pcmBlockMask.load();
        b.pcmHoldSeconds = s.pcmHoldSeconds.load();
        copyFixed(b.pcmSwitchText, pcmSwitchText(pst, b.pcmBlockMask, b.pcmHoldSeconds, cfg.codec));
        b.emitChannels = s.emitChannels.load();
        b.emitMask = s.emitMask.load();
        for (size_t ch = 0; ch < 8; ++ch) {
            const bool exists = ch < b.emitChannels;
            b.emitPeakDb[ch] = exists ? toDb(core_.emitMeters.peak(ch)) : kMinDb;
            b.emitRmsDb[ch] = exists ? toDb(core_.emitMeters.rms(ch)) : kMinDb;
        }
        b.lfeFoldPeakDb = shape.lfeFolded ? toDb(core_.lfeFoldMeter.peak(0)) : kMinDb;
        b.lfeFoldRmsDb = shape.lfeFolded ? toDb(core_.lfeFoldMeter.rms(0)) : kMinDb;
        b.driftPpm = s.driftPpm.load();
        b.correlation = s.correlation.load();
        b.dialogActivity = s.dialog.load();
        b.nightGainReductionDb = s.nightGrDb.load();
        b.limiterReductionDb = s.limiterGrDb.load();
        b.framesEncoded = s.blocks.load() * kBlockFrames;
        b.underruns = renderer_.underruns.load();
        b.overruns = s.outputOverruns.load() + capture_.ringOverflows.load();
        b.inputDrops = s.inputDrops.load();
        b.testChannel = core_.tests().currentChannel.load();
        b.testStep = core_.tests().currentStep.load();
        b.testFrequencyHz = core_.tests().currentFrequency.load();

        for (size_t ch = 0; ch < 8; ++ch) {
            const bool exists = ch < inFmt.channels;
            b.inPeakDb[ch] = exists ? toDb(core_.inputMeters.peak(ch)) : kMinDb;
            b.inRmsDb[ch] = exists ? toDb(core_.inputMeters.rms(ch)) : kMinDb;
        }
        for (size_t ch = 0; ch < kBusChannels; ++ch) {
            b.outPeakDb[ch] = toDb(core_.outputMeters.peak(ch));
            b.outRmsDb[ch] = toDb(core_.outputMeters.rms(ch));
        }

        // ---- human-readable stream inspector strings -------------------------------
        std::string in;
        const auto kind = static_cast<StreamKind>(b.inStreamKind);
        if (passthrough)
            in = std::format("{} bitstream @ {}", kind == StreamKind::BitstreamDTS ? "DTS"
                                                   : kind == StreamKind::BitstreamEAC3 ? "Dolby Digital Plus"
                                                                                       : "Dolby Digital",
                             rateText(inFmt.rate));
        else if (!active)
            in = "Idle (no audio)";
        else
            in = std::format("{} @ {}", ChannelMatrix::layoutName(b.inContentMask), rateText(inFmt.rate));
        copyFixed(b.inputDescription, in);

        std::string out;
        const char* modeTxt = cc.outputMode == OutputMode::Exclusive ? "Exclusive" : "Shared";
        if (passthrough && cc.bitstream())
            out = std::format("{} passthrough (bit-exact)", in.substr(0, in.find(" bitstream")));
        else if (cc.bitstream())
            out = std::format("{} {} @ {} {}", codecName(cc.codec), encodedLayoutName(b.outChannels, shape.lfeFolded),
                              bitrateText(b.outBitrate), rateText(kBusRate));
        else if (cc.stereoPcmOverride)
            out = std::format("PCM 2.0 @ 48 kHz - lossless stereo ({})", modeTxt);
        else
            out = std::format("PCM {} @ 48 kHz ({})", pcmLayoutName(cc.pcmChannels, shape.lfeFolded), modeTxt);
        copyFixed(b.outputDescription, out);
        copyFixed(b.inputDevice, capture_.deviceName() + (capture_.isLoopback() ? " (loopback)" : ""));
        copyFixed(b.outputDevice, renderer_.deviceName() + " - " + renderer_.formatText());
    });
}

// ===========================================================================
// Control commands (pipe thread)
// ===========================================================================
std::string EngineHost::stateDump() {
    EngineConfig c;
    { std::lock_guard lk(cfgMutex_); c = cfg_; }
    auto r = ipc::Response::ok();
    r.kv("input_device", c.inputDeviceId.empty() ? "default" : toUtf8(c.inputDeviceId));
    r.kv("output_device", toUtf8(c.outputDeviceId));
    r.kv("codec", ipc::codecToken(c.codec));
    r.kv("bitrate", std::to_string(c.bitrate));
    r.kv("output_mode", ipc::outputModeToken(c.outputMode));
    r.kv("buffer_ms", std::to_string(c.bufferMs));
    r.kv("upmix", ipc::upmixToken(c.upmix));
    r.kv("stereo_as_20", c.stereoAs20 ? "1" : "0");
    r.kv("night_mode", c.nightMode ? "1" : "0");
    r.kv("master_db", std::format("{:.1f}", c.masterDb));
    for (size_t i = 0; i < kBusChannels; ++i) {
        r.kv(std::format("gain{}", i), std::format("{:.1f}", c.gainDb[i]));
        r.kv(std::format("mute{}", i), c.mute[i] ? "1" : "0");
    }
    r.kv("centre_level", std::format("{:.2f}", c.centreLevel));
    r.kv("centre_floor", std::format("{:.2f}", c.centreFloor));
    r.kv("dialog_boost", std::format("{:.2f}", c.dialogBoost));
    r.kv("surround_level", std::format("{:.2f}", c.surroundLevel));
    r.kv("surround_delay_ms", std::format("{:.1f}", c.surroundDelayMs));
    r.kv("lfe_crossover_hz", std::format("{:.0f}", c.lfeCrossoverHz));
    r.kv("lfe_level_db", std::format("{:.1f}", c.lfeLevelDb));
    r.kv("mute_all", muteAll_ ? "1" : "0");
    r.kv("test", ipc::testToken(static_cast<TestSignal>(params_.testSignal.load())));
    r.kv("service_mode", opts_.serviceMode ? "1" : "0");
    r.kv("audio_enabled", audioEnabled_ ? "1" : "0");
    r.kv("bass_db", std::format("{:.1f}", c.bassDb));
    r.kv("dedicated_sub", c.dedicatedSub ? "1" : "0");
    r.kv("lfe_gen", ipc::lfeGenToken(c.lfeGen));
    r.kv("gaming", ipc::gamingToken(c.gaming));
    r.kv("gaming_active", params_.gamingActive.load() ? "1" : "0");
    r.kv("game_lfe_gen", c.gameLfeGen ? "1" : "0");
    r.kv("lfe_fold", c.lfeFold ? "1" : "0");
    r.kv("exp_lfe_gen", c.expLfeGen ? "1" : "0");
    r.kv("exp_lfe_fold", c.expLfeFold ? "1" : "0");
    r.kv("lfe_reset_s", std::format("{:.0f}", c.lfeResetSeconds));
    r.kv("eq_enabled", c.eq.enabled ? "1" : "0");
    for (size_t ch = 0; ch < kBusChannels; ++ch) {
        r.kv(std::format("eq{}_on", ch), c.eq.channelOn[ch] ? "1" : "0");
        for (size_t band = 0; band < kEqBands; ++band)
            r.kv(std::format("eq{}_b{}", ch, band), ipc::eqBandToken(c.eq.bands[ch][band]));
    }
    r.kv("audition", std::to_string(params_.auditionChannel.load()));
    return r.serialise();
}

std::string EngineHost::listDevices() {
    std::string err;
    auto devs = enumerateDevices(true, err);
    if (devs.empty() && !err.empty()) return ipc::Response::error(err).serialise();
    auto r = ipc::Response::ok(std::to_string(devs.size()));
    std::lock_guard lk(capsMutex_);
    for (auto& d : devs) {
        // The device we hold in exclusive mode cannot be probed; remember its last answer.
        // Bit 7 (kIecCapsKnown) tells the dashboard whether bits 0-2 are meaningful.
        if (d.render) {
            if (d.iecCaps & kIecCapsKnown) iecCapsCache_[d.id] = d.iecCaps;
            else if (auto it = iecCapsCache_.find(d.id); it != iecCapsCache_.end()) d.iecCaps = it->second;
        }
        std::string name = d.name;
        std::replace(name.begin(), name.end(), '\t', ' ');
        r.line(std::format("DEV\t{}\t{}\t{}\t{}\t{}\t{}\t{}", d.render ? "render" : "capture", toUtf8(d.id), name,
                           d.isDefault ? 1 : 0, d.channels, d.rate, d.iecCaps));
    }
    return r.serialise();
}

std::string EngineHost::handleCommand(const std::string& line) {
    using ipc::Cmd;
    using ipc::Response;
    const auto req = ipc::parseRequest(line);
    const auto& a = req.args;
    auto need = [&](size_t n) { return a.size() >= n; };
    auto bad = [&](std::string_view why) { return Response::error(std::format("{}: {}", req.verb, why)).serialise(); };
    auto ok = [] { return Response::ok().serialise(); };

    switch (req.cmd) {
        case Cmd::Ping: return Response::ok("pong").serialise();
        case Cmd::GetState: return stateDump();
        case Cmd::ListDevices: return listDevices();

        case Cmd::SetInputDevice:
        case Cmd::SetOutputDevice: {
            if (!need(1)) return bad("device id required");
            const std::wstring id = (a[0] == "default") ? std::wstring() : toWide(a[0]);
            {
                std::lock_guard lk(cfgMutex_);
                (req.cmd == Cmd::SetInputDevice ? cfg_.inputDeviceId : cfg_.outputDeviceId) = id;
            }
            markDirty();
            requestRestart("device changed");
            return ok();
        }
        case Cmd::SetCodec: {
            const auto c = need(1) ? ipc::parseCodec(a[0]) : std::nullopt;
            if (!c) return bad("AC3|DTS|EAC3|PCM");
            { std::lock_guard lk(cfgMutex_); cfg_.codec = *c; cfg_.bitrate = defaultBitrate(*c); }
            markDirty();
            requestRestart("codec changed");
            return ok();
        }
        case Cmd::SetBitrate: {
            const auto v = need(1) ? ipc::parseInt(a[0]) : std::nullopt;
            if (!v || *v <= 0) return bad("bits per second required");
            uint32_t applied;
            { std::lock_guard lk(cfgMutex_); applied = cfg_.bitrate = normaliseBitrate(cfg_.codec, uint32_t(*v), 6); }
            markDirty();
            requestRestart("bitrate changed");
            return Response::ok(std::to_string(applied)).serialise();
        }
        case Cmd::SetOutputMode: {
            const auto m = need(1) ? ipc::parseOutputMode(a[0]) : std::nullopt;
            if (!m) return bad("EXCLUSIVE|SHARED");
            { std::lock_guard lk(cfgMutex_); cfg_.outputMode = *m; }
            markDirty();
            requestRestart("output mode changed");
            return ok();
        }
        case Cmd::SetBufferMs: {
            const auto v = need(1) ? ipc::parseInt(a[0]) : std::nullopt;
            if (!v || (*v != 0 && (*v < 5 || *v > 200))) return bad("0 (automatic) or 5..200");
            { std::lock_guard lk(cfgMutex_); cfg_.bufferMs = uint32_t(*v); }
            markDirty();
            requestRestart("buffer changed");
            return ok();
        }
        case Cmd::SetUpmix: {
            const auto m = need(1) ? ipc::parseUpmixMode(a[0]) : std::nullopt;
            if (!m) return bad("PASSTHROUGH|UPMIX31|UPMIX51|PCM_DIRECT");
            { std::lock_guard lk(cfgMutex_); cfg_.upmix = *m; }
            params_.upmixMode = static_cast<uint32_t>(*m);
            markDirty();
            return ok();
        }
        case Cmd::SetUpmixParam: {
            const auto v = need(2) ? ipc::parseFloat(a[1]) : std::nullopt;
            if (!v) return bad("<name> <value>");
            std::string name(a[0]);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
            {
                std::lock_guard lk(cfgMutex_);
                if (name == "CENTRE") cfg_.centreLevel = std::clamp(*v, 0.0f, 1.0f);
                else if (name == "CENTRE_FLOOR") cfg_.centreFloor = std::clamp(*v, 0.0f, 1.0f);
                else if (name == "DIALOG") cfg_.dialogBoost = std::clamp(*v, 0.0f, 1.0f);
                else if (name == "SURROUND") cfg_.surroundLevel = std::clamp(*v, 0.0f, 1.0f);
                else if (name == "DELAY") cfg_.surroundDelayMs = std::clamp(*v, 0.0f, 25.0f);
                else if (name == "LFE_XOVER") cfg_.lfeCrossoverHz = std::clamp(*v, 40.0f, 200.0f);
                else if (name == "LFE_LEVEL") cfg_.lfeLevelDb = std::clamp(*v, -30.0f, 6.0f);
                else return bad("unknown parameter");
            }
            applyLiveParams();
            markDirty();
            return ok();
        }
        case Cmd::SetStereoAs20:
        case Cmd::SetNightMode: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            {
                std::lock_guard lk(cfgMutex_);
                (req.cmd == Cmd::SetNightMode ? cfg_.nightMode : cfg_.stereoAs20) = *b;
            }
            if (req.cmd == Cmd::SetNightMode) params_.nightMode = *b;
            else params_.stereoAsTwoChannel = *b;
            markDirty();
            return ok();
        }
        case Cmd::SetMaster: {
            const auto v = need(1) ? ipc::parseFloat(a[0]) : std::nullopt;
            if (!v) return bad("dB required");
            const float db = std::clamp(*v, -60.0f, 12.0f);
            { std::lock_guard lk(cfgMutex_); cfg_.masterDb = db; }
            params_.masterDb = db;
            markDirty();
            return ok();
        }
        case Cmd::SetGain: {
            const auto ch = need(2) ? ipc::parseInt(a[0]) : std::nullopt;
            const auto v = need(2) ? ipc::parseFloat(a[1]) : std::nullopt;
            if (!ch || !v || *ch < 0 || *ch >= static_cast<long long>(kBusChannels)) return bad("<0-5> <dB>");
            const float db = *v <= kGainMuteDb ? kGainMuteDb : std::clamp(*v, kGainMinDb, kGainMaxDb);
            { std::lock_guard lk(cfgMutex_); cfg_.gainDb[size_t(*ch)] = db; }
            params_.gainDb[size_t(*ch)] = db;
            markDirty();
            return ok();
        }
        case Cmd::SetMute: {
            const auto ch = need(2) ? ipc::parseInt(a[0]) : std::nullopt;
            const auto b = need(2) ? ipc::parseBool(a[1]) : std::nullopt;
            if (!ch || !b || *ch < 0 || *ch >= static_cast<long long>(kBusChannels)) return bad("<0-5> 0|1");
            { std::lock_guard lk(cfgMutex_); cfg_.mute[size_t(*ch)] = *b; }
            params_.mute[size_t(*ch)] = *b;
            markDirty();
            return ok();
        }
        case Cmd::SetMuteAll: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            muteAll_ = *b;
            params_.muteAll = *b;
            configRevision_.fetch_add(1);
            return ok();
        }
        case Cmd::Test: {
            const auto t = need(1) ? ipc::parseTestSignal(a[0]) : std::nullopt;
            if (!t) return bad("OFF|PINK|CHANNEL_ID|CENTER_BLEED|LFE_SWEEP|PHASE [mask]");
            uint32_t mask = 0x3F;
            if (a.size() >= 2)
                if (auto m = ipc::parseInt(a[1], 16)) mask = uint32_t(*m) & 0x3F;
            params_.testMask = mask;
            params_.testSignal = static_cast<uint32_t>(*t);
            configRevision_.fetch_add(1);
            return ok();
        }
        case Cmd::SetBass: {
            const auto v = need(1) ? ipc::parseFloat(a[0]) : std::nullopt;
            if (!v) return bad("dB required (-12..12)");
            const float db = std::clamp(*v, kBassMinDb, kBassMaxDb);
            { std::lock_guard lk(cfgMutex_); cfg_.bassDb = db; }
            params_.bassDb = db;
            markDirty();
            return ok();
        }
        case Cmd::SetSub: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            bool changed;
            {
                std::lock_guard lk(cfgMutex_);
                changed = cfg_.dedicatedSub != *b;
                cfg_.dedicatedSub = *b;
            }
            markDirty();
            // The encoder layout changes (5.1 <-> 5.0): re-open the output.
            if (changed) requestRestart(*b ? "dedicated subwoofer active" : "dedicated subwoofer inactive (no LFE channel)");
            return ok();
        }
        case Cmd::SetLfeGen: {
            const auto m = need(1) ? ipc::parseLfeGenMode(a[0]) : std::nullopt;
            if (!m) return bad("OFF|AUTO|ALWAYS");
            { std::lock_guard lk(cfgMutex_); cfg_.lfeGen = *m; }
            params_.lfeGenMode = static_cast<uint32_t>(*m);
            markDirty();
            return ok();
        }
        case Cmd::SetGaming: {
            const auto m = need(1) ? ipc::parseGamingMode(a[0]) : std::nullopt;
            if (!m) return bad("AUTO|ON|OFF");
            { std::lock_guard lk(cfgMutex_); cfg_.gaming = *m; }
            updateGaming(nowUs());
            markDirty();
            return ok();
        }
        case Cmd::SetGameLfe: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            { std::lock_guard lk(cfgMutex_); cfg_.gameLfeGen = *b; }
            params_.gameLfeGen = *b;
            markDirty();
            return ok();
        }
        case Cmd::SetExpLfeGen:    // v1.6.0 experimental gates: live, no restart (generation and fold ramp)
        case Cmd::SetExpLfeFold: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            {
                std::lock_guard lk(cfgMutex_);
                (req.cmd == Cmd::SetExpLfeGen ? cfg_.expLfeGen : cfg_.expLfeFold) = *b;
            }
            (req.cmd == Cmd::SetExpLfeGen ? params_.expLfeGen : params_.expLfeFold) = *b;
            markDirty();
            return ok();
        }
        case Cmd::PlayChime: {
            // v1.6.0: the chime is mixed into the encoder input, so it plays only while the
            // pipeline is processing PCM. Say why when it cannot, never pretend it played.
            if (!audioEnabled_) return bad("the audio pipeline is stopped");
            if (!pipelineUp_) return bad("the audio pipeline is not running");
            if (muteAll_) return bad("all speakers are muted");
            if (core_.stats.passthrough.load())
                return bad("pre-encoded audio is passed through bit-exact, so nothing can be mixed into it");
            params_.chimeRequest.fetch_add(1);
            return Response::ok("queued").serialise();
        }
        case Cmd::SetLfeFold: {   // v1.5.0: live, no restart (the fold ramps)
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            { std::lock_guard lk(cfgMutex_); cfg_.lfeFold = *b; }
            params_.lfeFold = *b;
            markDirty();
            return ok();
        }
        case Cmd::SetLfeReset: {
            const auto v = need(1) ? ipc::parseLfeResetSeconds(a[0]) : std::nullopt;
            if (!v) return bad("whole seconds from 1 to 300 required (default 10)");
            const float sec = *v;
            { std::lock_guard lk(cfgMutex_); cfg_.lfeResetSeconds = sec; }
            params_.lfeResetSeconds = sec;
            markDirty();
            return Response::ok(std::format("{:.0f}", sec)).serialise();
        }
        case Cmd::SetEq: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1");
            EqConfig eq;
            { std::lock_guard lk(cfgMutex_); cfg_.eq.enabled = *b; eq = cfg_.eq; }
            params_.setEq(eq);
            markDirty();
            return ok();
        }
        case Cmd::SetEqChannel: {
            const auto ch = need(2) ? ipc::parseInt(a[0]) : std::nullopt;
            const auto b = need(2) ? ipc::parseBool(a[1]) : std::nullopt;
            if (!ch || !b || *ch < 0 || *ch >= static_cast<long long>(kBusChannels)) return bad("<0-5> 0|1");
            EqConfig eq;
            { std::lock_guard lk(cfgMutex_); cfg_.eq.channelOn[size_t(*ch)] = *b; eq = cfg_.eq; }
            params_.setEq(eq);
            markDirty();
            return ok();
        }
        case Cmd::SetEqBand: {
            const auto ch = need(5) ? ipc::parseInt(a[0]) : std::nullopt;
            const auto band = need(5) ? ipc::parseInt(a[1]) : std::nullopt;
            const auto f = need(5) ? ipc::parseFloat(a[2]) : std::nullopt;
            const auto g = need(5) ? ipc::parseFloat(a[3]) : std::nullopt;
            const auto q = need(5) ? ipc::parseFloat(a[4]) : std::nullopt;
            if (!ch || !band || !f || !g || !q || *ch < 0 || *ch >= static_cast<long long>(kBusChannels) ||
                *band < 0 || *band >= static_cast<long long>(kEqBands))
                return bad("<ch 0-5> <band 0-4> <Hz> <dB> <Q>");
            const EqBand v = eqClampBand(size_t(*ch), size_t(*band), EqBand{*f, *g, *q});
            EqConfig eq;
            { std::lock_guard lk(cfgMutex_); cfg_.eq.bands[size_t(*ch)][size_t(*band)] = v; eq = cfg_.eq; }
            params_.setEq(eq);
            markDirty();
            return Response::ok(std::format("{:.1f} {:.2f} {:.2f}", v.freqHz, v.gainDb, v.q)).serialise();
        }
        case Cmd::ResetEqChannel: {
            const auto ch = need(1) ? ipc::parseInt(a[0]) : std::nullopt;
            if (!ch || *ch < 0 || *ch >= static_cast<long long>(kBusChannels)) return bad("<0-5>");
            EqConfig eq;
            {
                std::lock_guard lk(cfgMutex_);
                for (size_t band = 0; band < kEqBands; ++band) cfg_.eq.bands[size_t(*ch)][band] = eqDefaultBand(size_t(*ch), band);
                eq = cfg_.eq;
            }
            params_.setEq(eq);
            markDirty();
            return ok();
        }
        case Cmd::SetAudition: {
            if (!need(1)) return bad("<0-5>|OFF");
            // "0" is the front-left channel, so only OFF / -1 switch auditioning off.
            int32_t solo = -1;
            if (a[0] == "OFF" || a[0] == "off" || a[0] == "Off") solo = -1;
            else if (const auto ch = ipc::parseInt(a[0]); ch && *ch >= -1 && *ch < static_cast<long long>(kBusChannels))
                solo = int32_t(*ch);
            else return bad("<0-5>|OFF");
            auditionSeenUs_ = solo >= 0 ? nowUs() : 0;
            params_.auditionChannel = solo;
            return ok();   // temporary: no config change, no revision bump
        }
        case Cmd::GameDetected: {
            const auto b = need(1) ? ipc::parseBool(a[0]) : std::nullopt;
            if (!b) return bad("0|1 [name]");
            {
                std::lock_guard lk(errMutex_);
                gameName_ = *b && a.size() >= 2 ? a[1].substr(0, 60) : std::string();
            }
            gameSeenUs_ = *b ? nowUs() : 0;
            updateGaming(nowUs());
            return ok();   // heartbeat: no config change, no revision bump
        }
        case Cmd::StopAudio:
            audioEnabled_ = false;
            configRevision_.fetch_add(1);
            requestRestart("audio stopped by user");
            return ok();
        case Cmd::StartAudio:
            audioEnabled_ = true;
            configRevision_.fetch_add(1);
            requestRestart("audio started by user");
            return ok();
        case Cmd::Restart:
            requestRestart("user request");   // the supervisor also clears any passthrough override
            return ok();
        case Cmd::Shutdown:
            if (opts_.serviceMode) return bad("running as a service - stop it from Services or the installer script");
            requestStop();
            return Response::ok("bye").serialise();
        case Cmd::Unknown:
        default:
            break;
    }
    return Response::error("unknown command '" + req.verb + "'").serialise();
}

} // namespace avr::win
