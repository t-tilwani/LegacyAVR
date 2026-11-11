using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace LegacyAVR.UI.Services;

public enum StatsDisplayMode { Average = 0, Live = 1, Off = 2 }

/// <summary>Dashboard-only settings (%APPDATA%\LegacyAVR\ui-settings.json).</summary>
public sealed class UiSettings
{
    public bool StopEngineOnExit { get; set; } = true;
    public bool StartWithWindows { get; set; }
    public bool TrayHintShown { get; set; }
    public StatsDisplayMode StatsMode { get; set; } = StatsDisplayMode.Average;

    // ---- v1.4.0 profiles
    /// <summary>Name of the last profile applied (re-applied at startup when enabled).</summary>
    public string? LastProfile { get; set; }
    public bool ApplyLastProfileAtStartup { get; set; } = true;
    public bool ProfilesMigrated { get; set; }

    // ---- v1.4.0 output selection (LegacyAVR's own default - Windows' default device is never changed)
    public string? DefaultOutputDeviceId { get; set; }
    public string? DefaultOutputDeviceName { get; set; }
    /// <summary>Last output device seen in use, so a disconnected device can still be named.</summary>
    public string? LastOutputDeviceId { get; set; }
    public string? LastOutputDeviceName { get; set; }

    // ---- v1.6.0
    /// <summary>Short chime when audio starts with the app and after a successful input change (default on).</summary>
    public bool ActivationChime { get; set; } = true;

    // ---- read from older versions only (kept so a downgrade still finds them)
    public string? LastPreset { get; set; }
    public bool DefaultPresetsSeeded { get; set; }
}

/// <summary>Loads and saves <see cref="UiSettings"/>; owns the data folder.</summary>
public sealed class SettingsService
{
    internal static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNameCaseInsensitive = true,
    };

    public static string AppFolder { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "LegacyAVR");

    private readonly string _folder;
    private string SettingsFile => Path.Combine(_folder, "ui-settings.json");

    public SettingsService(string? folder = null) => _folder = folder ?? AppFolder;

    public string Folder => _folder;
    public UiSettings Settings { get; private set; } = new();

    public void Load()
    {
        try
        {
            if (File.Exists(SettingsFile))
                Settings = JsonSerializer.Deserialize<UiSettings>(File.ReadAllText(SettingsFile), Json) ?? new();
        }
        catch (Exception) { Settings = new(); }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(_folder);
            File.WriteAllText(SettingsFile, JsonSerializer.Serialize(Settings, Json));
        }
        catch (Exception) { /* settings are a convenience: never crash the dashboard over them */ }
    }
}
