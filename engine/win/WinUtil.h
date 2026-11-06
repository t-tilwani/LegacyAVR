// LegacyAVR - small Win32 helpers shared by the Windows layer.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / CoTaskMemFree (excluded by WIN32_LEAN_AND_MEAN)

#include <avrt.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace avr::win {

using Microsoft::WRL::ComPtr;

std::string toUtf8(std::wstring_view w);
std::wstring toWide(std::string_view s);
std::string hresultText(HRESULT hr);           // "0x88890008 AUDCLNT_E_UNSUPPORTED_FORMAT ..."

// Monotonic microseconds from QueryPerformanceCounter.
uint64_t nowUs() noexcept;
uint64_t qpcNow() noexcept;

// Flush-to-zero / denormals-are-zero for the calling thread (DSP + render threads).
void enableFlushToZero() noexcept;

// RAII HANDLE
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : h_(h) {}
    ~Handle() { reset(); }
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept { if (this != &o) { reset(); h_ = o.release(); } return *this; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE h = nullptr) noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
    HANDLE release() noexcept { HANDLE h = h_; h_ = nullptr; return h; }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] explicit operator bool() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// RAII COM apartment for a thread.
class ComScope {
public:
    explicit ComScope(DWORD model = COINIT_MULTITHREADED) noexcept { hr_ = CoInitializeEx(nullptr, model); }
    ~ComScope() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// RAII MMCSS registration ("Pro Audio" task, configurable priority).
class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* task = L"Pro Audio", AVRT_PRIORITY prio = AVRT_PRIORITY_HIGH) noexcept {
        DWORD index = 0;
        h_ = AvSetMmThreadCharacteristicsW(task, &index);
        if (h_) AvSetMmThreadPriority(h_, prio);
    }
    ~MmcssScope() { if (h_) AvRevertMmThreadCharacteristics(h_); }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;
    [[nodiscard]] bool registered() const noexcept { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

void setThreadName(const wchar_t* name) noexcept;

// Security descriptor allowing SYSTEM, Administrators and interactive users (service mode IPC).
// Returns nullptr in per-user mode (default DACL). Caller owns via LocalFree.
PSECURITY_DESCRIPTOR createIpcSecurityDescriptor(bool serviceMode) noexcept;

// Copies UTF-8 into a fixed char array, always NUL terminated.
template <size_t N>
void copyFixed(char (&dst)[N], std::string_view src) noexcept {
    const size_t n = src.size() < N - 1 ? src.size() : N - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    for (size_t i = n; i < N; ++i) dst[i] = 0;
}

} // namespace avr::win
