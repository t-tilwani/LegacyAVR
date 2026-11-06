// LegacyAVR - WASAPI capture: loopback of a render endpoint (the 7.1 virtual cable)
// or direct capture of a recording endpoint, shared mode, event driven.
//
// The capture thread converts any endpoint sample format to interleaved float and
// pushes it into the SPSC capture ring, then signals the DSP thread. No allocation,
// no locks, MMCSS "Pro Audio".
#pragma once

#include "WasapiDevices.h"
#include "WinUtil.h"

#include "../core/SpscRing.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace avr::win {

class WasapiCapture {
public:
    WasapiCapture() = default;
    ~WasapiCapture() { stop(); }

    // Not real-time. Activates the client and negotiates the endpoint mix format.
    bool open(const std::wstring& deviceId, uint32_t bufferMs, std::string& error);
    bool start(SpscRing<float>* ring, HANDLE dspWakeEvent, std::string& error);
    void stop() noexcept;

    [[nodiscard]] const PcmFormatInfo& format() const noexcept { return fmt_; }
    [[nodiscard]] bool isLoopback() const noexcept { return loopback_; }
    [[nodiscard]] const std::string& deviceName() const noexcept { return name_; }
    [[nodiscard]] const std::wstring& resolvedId() const noexcept { return id_; }
    [[nodiscard]] double latencyMs() const noexcept { return latencyMs_; }
    // Capture packet duration (shared-mode engine period, normally 10 ms).
    [[nodiscard]] double periodMs() const noexcept { return periodMs_; }
    [[nodiscard]] std::string formatText() const { return fmtText_; }

    // Real-time status, read by the supervisor.
    std::atomic<bool> failed{false};
    std::atomic<long> failHr{0};
    std::atomic<uint32_t> discontinuities{0};
    std::atomic<uint32_t> ringOverflows{0};
    std::atomic<uint64_t> lastPacketUs{0};

private:
    void run() noexcept;
    void convert(const BYTE* src, UINT32 frames, bool silent) noexcept;

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    Handle audioEvent_;
    Handle stopEvent_;
    std::thread thread_;
    SpscRing<float>* ring_ = nullptr;
    HANDLE dspWake_ = nullptr;

    PcmFormatInfo fmt_{};
    std::string fmtText_;
    std::string name_;
    std::wstring id_;
    bool loopback_ = true;
    UINT32 bufferFrames_ = 0;
    double latencyMs_ = 0.0;
    double periodMs_ = 10.0;
    DWORD pollMs_ = 10;
    std::vector<float> scratch_;
};

} // namespace avr::win
