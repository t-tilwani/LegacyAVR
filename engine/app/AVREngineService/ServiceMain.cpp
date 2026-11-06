// AVREngineService.exe - LegacyAVR engine hosted as a Windows service, so encoding
// starts at boot before anyone signs in. Same engine core as AVREngine.exe; IPC uses
// the Global\ namespace with a DACL that lets interactive users connect the UI.
//
//   AVREngineService.exe --install     (elevated) register + set to Automatic
//   AVREngineService.exe --uninstall   (elevated)
//   AVREngineService.exe --console     run in the foreground for debugging
//
// Use either the service OR AVREngine.exe - a global mutex prevents both running.

#include "../../win/EngineHost.h"
#include "../../win/Logger.h"
#include "../../win/WinUtil.h"

#include <cstdio>
#include <string>

namespace {

constexpr wchar_t kServiceName[] = L"LegacyAVREngine";
constexpr wchar_t kDisplayName[] = L"LegacyAVR Encoder Engine (AVREngineService)";
constexpr wchar_t kDescription[] =
    L"Real-time Dolby Digital / DTS encoder: captures the 7.1 virtual cable and bitstreams to HDMI ARC / S/PDIF.";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;

void report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            SetEvent(g_stopEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

int runEngine(bool serviceMode, bool console) {
    avr::win::ComScope com(COINIT_MULTITHREADED);
    avr::win::HostOptions opts;
    opts.serviceMode = serviceMode;
    opts.console = console;
    avr::win::EngineHost host(opts);
    std::string err;
    if (!host.initialise(err)) {
        AVR_LOGE("service startup failed: {}", err);
        if (console) std::fprintf(stderr, "AVREngineService: %s\n", err.c_str());
        return 1;
    }
    if (serviceMode) report(SERVICE_RUNNING);
    host.run(g_stopEvent);
    return 0;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, controlHandler, nullptr);
    if (!g_statusHandle) return;
    report(SERVICE_START_PENDING, NO_ERROR, 10000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const int rc = runEngine(true, false);
    CloseHandle(g_stopEvent);
    report(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

int install() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { std::fputs("OpenSCManager failed (run elevated)\n", stderr); return 1; }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr,
                                   L"Audiosrv\0AudioEndpointBuilder\0\0", nullptr /*LocalSystem*/, nullptr);
    if (!svc) {
        std::fprintf(stderr, "CreateService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_DESCRIPTIONW desc{const_cast<LPWSTR>(kDescription)};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    // Restart automatically after a crash (5 s, 5 s, 30 s).
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    StartServiceW(svc, 0, nullptr);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::fputs("LegacyAVREngine service installed and started.\n", stdout);
    return 0;
}

int uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { std::fputs("OpenSCManager failed (run elevated)\n", stderr); return 1; }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) { std::fputs("service not installed\n", stderr); CloseServiceHandle(scm); return 1; }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
    const BOOL ok = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::fputs(ok ? "LegacyAVREngine service removed.\n" : "DeleteService failed\n", ok ? stdout : stderr);
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        const std::wstring a = argv[1];
        if (a == L"--install") return install();
        if (a == L"--uninstall") return uninstall();
        if (a == L"--console") {
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            SetConsoleCtrlHandler([](DWORD) -> BOOL { SetEvent(g_stopEvent); return TRUE; }, TRUE);
            return runEngine(true, true);
        }
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        std::fputs("AVREngineService.exe is a Windows service. Use --install, --uninstall or --console.\n", stderr);
        return 1;
    }
    return 0;
}
