// LegacyAVR - thread-safe logger (file + debugger + optional console).
// NEVER call from the capture / DSP / render threads: those report failures
// through atomics that the supervisor thread turns into log lines.
#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace avr::win {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static Logger& instance();

    void open(const std::wstring& path, bool console);
    void setLevel(LogLevel l) noexcept { level_ = l; }
    void write(LogLevel l, std::string_view msg);
    [[nodiscard]] std::string lastError() const;

private:
    Logger() = default;
    mutable std::mutex mu_;
    std::FILE* file_ = nullptr;
    bool console_ = false;
    LogLevel level_ = LogLevel::Info;
    std::string lastError_;
};

// (Named logFormat, not logf, to stay clear of the C library's ::logf.)
template <class... A>
void logFormat(LogLevel l, std::format_string<A...> fmt, A&&... args) {
    Logger::instance().write(l, std::format(fmt, std::forward<A>(args)...));
}

#define AVR_LOGD(...) ::avr::win::logFormat(::avr::win::LogLevel::Debug, __VA_ARGS__)
#define AVR_LOGI(...) ::avr::win::logFormat(::avr::win::LogLevel::Info, __VA_ARGS__)
#define AVR_LOGW(...) ::avr::win::logFormat(::avr::win::LogLevel::Warn, __VA_ARGS__)
#define AVR_LOGE(...) ::avr::win::logFormat(::avr::win::LogLevel::Error, __VA_ARGS__)

} // namespace avr::win
