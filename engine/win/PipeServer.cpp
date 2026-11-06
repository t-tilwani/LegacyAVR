#include "PipeServer.h"

#include "Logger.h"

#include "../ipc/TelemetryLayout.h"

namespace avr::win {

namespace {
constexpr DWORD kBufSize = 64 * 1024;
constexpr size_t kMaxLine = 16 * 1024;
} // namespace

bool PipeServer::start(bool serviceMode, Handler handler, std::string& error) {
    stop();
    handler_ = std::move(handler);
    serviceMode_ = serviceMode;
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) { error = "CreateEvent failed"; return false; }
    thread_ = std::thread([this] { run(); });
    return true;
}

void PipeServer::stop() noexcept {
    if (thread_.joinable()) {
        SetEvent(stopEvent_.get());
        thread_.join();
    }
}

bool PipeServer::writeAll(HANDLE pipe, const std::string& data) noexcept {
    OVERLAPPED ov{};
    Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ov.hEvent = ev.get();
    DWORD written = 0;
    if (!WriteFile(pipe, data.data(), DWORD(data.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
        return false;
    HANDLE waits[2] = {stopEvent_.get(), ev.get()};
    if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0 + 1) { CancelIoEx(pipe, &ov); return false; }
    return GetOverlappedResult(pipe, &ov, &written, FALSE) && written == data.size();
}

bool PipeServer::serveClient(HANDLE pipe) noexcept {
    std::string pending;
    char buf[4096];
    Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        ResetEvent(ev.get());
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), nullptr, &ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING && e != ERROR_MORE_DATA) return true;   // client gone
        }
        HANDLE waits[2] = {stopEvent_.get(), ev.get()};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) { CancelIoEx(pipe, &ov); return false; }   // shutting down
        if (!GetOverlappedResult(pipe, &ov, &got, FALSE) && GetLastError() != ERROR_MORE_DATA) return true;

        pending.append(buf, got);
        if (pending.size() > kMaxLine * 4) return true;   // misbehaving client
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string reply;
            try {
                reply = handler_(line);
            } catch (const std::exception& ex) {
                reply = std::string("ERR internal: ") + ex.what() + "\nEND\n";
            }
            if (!writeAll(pipe, reply)) return true;
        }
    }
}

void PipeServer::run() noexcept {
    ComScope com;   // handler enumerates devices (COM)
    setThreadName(L"AVR Control Pipe");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = createIpcSecurityDescriptor(serviceMode_);
    sa.lpSecurityDescriptor = sd;

    while (WaitForSingleObject(stopEvent_.get(), 0) != WAIT_OBJECT_0) {
        Handle pipe(CreateNamedPipeW(ipc::kPipeName,
                                     PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     PIPE_UNLIMITED_INSTANCES, kBufSize, kBufSize, 0, sd ? &sa : nullptr));
        if (!pipe) {
            AVR_LOGE("CreateNamedPipe failed: {}", hresultText(HRESULT_FROM_WIN32(GetLastError())));
            if (WaitForSingleObject(stopEvent_.get(), 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        OVERLAPPED ov{};
        Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ov.hEvent = ev.get();
        bool connected = ConnectNamedPipe(pipe.get(), &ov) != 0;
        if (!connected) {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) {
                HANDLE waits[2] = {stopEvent_.get(), ev.get()};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    CancelIoEx(pipe.get(), &ov);
                    break;
                }
                DWORD dummy = 0;
                connected = GetOverlappedResult(pipe.get(), &ov, &dummy, FALSE) != 0;
            }
        }
        if (connected) {
            AVR_LOGI("control client connected");
            const bool keepRunning = serveClient(pipe.get());
            FlushFileBuffers(pipe.get());
            DisconnectNamedPipe(pipe.get());
            AVR_LOGI("control client disconnected");
            if (!keepRunning) break;
        }
    }
    if (sd) LocalFree(sd);
}

} // namespace avr::win
