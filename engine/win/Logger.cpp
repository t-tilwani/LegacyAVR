#include "Logger.h"

#include "WinUtil.h"

#include <chrono>
#include <filesystem>

namespace avr::win {

Logger& Logger::instance() {
    static Logger l;
    return l;
}

void Logger::open(const std::wstring& path, bool console) {
    std::lock_guard lk(mu_);
    console_ = console;
    if (file_) { std::fclose(file_); file_ = nullptr; }
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    // Keep one previous log for diagnostics.
    std::filesystem::rename(path, path + L".old", ec);
    _wfopen_s(&file_, path.c_str(), L"w");   // narrow stream; we write UTF-8 bytes
}

void Logger::write(LogLevel l, std::string_view msg) {
    if (l < level_) return;
    static constexpr const char* tags[] = {"DBG", "INF", "WRN", "ERR"};
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    std::string stamp;
    try {
        stamp = std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (...) {
        stamp = std::format("{:%Y-%m-%d %H:%M:%S}Z", now);   // tzdb unavailable
    }
    const std::string line = std::format("{} [{}] {}\n", stamp, tags[int(l)], msg);
    std::lock_guard lk(mu_);
    if (l == LogLevel::Error) lastError_ = std::string(msg);
    if (file_) { std::fputs(line.c_str(), file_); std::fflush(file_); }
    if (console_) std::fputs(line.c_str(), l >= LogLevel::Warn ? stderr : stdout);
    OutputDebugStringW(toWide(line).c_str());
}

std::string Logger::lastError() const {
    std::lock_guard lk(mu_);
    return lastError_;
}

} // namespace avr::win
