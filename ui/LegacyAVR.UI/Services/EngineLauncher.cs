using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace LegacyAVR.UI.Services;

/// <summary>
/// Makes sure an engine is running: either the AVREngineService (Global mapping) or a
/// per-user AVREngine.exe started from the UI's own folder with --quiet (no console).
/// </summary>
public static class EngineLauncher
{
    public const string EngineExe = "AVREngine.exe";

    public static string EnginePath => Path.Combine(AppContext.BaseDirectory, EngineExe);

    public static bool IsEngineProcessRunning() =>
        Process.GetProcessesByName(Path.GetFileNameWithoutExtension(EngineExe)).Length > 0 ||
        Process.GetProcessesByName("AVREngineService").Length > 0;

    /// <returns>null on success, otherwise a user-facing error message.</returns>
    public static async Task<string?> EnsureRunningAsync(EngineClient client, CancellationToken ct = default)
    {
        if (await client.ConnectAsync(500, ct).ConfigureAwait(false)) return null;

        if (!IsEngineProcessRunning())
        {
            if (!File.Exists(EnginePath))
                return $"{EngineExe} was not found next to LegacyAVR.UI.exe ({AppContext.BaseDirectory}). Build the AVREngine project.";
            try
            {
                var psi = new ProcessStartInfo(EnginePath, "--quiet")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WorkingDirectory = AppContext.BaseDirectory,
                };
                Process.Start(psi);
            }
            catch (Exception ex)
            {
                return $"Could not start {EngineExe}: {ex.Message}";
            }
        }

        for (int i = 0; i < 20; i++)   // up to ~6 s (first start synthesises the voice prompts)
        {
            if (await client.ConnectAsync(300, ct).ConfigureAwait(false)) return null;
        }
        return "The engine did not respond. See %LOCALAPPDATA%\\LegacyAVR\\AVREngine.log";
    }
}
