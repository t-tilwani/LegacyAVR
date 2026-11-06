#include "WinUtil.h"

#include <sddl.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <cstdio>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "advapi32.lib")

namespace avr::win {

std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring toWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string hresultText(HRESULT hr) {
    const char* name = nullptr;
    switch (static_cast<unsigned long>(hr)) {
        case 0x88890001: name = "AUDCLNT_E_NOT_INITIALIZED"; break;
        case 0x88890004: name = "AUDCLNT_E_DEVICE_INVALIDATED"; break;
        case 0x88890008: name = "AUDCLNT_E_UNSUPPORTED_FORMAT"; break;
        case 0x8889000A: name = "AUDCLNT_E_DEVICE_IN_USE"; break;
        case 0x8889000E: name = "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED"; break;
        case 0x88890013: name = "AUDCLNT_E_EVENTHANDLE_NOT_SET"; break;
        case 0x88890017: name = "AUDCLNT_E_BUFFER_SIZE_ERROR"; break;
        case 0x88890019: name = "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"; break;
        case 0x88890020: name = "AUDCLNT_E_INVALID_DEVICE_PERIOD"; break;
        case 0x80070490: name = "E_NOTFOUND (device not found)"; break;
        case 0x80070005: name = "E_ACCESSDENIED"; break;
        default: break;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "0x%08lX%s%s", static_cast<unsigned long>(hr), name ? " " : "", name ? name : "");
    return buf;
}

uint64_t qpcNow() noexcept {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return uint64_t(v.QuadPart);
}

uint64_t nowUs() noexcept {
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    const uint64_t t = qpcNow();
    return (t / freq) * 1'000'000ull + (t % freq) * 1'000'000ull / freq;
}

void enableFlushToZero() noexcept {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

void setThreadName(const wchar_t* name) noexcept {
    SetThreadDescription(GetCurrentThread(), name);
}

PSECURITY_DESCRIPTOR createIpcSecurityDescriptor(bool serviceMode) noexcept {
    if (!serviceMode) return nullptr;
    // SYSTEM + Administrators: full; Interactive users: read/write.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr))
        return nullptr;
    return sd;
}

} // namespace avr::win
