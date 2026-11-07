using System;
using System.IO;

namespace LegacyAVR.UI.Services;

/// <summary>v1.5.0: appends unexpected dashboard errors to %LOCALAPPDATA%\LegacyAVR\LegacyAVR.UI.log
/// (next to the engine's AVREngine.log), so "Open log folder" finds both.</summary>
public static class UiLog
{
    private const long MaxBytes = 1024 * 1024;   // start again above 1 MB

    /// <returns>The log path, or null when it could not be written.</returns>
    public static string? Write(Exception ex)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LegacyAVR");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, "LegacyAVR.UI.log");
            if (File.Exists(path) && new FileInfo(path).Length > MaxBytes) File.Delete(path);
            File.AppendAllText(path, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss}  {ex}{Environment.NewLine}{Environment.NewLine}");
            return path;
        }
        catch (Exception)
        {
            return null;
        }
    }
}
