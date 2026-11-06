// LegacyAVR - WASAPI renderer for the physical HDMI ARC / S/PDIF endpoint.
//
//  Exclusive + bitstream : IEC 61937 (AC-3 / DTS @ 48 kHz, E-AC-3 @ 192 kHz) copied
//                          verbatim from the output ring. Format negotiation tries
//                          WAVEFORMATEXTENSIBLE_IEC61937, WAVEFORMATEXTENSIBLE and
//                          WAVE_FORMAT_DOLBY_AC3_SPDIF in that order.
//  Exclusive + PCM       : float32 / int32(24) / int16 at 48 kHz, 2/6/8 channels.
//  Shared  (compat mode) : float32 48 kHz with AUTOCONVERTPCM so the Windows mixer
//                          can resample to whatever the device runs at.
//
// The render thread is the deadline-critical thread: it only copies bytes out of the
// SPSC ring (and converts float->int for exclusive PCM). It never encodes.
#pragma once

#include "WasapiDevices.h"
#include "WinUtil.h"

#include "../core/Iec61937.h"
#include "../core/SpscRing.h"
#include "../core/Types.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace avr::win {

struct RenderRequest {
    OutputMode mode = OutputMode::Exclusive;
    IecType iec = IecType::None;      // None => PCM
    uint16_t pcmChannels = 2;         // PCM only (0 = pick from device)
    uint32_t bufferMs = 20;
};

class WasapiRenderer {
public:
    WasapiRenderer() = default;
    ~WasapiRenderer() { stop(); }

    bool open(const std::wstring& deviceId, const RenderRequest& req, std::string& error);
    bool start(SpscRing<uint8_t>* ring, HANDLE dspWakeEvent, std::string& error);
    void stop() noexcept;

    // Channel count the device mix format offers (used to pick PCM channels).
    static uint16_t queryMixChannels(const std::wstring& deviceId);

    [[nodiscard]] bool bitstream() const noexcept { return iec_ != IecType::None; }
    [[nodiscard]] IecType iecType() const noexcept { return iec_; }
    [[nodiscard]] uint16_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t sampleRate() const noexcept { return rate_; }
    // Renderer period expressed in 48 kHz bus frames (for the drift target).
    [[nodiscard]] uint32_t periodBusFrames() const noexcept { return periodBusFrames_; }
    [[nodiscard]] double latencyMs() const noexcept { return latencyMs_; }
    [[nodiscard]] double bufferMs() const noexcept { return bufferMsActual_; }        // device buffer
    [[nodiscard]] double streamLatencyMs() const noexcept { return streamLatencyMs_; } // driver / FIFO
    [[nodiscard]] const std::string& deviceName() const noexcept { return name_; }
    [[nodiscard]] const std::string& formatText() const noexcept { return fmtText_; }
    [[nodiscard]] OutputMode mode() const noexcept { return mode_; }

    std::atomic<bool> failed{false};
    std::atomic<long> failHr{0};
    std::atomic<uint32_t> underruns{0};

private:
    enum class SampleKind { Bytes, Float32, Int32, Int16 };

    bool initClient(const WAVEFORMATEX* fmt, std::string& error);
    void run() noexcept;
    UINT32 fill(BYTE* dst, UINT32 frames) noexcept;

    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    Handle audioEvent_, stopEvent_;
    std::thread thread_;
    SpscRing<uint8_t>* ring_ = nullptr;
    HANDLE dspWake_ = nullptr;

    OutputMode mode_ = OutputMode::Exclusive;
    IecType iec_ = IecType::None;
    SampleKind kind_ = SampleKind::Bytes;
    uint16_t channels_ = 2;
    uint32_t rate_ = 48000;
    UINT32 bufferFrames_ = 0;
    uint32_t deviceFrameBytes_ = 4;   // bytes per frame on the device
    uint32_t ringFrameBytes_ = 4;     // bytes per frame in the ring
    uint32_t periodBusFrames_ = 480;
    uint32_t bufferMs_ = 20;
    double latencyMs_ = 0;
    double bufferMsActual_ = 0;
    double streamLatencyMs_ = 0;
    std::string name_, fmtText_;
    std::vector<uint8_t> convert_;    // ring -> device conversion scratch
};

} // namespace avr::win
