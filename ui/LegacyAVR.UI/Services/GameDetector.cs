using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace LegacyAVR.UI.Services;

/// <summary>
/// "Auto" game detection (v1.2.0): a full-screen window in the foreground (exclusive
/// Direct3D full screen, or a borderless window covering its monitor) that does not belong
/// to a browser, media player or other known non-game app. The dashboard sends the result
/// to the engine as a GAME_DETECTED heartbeat; the engine forgets it after 6 s, so a
/// closed dashboard can never leave game mode stuck on.
/// </summary>
public static partial class GameDetector
{
    // Process names (without .exe) that are full-screen often but are not games.
    private static readonly HashSet<string> NotGames = new(StringComparer.OrdinalIgnoreCase)
    {
        // browsers
        "chrome", "msedge", "msedgewebview2", "firefox", "opera", "opera_gx", "brave", "vivaldi", "iexplore",
        "arc", "waterfox", "librewolf", "chromium", "thorium", "floorp", "zen",
        // media players / streaming apps
        "vlc", "mpc-hc", "mpc-hc64", "mpc-be", "mpc-be64", "mpv", "smplayer", "potplayer", "potplayermini",
        "potplayermini64", "potplayer64", "wmplayer", "Microsoft.Media.Player", "Video.UI", "kodi", "plex",
        "plexamp", "plex htpc", "jellyfin media player", "jellyfinmediaplayer", "emby", "spotify", "itunes",
        "applemusic", "appletv", "foobar2000", "musicbee", "aimp", "winamp", "netflix", "disney+", "primevideo",
        "ApplicationFrameHost",
        // shell, office, communication, capture, this app
        "explorer", "ShellExperienceHost", "StartMenuExperienceHost", "SearchHost", "LockApp", "dwm",
        "powerpnt", "winword", "excel", "acrord32", "acrobat", "teams", "ms-teams", "zoom", "discord", "slack",
        "obs64", "obs32", "LegacyAVR.UI", "AVREngine", "AVREngineService", "mstsc", "vmware", "virtualboxvm",
    };

    /// <summary>True if a full-screen window of this process may be a game.</summary>
    public static bool IsGameCandidate(string processName) =>
        !string.IsNullOrWhiteSpace(processName) && !NotGames.Contains(processName.Trim());

    /// <summary>Checks the foreground window. Cheap; call about once a second.</summary>
    public static (bool IsGame, string Name) Probe()
    {
        try
        {
            if (SHQueryUserNotificationState(out int quns) != 0) quns = 0;
            bool d3dFullScreen = quns == 3;   // QUNS_RUNNING_D3D_FULL_SCREEN
            bool busyFullScreen = quns == 2;  // QUNS_BUSY: a full-screen app is in the foreground

            IntPtr hwnd = GetForegroundWindow();
            if (hwnd == IntPtr.Zero || hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return (false, string.Empty);

            bool coversMonitor = false;
            if (GetWindowRect(hwnd, out var wr))
            {
                IntPtr mon = MonitorFromWindow(hwnd, 2 /* MONITOR_DEFAULTTONEAREST */);
                var mi = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
                if (mon != IntPtr.Zero && GetMonitorInfoW(mon, ref mi))
                    coversMonitor = wr.Left <= mi.rcMonitor.Left && wr.Top <= mi.rcMonitor.Top &&
                                    wr.Right >= mi.rcMonitor.Right && wr.Bottom >= mi.rcMonitor.Bottom;
            }
            if (!d3dFullScreen && !busyFullScreen && !coversMonitor) return (false, string.Empty);

            GetWindowThreadProcessId(hwnd, out uint pid);
            if (pid == 0 || pid == (uint)Environment.ProcessId) return (false, string.Empty);
            string name;
            using (var p = Process.GetProcessById((int)pid)) name = p.ProcessName;
            return (IsGameCandidate(name), name);
        }
        catch (Exception)
        {
            return (false, string.Empty);   // process exited / access denied: treat as no game
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [LibraryImport("user32.dll")]
    private static partial IntPtr GetForegroundWindow();

    [LibraryImport("user32.dll")]
    private static partial IntPtr GetShellWindow();

    [LibraryImport("user32.dll")]
    private static partial IntPtr GetDesktopWindow();

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [LibraryImport("user32.dll")]
    private static partial IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetMonitorInfoW(IntPtr hMonitor, ref MONITORINFO info);

    [LibraryImport("user32.dll")]
    private static partial uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [LibraryImport("shell32.dll")]
    private static partial int SHQueryUserNotificationState(out int state);
}
