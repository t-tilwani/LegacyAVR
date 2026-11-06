// AVREngine.exe - LegacyAVR real-time encoder engine (per-user, headless).
//
//   AVREngine.exe                 run until Ctrl+C / SHUTDOWN pipe command
//   AVREngine.exe --data-dir DIR  use DIR for engine.cfg + AVREngine.log
//   AVREngine.exe --quiet         no console log output (UI launches it this way)
//
// The WPF UI (LegacyAVR.UI.exe) starts this process when it cannot reach the
// control pipe, and the engine keeps running when the UI is closed to the tray.

#include "../../win/EngineHost.h"
#include "../../win/Logger.h"
#include "../../win/WinUtil.h"

#include <cstdio>
#include <string>

namespace {
avr::win::EngineHost* g_host = nullptr;

BOOL WINAPI consoleHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_host) g_host->requestStop();
            return TRUE;
        default:
            return FALSE;
    }
}

void disablePowerThrottling() {
    // Opt out of EcoQoS / timer coalescing on Windows 11 so the audio threads are
    // never parked on efficiency cores or have their waits stretched.
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    avr::win::HostOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--data-dir" && i + 1 < argc) opts.dataDir = argv[++i];
        else if (a == L"--quiet") opts.console = false;
        else if (a == L"--help" || a == L"-h" || a == L"/?") {
            std::fputs("AVREngine.exe [--data-dir DIR] [--quiet]\n", stdout);
            return 0;
        }
    }

    avr::win::ComScope com(COINIT_MULTITHREADED);
    if (!com.ok()) { std::fputs("COM initialisation failed\n", stderr); return 2; }
    disablePowerThrottling();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    avr::win::EngineHost host(opts);
    std::string err;
    if (!host.initialise(err)) {
        AVR_LOGE("startup failed: {}", err);
        std::fprintf(stderr, "AVREngine: %s\n", err.c_str());
        return 1;
    }
    g_host = &host;
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    host.run(nullptr);
    g_host = nullptr;
    return 0;
}
