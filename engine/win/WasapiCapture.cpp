#include "WasapiCapture.h"

#include <algorithm>
#include <cstring>

namespace avr::win {

bool WasapiCapture::open(const std::wstring& deviceId, uint32_t bufferMs, std::string& error) {
    stop();
    client_.Reset();
    capture_.Reset();
    failed = false;

    // A render endpoint (e.g. "CABLE Input") is captured in loopback; a capture endpoint
    // (e.g. "CABLE Output") is captured directly. Default = default render endpoint.
    device_ = openDevice(deviceId, eRender, error);
    if (!device_) return false;
    loopback_ = isRenderEndpoint(device_.Get());
    name_ = win::deviceName(device_.Get());
    id_ = win::deviceId(device_.Get());

    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) { error = "capture Activate: " + hresultText(hr); return false; }

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { error = "GetMixFormat: " + hresultText(hr); return false; }
    fmt_ = parseFormat(mix);
    fmtText_ = describeFormat(mix);

    if (!fmt_.isFloat && !fmt_.isPcm) {
        CoTaskMemFree(mix);
        error = "unsupported capture mix format: " + fmtText_;
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    const REFERENCE_TIME dur = REFERENCE_TIME(std::clamp<uint32_t>(bufferMs, 10, 200)) * 10'000;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { error = "capture Initialize: " + hresultText(hr); return false; }

    client_->GetBufferSize(&bufferFrames_);
    REFERENCE_TIME lat = 0, defPeriod = 0, minPeriod = 0;
    client_->GetStreamLatency(&lat);
    client_->GetDevicePeriod(&defPeriod, &minPeriod);
    // v1.2.0: a sample waits on average half a packet (engine period) before the packet
    // is delivered, plus the stream latency WASAPI reports. v1.1 counted a whole period.
    periodMs_ = double(defPeriod) / 10'000.0;
    if (periodMs_ <= 0.0) periodMs_ = 10.0;
    latencyMs_ = double(lat) / 10'000.0 + 0.5 * periodMs_;
    pollMs_ = DWORD(std::max<REFERENCE_TIME>(defPeriod / 10'000, 2));

    audioEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    hr = client_->SetEventHandle(audioEvent_.get());
    if (FAILED(hr)) { error = "capture SetEventHandle: " + hresultText(hr); return false; }

    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) { error = "IAudioCaptureClient: " + hresultText(hr); return false; }

    scratch_.assign(size_t(bufferFrames_ + 4096) * fmt_.channels, 0.0f);
    return true;
}

bool WasapiCapture::start(SpscRing<float>* ring, HANDLE dspWakeEvent, std::string& error) {
    if (!client_) { error = "capture not opened"; return false; }
    ring_ = ring;
    dspWake_ = dspWakeEvent;
    ResetEvent(stopEvent_.get());
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) { error = "capture Start: " + hresultText(hr); return false; }
    thread_ = std::thread([this] { run(); });
    return true;
}

void WasapiCapture::stop() noexcept {
    if (thread_.joinable()) {
        SetEvent(stopEvent_.get());
        thread_.join();
    }
    if (client_) client_->Stop();
    capture_.Reset();   // release the stream fully so a restart starts clean
    client_.Reset();
    device_.Reset();
}

void WasapiCapture::convert(const BYTE* src, UINT32 frames, bool silent) noexcept {
    const size_t samples = size_t(frames) * fmt_.channels;
    float* dst = scratch_.data();
    if (silent || !src) {
        std::fill(dst, dst + samples, 0.0f);
        return;
    }
    if (fmt_.isFloat && fmt_.bits == 32) {
        std::memcpy(dst, src, samples * sizeof(float));
    } else if (fmt_.isPcm && fmt_.bits == 16) {
        const auto* s = reinterpret_cast<const int16_t*>(src);
        for (size_t i = 0; i < samples; ++i) dst[i] = float(s[i]) * (1.0f / 32768.0f);
    } else if (fmt_.isPcm && fmt_.bits == 32) {
        const auto* s = reinterpret_cast<const int32_t*>(src);   // 24-in-32 is left-justified
        for (size_t i = 0; i < samples; ++i) dst[i] = float(s[i]) * (1.0f / 2147483648.0f);
    } else if (fmt_.isPcm && fmt_.bits == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const BYTE* p = src + i * 3;
            const int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24);
            dst[i] = float(v) * (1.0f / 2147483648.0f);
        }
    } else {
        std::fill(dst, dst + samples, 0.0f);
    }
}

void WasapiCapture::run() noexcept {
    ComScope com;
    setThreadName(L"AVR Capture");
    MmcssScope mmcss(L"Pro Audio", AVRT_PRIORITY_HIGH);
    HANDLE waits[2] = {stopEvent_.get(), audioEvent_.get()};

    for (;;) {
        // Event-driven loopback is reliable on Windows 10 1703+; the timeout keeps us
        // polling anyway so a missed event can never stall capture.
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, pollMs_ * 2);
        if (w == WAIT_OBJECT_0) break;

        bool pushed = false;
        for (;;) {
            UINT32 packet = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet);
            if (FAILED(hr)) { failHr = hr; failed = true; break; }
            if (packet == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) { failHr = hr; failed = true; break; }
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuities.fetch_add(1, std::memory_order_relaxed);

            UINT32 done = 0;
            while (done < frames) {   // scratch is sized for >= one buffer, loop for safety
                const UINT32 chunk = std::min<UINT32>(frames - done, UINT32(scratch_.size() / fmt_.channels));
                convert(data ? data + size_t(done) * fmt_.blockAlign : nullptr, chunk,
                        (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                const size_t samples = size_t(chunk) * fmt_.channels;
                if (ring_->write(scratch_.data(), samples) != samples)
                    ringOverflows.fetch_add(1, std::memory_order_relaxed);   // DSP thread stalled
                done += chunk;
            }
            capture_->ReleaseBuffer(frames);
            pushed = true;
        }
        if (failed.load(std::memory_order_relaxed)) break;   // supervisor restarts the pipeline
        if (pushed) {
            lastPacketUs.store(nowUs(), std::memory_order_relaxed);
            SetEvent(dspWake_);
        }
    }
}

} // namespace avr::win
