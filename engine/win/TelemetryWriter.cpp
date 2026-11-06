#include "TelemetryWriter.h"

#include <atomic>
#include <cstring>

namespace avr::win {

bool TelemetryWriter::open(bool serviceMode, std::string& error) {
    close();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = createIpcSecurityDescriptor(serviceMode);
    sa.lpSecurityDescriptor = sd;
    const wchar_t* name = serviceMode ? ipc::kTelemetryNameGlobal : ipc::kTelemetryNameLocal;
    mapping_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0,
                                      sizeof(ipc::TelemetryBlock), name));
    if (sd) LocalFree(sd);
    if (!mapping_) { error = "CreateFileMapping failed: " + hresultText(HRESULT_FROM_WIN32(GetLastError())); return false; }
    view_ = static_cast<ipc::TelemetryBlock*>(
        MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::TelemetryBlock)));
    if (!view_) { error = "MapViewOfFile failed"; mapping_.reset(); return false; }
    std::memset(view_, 0, sizeof(*view_));
    shadow_ = {};
    seq_ = 0;
    return true;
}

void TelemetryWriter::close() noexcept {
    if (view_) {
        view_->engineState = 0;   // Stopped
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    mapping_.reset();
}

void TelemetryWriter::write(const ipc::TelemetryBlock& b) noexcept {
    // Seqlock: odd while writing. Interlocked ops are full barriers on x64.
    auto* seq = reinterpret_cast<volatile LONG*>(&view_->sequence);
    InterlockedExchange(seq, LONG(++seq_));          // odd
    std::atomic_thread_fence(std::memory_order_release);
    auto* dst = reinterpret_cast<uint8_t*>(view_);
    const auto* src = reinterpret_cast<const uint8_t*>(&b);
    constexpr size_t kSeqOff = offsetof(ipc::TelemetryBlock, sequence);
    std::memcpy(dst, src, kSeqOff);                                       // before sequence
    std::memcpy(dst + kSeqOff + 4, src + kSeqOff + 4, sizeof(b) - kSeqOff - 4);   // after
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange(seq, LONG(++seq_));          // even
}

} // namespace avr::win
