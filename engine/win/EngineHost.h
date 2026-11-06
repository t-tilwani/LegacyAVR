// LegacyAVR - EngineHost: owns the whole engine and supervises it.
//
// Threads
//   supervisor (caller of run())   : start/stop/restart pipeline, health checks,
//                                    device-change handling, config persistence
//   "AVR Capture"   MMCSS Pro Audio : WASAPI loopback -> capture ring
//   "AVR DSP"       MMCSS Pro Audio : ProcessingCore::step (matrix/upmix/encode)
//   "AVR Render"    MMCSS Pro Audio : output ring -> WASAPI exclusive/shared (critical prio)
//   "AVR Telemetry" normal          : 30 Hz shared-memory publish
//   "AVR Control"   normal          : named-pipe commands
#pragma once

#include "ConfigStore.h"
#include "PipeServer.h"
#include "TelemetryWriter.h"
#include "WasapiCapture.h"
#include "WasapiRenderer.h"
#include "WinUtil.h"

#include "../core/LiveParams.h"
#include "../core/OutputPolicy.h"
#include "../core/ProcessingCore.h"
#include "../core/SpscRing.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace avr::win {

struct HostOptions {
    bool serviceMode = false;
    bool console = true;
    std::filesystem::path dataDir;   // empty => default per mode
};

class DeviceWatcher;

class EngineHost {
public:
    explicit EngineHost(HostOptions opts);
    ~EngineHost();
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    bool initialise(std::string& error);
    // Blocks until externalStop is signalled, requestStop() or a SHUTDOWN command.
    void run(HANDLE externalStop);
    void requestStop() noexcept;
    void requestRestart(const char* reason) noexcept;

    // Called by DeviceWatcher (COM thread): cheap, non-blocking.
    void onDeviceTopologyChanged(const std::wstring& deviceId, bool defaultChanged) noexcept;

private:
    bool startPipeline(std::string& error);
    void stopPipeline() noexcept;
    void dspLoop() noexcept;
    void telemetryLoop() noexcept;
    void publishTelemetry() noexcept;
    void applyLiveParams() noexcept;
    void healthCheck() noexcept;
    void updateGaming(uint64_t nowUs) noexcept;
    void expireAudition(uint64_t nowUs) noexcept;   // v1.4.0
    void setError(std::string msg);
    void setEffective(OutputStatus status, std::string reason);
    void markDirty() noexcept;

    std::string handleCommand(const std::string& line);
    std::string stateDump();
    std::string listDevices();

    HostOptions opts_;
    std::filesystem::path configFile_;

    std::mutex cfgMutex_;
    EngineConfig cfg_;
    std::atomic<uint32_t> configRevision_{1};
    std::atomic<bool> configDirty_{false};

    LiveParams params_;
    SpscRing<float> captureRing_;
    SpscRing<uint8_t> outputRing_;
    ProcessingCore core_;
    WasapiCapture capture_;
    WasapiRenderer renderer_;
    TelemetryWriter telemetry_;
    PipeServer pipe_;
    DeviceWatcher* watcher_ = nullptr;

    std::mutex pipelineMutex_;   // supervisor holds it while (re)building the pipeline
    std::thread dspThread_, telemetryThread_;
    Handle stopEvent_, restartEvent_, dspWake_, dspStop_, telemetryStop_, instanceMutex_;
    std::atomic<EngineState> state_{EngineState::Stopped};
    std::atomic<bool> pipelineUp_{false};
    std::atomic<bool> muteAll_{false};
    std::atomic<bool> speechAvailable_{false};
    std::array<std::vector<float>, 6> speech_;

    IecType passthroughIec_ = IecType::None;   // renderer override while bitstream input is forwarded
    std::atomic<bool> audioEnabled_{true};     // STOP_AUDIO / START_AUDIO
    std::atomic<bool> stereoPcmActive_{false}; // PcmDirect: pipeline currently opened as PCM 2.0
    StereoPcmSwitch pcmSwitch_;                 // v1.2.0: shared with the unit tests (core/OutputPolicy)
    bool lastStartFellBack_ = false;
    std::atomic<uint64_t> gameSeenUs_{0};       // last GAME_DETECTED 1 heartbeat (Auto mode)
    std::atomic<uint64_t> auditionSeenUs_{0};   // v1.4.0: last SET_AUDITION <ch> refresh
    std::string gameName_;                      // guarded by errMutex_
    OutputStatus outputStatus_ = OutputStatus::Match;   // guarded by errMutex_
    std::string effectiveReason_;                        // guarded by errMutex_
    uint64_t iecMismatchSinceUs_ = 0;
    uint64_t nextRetryUs_ = 0;
    uint32_t pcmChannels_ = 2;

    std::mutex errMutex_;
    std::string lastError_;

    std::mutex capsMutex_;
    std::map<std::wstring, uint32_t> iecCapsCache_;

    // CPU accounting (telemetry thread only)
    uint64_t lastCpu100ns_ = 0, lastWallUs_ = 0;
    float cpuPercent_ = 0.0f;
};

} // namespace avr::win
