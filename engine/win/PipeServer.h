// LegacyAVR - named-pipe control server (\\.\pipe\LegacyAVR.Control).
// Overlapped I/O so stop() can interrupt a blocking connect/read immediately.
// Handles one client at a time (the UI); requests are processed sequentially.
#pragma once

#include "WinUtil.h"

#include <functional>
#include <string>
#include <thread>

namespace avr::win {

class PipeServer {
public:
    using Handler = std::function<std::string(const std::string& line)>;

    ~PipeServer() { stop(); }
    bool start(bool serviceMode, Handler handler, std::string& error);
    void stop() noexcept;

private:
    void run() noexcept;
    bool serveClient(HANDLE pipe) noexcept;
    bool writeAll(HANDLE pipe, const std::string& data) noexcept;

    Handler handler_;
    Handle stopEvent_;
    std::thread thread_;
    bool serviceMode_ = false;
};

} // namespace avr::win
