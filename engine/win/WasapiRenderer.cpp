#include "WasapiRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avr::win {

uint16_t WasapiRenderer::queryMixChannels(const std::wstring& deviceId) {
    std::string err;
    auto dev = openDevice(deviceId, eRender, err);
    if (!dev) return 2;
    ComPtr<IAudioClient> c;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c))) return 2;
    WAVEFORMATEX* mix = nullptr;
    uint16_t ch = 2;
    if (SUCCEEDED(c->GetMixFormat(&mix)) && mix) { ch = mix->nChannels; CoTaskMemFree(mix); }
    return ch;
}

bool WasapiRenderer::initClient(const WAVEFORMATEX* fmt, std::string& error) {
    client_.Reset();
    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) { error = "render Activate: " + hresultText(hr); return false; }

    if (mode_ == OutputMode::Shared) {
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, REFERENCE_TIME(bufferMs_) * 10'000, 0, fmt, nullptr);
        if (FAILED(hr)) { error = "shared Initialize: " + hresultText(hr); return false; }
        return true;
    }

    // Exclusive, event driven: buffer == period. Use the larger of the device minimum
    // period and the user's buffer setting (smaller = lower latency, more risk).
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client_->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME period = std::max<REFERENCE_TIME>(minPeriod, REFERENCE_TIME(bufferMs_) * 10'000 / 2);
    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, fmt, nullptr);
    // HDMI drivers can take a moment to release a just-closed exclusive stream (and
    // the AVR re-handshakes). Retry instead of failing over to shared mode.
    for (int attempt = 0; hr == AUDCLNT_E_DEVICE_IN_USE && attempt < 8; ++attempt) {
        Sleep(250);
        client_.Reset();
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr)) { error = "render re-Activate: " + hresultText(hr); return false; }
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, fmt, nullptr);
    }
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Documented dance: query the aligned size, re-activate, re-initialise.
        UINT32 frames = 0;
        client_->GetBufferSize(&frames);
        period = REFERENCE_TIME(10'000'000.0 * frames / fmt->nSamplesPerSec + 0.5);
        client_.Reset();
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr)) { error = "render re-Activate: " + hresultText(hr); return false; }
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, fmt, nullptr);
    }
    if (FAILED(hr)) { error = "exclusive Initialize: " + hresultText(hr); return false; }
    return true;
}

bool WasapiRenderer::open(const std::wstring& deviceId, const RenderRequest& req, std::string& error) {
    stop();
    failed = false;
    underruns = 0;
    mode_ = req.mode;
    iec_ = req.mode == OutputMode::Exclusive ? req.iec : IecType::None;
    bufferMs_ = std::clamp<uint32_t>(req.bufferMs, 4, 200);

    device_ = openDevice(deviceId, eRender, error);
    if (!device_) return false;
    name_ = win::deviceName(device_.Get());

    bool ok = false;
    std::string lastErr;
    if (iec_ != IecType::None) {
        // ---- bitstream -----------------------------------------------------------------
        for (int variant = 0; variant < 3 && !ok; ++variant) {
            IecWaveFormat fmt;
            if (!buildIecFormat(iec_, variant, fmt)) continue;
            const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(&fmt);
            if (initClient(wf, lastErr)) {
                ok = true;
                rate_ = wf->nSamplesPerSec;
                channels_ = 2;
                fmtText_ = std::string("IEC 61937 ") + iecTypeName(iec_) +
                           (variant == 0 ? " (EXTENSIBLE_IEC61937)" : variant == 1 ? " (EXTENSIBLE)" : " (AC3_SPDIF)");
            }
        }
        if (!ok) {
            error = std::string("device rejected ") + iecTypeName(iec_) + " bitstream: " + lastErr +
                    ". Check Sound > device > Supported Formats, and that the TV/AVR advertises this codec over ARC.";
            return false;
        }
        kind_ = SampleKind::Bytes;
        deviceFrameBytes_ = 4;
        ringFrameBytes_ = 4;
    } else {
        // ---- PCM -------------------------------------------------------------------------
        channels_ = req.pcmChannels;
        rate_ = 48000;
        ringFrameBytes_ = uint32_t(channels_) * 4;   // ring always carries float32
        struct Cand { uint16_t bits; bool flt; SampleKind kind; };
        const Cand shared[] = {{32, true, SampleKind::Float32}};
        const Cand excl[] = {{32, true, SampleKind::Float32}, {32, false, SampleKind::Int32}, {16, false, SampleKind::Int16}};
        const Cand* list = mode_ == OutputMode::Shared ? shared : excl;
        const int n = mode_ == OutputMode::Shared ? 1 : 3;
        for (int i = 0; i < n && !ok; ++i) {
            const auto fmt = buildPcmFormat(rate_, channels_, list[i].bits, list[i].flt);
            if (initClient(&fmt.Format, lastErr)) {
                ok = true;
                kind_ = list[i].kind;
                deviceFrameBytes_ = fmt.Format.nBlockAlign;
                fmtText_ = describeFormat(&fmt.Format);
            }
        }
        if (!ok) { error = "device rejected PCM output: " + lastErr; return false; }
    }

    client_->GetBufferSize(&bufferFrames_);
    REFERENCE_TIME lat = 0;
    client_->GetStreamLatency(&lat);
    streamLatencyMs_ = double(lat) / 10'000.0;
    bufferMsActual_ = 1000.0 * bufferFrames_ / rate_;
    latencyMs_ = streamLatencyMs_ + bufferMsActual_;
    // E-AC-3 transport runs at 4x; express the period in 48 kHz bus frames.
    periodBusFrames_ = uint32_t(uint64_t(bufferFrames_) * kBusRate / rate_);

    audioEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    HRESULT hr = client_->SetEventHandle(audioEvent_.get());
    if (FAILED(hr)) { error = "render SetEventHandle: " + hresultText(hr); return false; }
    hr = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) { error = "IAudioRenderClient: " + hresultText(hr); return false; }

    convert_.assign(size_t(bufferFrames_) * ringFrameBytes_ + 64, 0);
    return true;
}

bool WasapiRenderer::start(SpscRing<uint8_t>* ring, HANDLE dspWakeEvent, std::string& error) {
    if (!client_) { error = "renderer not opened"; return false; }
    ring_ = ring;
    dspWake_ = dspWakeEvent;

    // Pre-roll one buffer of silence (zero words are a valid IEC 61937 gap).
    BYTE* data = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &data)))
        render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);

    ResetEvent(stopEvent_.get());
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) { error = "render Start: " + hresultText(hr); return false; }
    thread_ = std::thread([this] { run(); });
    return true;
}

void WasapiRenderer::stop() noexcept {
    if (thread_.joinable()) {
        SetEvent(stopEvent_.get());
        thread_.join();
    }
    if (client_) client_->Stop();
    // v1.1.0 FIX: release EVERY interface that references the exclusive stream.
    // v1.0.x kept the IAudioRenderClient alive until the next open() had already
    // called Initialize(), so the HDMI/S-PDIF device was still held in exclusive
    // mode and the re-open failed (the "switch output away and back" bug after
    // any codec/bitrate/buffer change or engine restart).
    render_.Reset();
    client_.Reset();
    device_.Reset();
}

UINT32 WasapiRenderer::fill(BYTE* dst, UINT32 frames) noexcept {
    const size_t wantBytes = size_t(frames) * ringFrameBytes_;
    // Whole frames only (never split an IEC word pair / PCM frame).
    const size_t availFrames = ring_->readAvailable() / ringFrameBytes_;
    const UINT32 got = UINT32(std::min<size_t>(availFrames, frames));

    if (kind_ == SampleKind::Bytes || kind_ == SampleKind::Float32) {
        ring_->read(dst, size_t(got) * ringFrameBytes_);
    } else {
        ring_->read(convert_.data(), size_t(got) * ringFrameBytes_);
        const auto* f = reinterpret_cast<const float*>(convert_.data());
        const size_t samples = size_t(got) * channels_;
        if (kind_ == SampleKind::Int32) {
            auto* o = reinterpret_cast<int32_t*>(dst);
            for (size_t i = 0; i < samples; ++i)
                o[i] = int32_t(std::clamp(double(f[i]), -1.0, 0.99999994) * 2147483648.0);
        } else {
            auto* o = reinterpret_cast<int16_t*>(dst);
            for (size_t i = 0; i < samples; ++i)
                o[i] = int16_t(std::lrint(std::clamp(f[i], -1.0f, 0.99997f) * 32768.0f));
        }
    }
    if (got < frames) {
        std::memset(dst + size_t(got) * deviceFrameBytes_, 0, size_t(frames - got) * deviceFrameBytes_);
        underruns.fetch_add(1, std::memory_order_relaxed);
    }
    (void)wantBytes;
    return got;
}

void WasapiRenderer::run() noexcept {
    ComScope com;
    setThreadName(L"AVR Render");
    MmcssScope mmcss(L"Pro Audio", AVRT_PRIORITY_CRITICAL);
    enableFlushToZero();
    HANDLE waits[2] = {stopEvent_.get(), audioEvent_.get()};
    const DWORD timeout = std::max<DWORD>(DWORD(2000.0 * bufferFrames_ / rate_), 20);
    const bool exclusive = mode_ == OutputMode::Exclusive;

    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) continue;   // device stalled (e.g. HDMI re-handshake); keep waiting

        UINT32 frames = bufferFrames_;
        if (!exclusive) {
            UINT32 padding = 0;
            HRESULT hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) { failHr = hr; failed = true; break; }
            frames = bufferFrames_ - padding;
            if (frames == 0) continue;
        }
        BYTE* data = nullptr;
        HRESULT hr = render_->GetBuffer(frames, &data);
        if (FAILED(hr)) { failHr = hr; failed = true; break; }
        fill(data, frames);
        hr = render_->ReleaseBuffer(frames, 0);
        if (FAILED(hr)) { failHr = hr; failed = true; break; }
        SetEvent(dspWake_);   // the DSP thread tops the ring back up
    }
}

} // namespace avr::win
