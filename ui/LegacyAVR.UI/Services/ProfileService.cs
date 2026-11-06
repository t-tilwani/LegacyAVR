using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Tasks;
using LegacyAVR.UI.Interop;

namespace LegacyAVR.UI.Services;

/// <summary>
/// v1.4.0: one profile = a complete set of LegacyAVR settings. It replaces both the presets
/// (v1.0-1.3) and the device profiles (v1.3).
/// <para>
/// Every setting is optional on purpose: a profile saved in 1.4 stores everything, while one
/// converted from an old preset only stores what the preset had (format, stereo handling,
/// LFE, game mode, bass, Night Mode) and leaves the rest - speakers, trims, EQ - as they are.
/// </para>
/// <para>
/// <see cref="DeviceId"/>/<see cref="DeviceName"/> remember the output device the profile was
/// made for. They are a preference, not a lock: a profile can always be applied to the
/// output currently selected instead (portable profiles, imports from another PC).
/// </para>
/// The property names are the same as in the old preset and device-profile files, so those
/// files load directly into this type.
/// </summary>
public sealed record Profile
{
    public required string Name { get; init; }
    public string? Description { get; init; }
    public string? DeviceId { get; init; }
    public string? DeviceName { get; init; }

    // Output & encoder
    public CodecKind? Codec { get; init; }
    public uint? Bitrate { get; init; }
    public OutputModeKind? OutputMode { get; init; }
    public int? BufferMs { get; init; }
    // Processing
    public UpmixKind? Upmix { get; init; }
    public bool? StereoAs20 { get; init; }
    public bool? NightMode { get; init; }
    public LfeGenKind? LfeGen { get; init; }
    public GamingKind? Gaming { get; init; }
    public bool? GameLfeGen { get; init; }
    public double? LfeResetSeconds { get; init; }
    public double? BassDb { get; init; }
    public double? CentreLevel { get; init; }
    public double? CentreFloor { get; init; }
    public double? DialogBoost { get; init; }
    public double? SurroundLevel { get; init; }
    public double? SurroundDelayMs { get; init; }
    // Speakers
    public bool? DedicatedSub { get; init; }
    public double? LfeCrossoverHz { get; init; }
    public double? LfeLevelDb { get; init; }
    public double? MasterDb { get; init; }
    public double[]? GainsDb { get; init; }
    public bool[]? Mutes { get; init; }
    public EqSettings? Eq { get; init; }

    /// <summary>True when every setting is stored (anything saved by 1.4).</summary>
    [JsonIgnore]
    public bool IsComplete =>
        Codec is not null && Bitrate is not null && OutputMode is not null && BufferMs is not null && Upmix is not null &&
        StereoAs20 is not null && NightMode is not null && LfeGen is not null && Gaming is not null && GameLfeGen is not null &&
        LfeResetSeconds is not null && BassDb is not null && CentreLevel is not null && CentreFloor is not null &&
        DialogBoost is not null && SurroundLevel is not null && SurroundDelayMs is not null && DedicatedSub is not null &&
        LfeCrossoverHz is not null && LfeLevelDb is not null && MasterDb is not null && GainsDb is { Length: 6 } &&
        Mutes is { Length: 6 } && Eq is not null;

    [JsonIgnore]
    public bool HasDevice => !string.IsNullOrEmpty(DeviceId);

    /// <summary>List text: "Movies  (Denon AVR)" or "Late night  (any output, partial)".</summary>
    [JsonIgnore]
    public string Display => $"{Name}  ({(HasDevice ? DeviceName ?? "saved device" : "any output")}{(IsComplete ? "" : ", partial")})";

    public override string ToString() => Name;

    // Records compare arrays by reference; profiles are compared by content in tests.
    public bool SameSettings(Profile o) =>
        ProfileStore.Commands(this with { Name = "", Description = null }, null)
            .SequenceEqual(ProfileStore.Commands(o with { Name = "", Description = null }, null)) &&
        DeviceId == o.DeviceId && DeviceName == o.DeviceName;
}

/// <summary>What happens to a profile's output device when it is applied.</summary>
public enum ProfileTarget
{
    AnyOutput,          // portable profile: applied to the output currently selected
    SameOutput,         // made for the output that is selected now
    OtherOutput,        // made for another output that is connected: switch to it, or apply here
    OutputMissing,      // made for an output that is not connected now: can only be applied here
}

/// <summary>Result of an import.</summary>
public sealed record ImportResult(int Added, int Renamed, IReadOnlyList<string> Names);

/// <summary>
/// Stores profiles in %APPDATA%\LegacyAVR\profiles.json and converts, imports and exports
/// them. On first use it migrates presets.json and device-profiles.json (left untouched, so an
/// older LegacyAVR still finds them).
/// </summary>
public sealed class ProfileStore
{
    public const string FileFormat = "LegacyAVR.Profiles";
    public const int FileVersion = 2;

    private sealed record ProfileFile(string Format, int Version, List<Profile> Profiles);

    private readonly string _folder;
    private readonly bool _persist;
    public ProfileStore(string folder, bool persist = true)
    {
        _folder = folder;
        _persist = persist;
    }

    private string FilePath => Path.Combine(_folder, "profiles.json");
    public List<Profile> Profiles { get; private set; } = new();
    /// <summary>One-line summary of what the first-run migration did ("" when nothing ran).</summary>
    public string MigrationNote { get; private set; } = string.Empty;

    // ------------------------------------------------------------------ built-in starting points
    /// <summary>The five set-ups most people need (the former default presets). They store no
    /// output device, speaker levels or EQ, so they work on any output.</summary>
    public static IReadOnlyList<Profile> Defaults { get; } = new[]
    {
        Partial("Dolby Digital 5.1 - 640 kbps", CodecKind.AC3, 640000, UpmixKind.Upmix51, false, LfeGenKind.Auto, 0,
                "Works with almost every AVR, soundbar and TV (optical or HDMI ARC). Stereo is upmixed to 5.1, 5.0 sources get an LFE, games are left alone."),
        Partial("DTS 5.1 - 1509 kbps", CodecKind.DTS, 1509750, UpmixKind.Upmix51, false, LfeGenKind.Auto, 0,
                "Highest-bitrate surround over optical. For receivers that decode DTS."),
        Partial("Dolby Digital Plus 5.1 - 1024 kbps (HDMI ARC)", CodecKind.EAC3, 1024000, UpmixKind.Upmix51, false, LfeGenKind.Auto, 0,
                "Better quality than Dolby Digital, but only over HDMI ARC to devices that list DD+. Not possible over optical."),
        Partial("Stereo music: lossless PCM 2.0, DD 5.1 for games", CodecKind.AC3, 640000, UpmixKind.PcmDirect, false, LfeGenKind.Auto, 0,
                "Stereo sources stay lossless PCM; switches to Dolby Digital 5.1 automatically when a game or film plays surround. Game mode keeps games on Dolby Digital."),
        Partial("Late night - Dolby Digital 5.1 + Night Mode", CodecKind.AC3, 448000, UpmixKind.Upmix51, true, LfeGenKind.Off, -4,
                "Compresses loud effects and lifts dialogue so you can keep the volume low. Bass is turned down 4 dB."),
    };

    private static Profile Partial(string name, CodecKind codec, uint bitrate, UpmixKind upmix, bool night, LfeGenKind lfe, double bass, string desc) =>
        new()
        {
            Name = name, Description = desc, Codec = codec, Bitrate = bitrate, OutputMode = OutputModeKind.Exclusive, BufferMs = 0,
            Upmix = upmix, StereoAs20 = true, NightMode = night, LfeGen = lfe, Gaming = GamingKind.Auto, BassDb = bass,
        };

    // ------------------------------------------------------------------ persistence
    /// <summary>Loads profiles.json, or migrates the 1.0-1.3 files the first time.</summary>
    /// <param name="seedDefaults">Add the five starting points when nothing could be migrated (a new install).</param>
    public void Load(bool seedDefaults = true)
    {
        MigrationNote = string.Empty;
        if (File.Exists(FilePath))
        {
            try { Profiles = Parse(File.ReadAllText(FilePath), allowEmpty: true); return; }   // deleting every profile is allowed
            catch (Exception)
            {
                // A damaged file is kept aside, never overwritten silently.
                TryCopy(FilePath, FilePath + ".damaged");
                MigrationNote = "profiles.json could not be read; a copy was kept as profiles.json.damaged.";
            }
        }
        Profiles = new List<Profile>();
        int presets = 0, devices = 0;
        var presetFile = Path.Combine(_folder, "presets.json");
        var deviceFile = Path.Combine(_folder, "device-profiles.json");
        try { if (File.Exists(presetFile)) foreach (var p in ReadFile(presetFile)) { AddUnique(p); presets++; } }
        catch (Exception) { /* unreadable legacy file: skip it */ }
        try
        {
            if (File.Exists(deviceFile))
                foreach (var p in ReadFile(deviceFile))
                {
                    // Device profiles were unique per device; profiles are unique by name.
                    var named = Find(p.Name) is null ? p : p with { Name = $"{p.Name} ({p.DeviceName ?? "device"})" };
                    AddUnique(named);
                    devices++;
                }
        }
        catch (Exception) { }
        if (presets + devices == 0 && seedDefaults) Profiles.AddRange(Defaults);
        if (presets + devices > 0)
            MigrationNote = $"Profiles now replace presets and device profiles: {presets} preset(s) and {devices} device profile(s) were converted.";
        Save();
    }

    public void Save()
    {
        if (!_persist) return;
        Directory.CreateDirectory(_folder);
        var tmp = FilePath + ".tmp";
        File.WriteAllText(tmp, Serialize(Profiles));
        File.Move(tmp, FilePath, overwrite: true);
    }

    public static string Serialize(IEnumerable<Profile> profiles) =>
        JsonSerializer.Serialize(new ProfileFile(FileFormat, FileVersion, profiles.ToList()), SettingsService.Json);

    private static void TryCopy(string from, string to)
    {
        try { File.Copy(from, to, overwrite: true); } catch (Exception) { }
    }

    // ------------------------------------------------------------------ list operations
    public Profile? Find(string? name) =>
        name is null ? null : Profiles.FirstOrDefault(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));

    /// <summary>Adds, or replaces the profile with the same name (keeps its position).</summary>
    public void Upsert(Profile p)
    {
        int i = Profiles.FindIndex(x => string.Equals(x.Name, p.Name, StringComparison.OrdinalIgnoreCase));
        if (i >= 0) Profiles[i] = p;
        else Profiles.Add(p);
        Save();
    }

    public bool Delete(string name)
    {
        bool removed = Profiles.RemoveAll(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase)) > 0;
        if (removed) Save();
        return removed;
    }

    /// <summary>"Movies", "Movies (2)", "Movies (3)"... - the first name not in use.</summary>
    public string UniqueName(string name)
    {
        name = string.IsNullOrWhiteSpace(name) ? "Profile" : name.Trim();
        if (Find(name) is null) return name;
        for (int n = 2; ; n++)
            if (Find($"{name} ({n})") is null) return $"{name} ({n})";
    }

    private void AddUnique(Profile p) => Profiles.Add(p with { Name = UniqueName(p.Name) });

    /// <summary>Re-adds the five starting points (existing ones with the same name are reset).</summary>
    public void RestoreDefaults()
    {
        for (int k = 0; k < Defaults.Count; k++)
        {
            var d = Defaults[k];
            int i = Profiles.FindIndex(x => string.Equals(x.Name, d.Name, StringComparison.OrdinalIgnoreCase));
            if (i >= 0) Profiles[i] = d;
            else Profiles.Insert(Math.Min(Profiles.Count, k), d);
        }
        Save();
    }

    // ------------------------------------------------------------------ import / export
    public static void Export(string path, IEnumerable<Profile> profiles) => File.WriteAllText(path, Serialize(profiles));

    /// <summary>
    /// Reads any LegacyAVR profile file: a 1.4 profile file, a list of 1.0-1.3 presets, a list
    /// of 1.3 device profiles, or a single exported item. Values are brought into range.
    /// </summary>
    public static List<Profile> ReadFile(string path) => Parse(File.ReadAllText(path));

    public static List<Profile> Parse(string json, bool allowEmpty = false)
    {
        using var doc = JsonDocument.Parse(json, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
        var root = doc.RootElement;
        IEnumerable<JsonElement> items = root.ValueKind switch
        {
            JsonValueKind.Array => root.EnumerateArray(),
            JsonValueKind.Object when TryGet(root, "Profiles", out var arr) && arr.ValueKind == JsonValueKind.Array => arr.EnumerateArray(),
            JsonValueKind.Object => new[] { root },
            _ => throw new InvalidDataException("not a LegacyAVR profile file"),
        };
        var list = new List<Profile>();
        foreach (var e in items)
        {
            if (e.ValueKind != JsonValueKind.Object || !TryGet(e, "Name", out var n) || n.ValueKind != JsonValueKind.String ||
                string.IsNullOrWhiteSpace(n.GetString()))
                continue;
            var p = e.Deserialize<Profile>(SettingsService.Json);
            if (p != null) list.Add(Normalise(p));
        }
        if (list.Count == 0 && !allowEmpty) throw new InvalidDataException("the file contains no profiles");
        return list;
    }

    private static bool TryGet(JsonElement e, string name, out JsonElement value)
    {
        foreach (var prop in e.EnumerateObject())
            if (string.Equals(prop.Name, name, StringComparison.OrdinalIgnoreCase)) { value = prop.Value; return true; }
        value = default;
        return false;
    }

    /// <summary>Brings every stored value into the range the engine accepts.</summary>
    public static Profile Normalise(Profile p) => p with
    {
        Name = p.Name.Trim(),
        BufferMs = p.BufferMs is int ms ? (ms is 0 or (>= 5 and <= 200) ? ms : 0) : null,
        LfeResetSeconds = p.LfeResetSeconds is double r ? FactoryDefaults.ClampLfeReset(r) : null,
        BassDb = p.BassDb is double b ? Math.Clamp(b, -12, 12) : null,
        CentreLevel = Clamp01(p.CentreLevel),
        CentreFloor = Clamp01(p.CentreFloor),
        DialogBoost = Clamp01(p.DialogBoost),
        SurroundLevel = Clamp01(p.SurroundLevel),
        SurroundDelayMs = p.SurroundDelayMs is double d ? Math.Clamp(d, 0, 25) : null,
        LfeCrossoverHz = p.LfeCrossoverHz is double x ? Math.Clamp(x, 40, 200) : null,
        LfeLevelDb = p.LfeLevelDb is double l ? Math.Clamp(l, -30, 6) : null,
        MasterDb = p.MasterDb is double m ? Math.Clamp(m, -60, 12) : null,
        GainsDb = p.GainsDb is { Length: 6 } g ? g.Select(v => double.IsFinite(v) ? Math.Clamp(v, -48.5, 6) : 0).ToArray() : null,
        Mutes = p.Mutes is { Length: 6 } mu ? mu.ToArray() : null,
        Eq = p.Eq is null ? null : EqModel.Normalise(p.Eq),
    };

    private static double? Clamp01(double? v) => v is double d ? Math.Clamp(d, 0, 1) : null;

    /// <summary>Adds every profile of the file. Names already in use get " (2)" etc. - nothing
    /// existing is overwritten.</summary>
    public ImportResult Import(string path) => Import(ReadFile(path));

    public ImportResult Import(IEnumerable<Profile> incoming)
    {
        int renamed = 0;
        var names = new List<string>();
        foreach (var p in incoming)
        {
            var name = UniqueName(p.Name);
            if (name != p.Name) renamed++;
            Profiles.Add(p with { Name = name });
            names.Add(name);
        }
        Save();
        return new ImportResult(names.Count, renamed, names);
    }

    // ------------------------------------------------------------------ applying
    public static ProfileTarget TargetOf(Profile p, string? currentOutputId, IEnumerable<string> availableOutputIds)
    {
        if (!p.HasDevice) return ProfileTarget.AnyOutput;
        if (string.Equals(p.DeviceId, currentOutputId, StringComparison.OrdinalIgnoreCase)) return ProfileTarget.SameOutput;
        return availableOutputIds.Any(id => string.Equals(id, p.DeviceId, StringComparison.OrdinalIgnoreCase))
            ? ProfileTarget.OtherOutput
            : ProfileTarget.OutputMissing;
    }

    /// <summary>
    /// Every engine command needed to reproduce the profile. <paramref name="switchToDeviceId"/>
    /// non-null adds SET_OUTPUT_DEVICE first; null keeps the output that is selected now (this is
    /// how a profile is applied to another output). Settings the profile does not store are left
    /// out, so they keep their current value.
    /// </summary>
    public static List<string> Commands(Profile p, string? switchToDeviceId)
    {
        static string F(double v) => Proto.F(v);
        var c = new List<string>();
        if (!string.IsNullOrEmpty(switchToDeviceId)) c.Add($"SET_OUTPUT_DEVICE {Proto.Quote(switchToDeviceId)}");
        if (p.DedicatedSub is bool sub) c.Add($"SET_SUB {(sub ? 1 : 0)}");
        if (p.Upmix is { } up) c.Add($"SET_UPMIX {UpmixToken(up)}");
        if (p.StereoAs20 is bool s20) c.Add($"SET_STEREO_AS_20 {(s20 ? 1 : 0)}");
        if (p.NightMode is bool night) c.Add($"SET_NIGHT_MODE {(night ? 1 : 0)}");
        if (p.OutputMode is { } om) c.Add($"SET_OUTPUT_MODE {(om == OutputModeKind.Shared ? "SHARED" : "EXCLUSIVE")}");
        if (p.Codec is { } codec) c.Add($"SET_CODEC {codec}");
        // SET_CODEC resets the bitrate to the codec default, so the bitrate always follows it.
        if (p.Bitrate is uint br && br > 0 && (p.Codec ?? CodecKind.AC3) != CodecKind.PCM) c.Add($"SET_BITRATE {br}");
        if (p.BufferMs is int ms) c.Add($"SET_BUFFER_MS {ms}");
        if (p.LfeGen is { } lg) c.Add($"SET_LFE_GEN {LfeGenToken(lg)}");
        if (p.Gaming is { } gm) c.Add($"SET_GAMING {GamingToken(gm)}");
        if (p.GameLfeGen is bool glfe) c.Add($"SET_GAME_LFE {(glfe ? 1 : 0)}");
        if (p.LfeResetSeconds is double reset) c.Add($"SET_LFE_RESET {F(FactoryDefaults.ClampLfeReset(reset))}");
        if (p.BassDb is double bass) c.Add($"SET_BASS {F(Math.Clamp(bass, -12, 12))}");
        if (p.LfeCrossoverHz is double xo) c.Add($"SET_UPMIX_PARAM LFE_XOVER {F(xo)}");
        if (p.LfeLevelDb is double ll) c.Add($"SET_UPMIX_PARAM LFE_LEVEL {F(ll)}");
        if (p.CentreLevel is double ce) c.Add($"SET_UPMIX_PARAM CENTRE {F(ce)}");
        if (p.CentreFloor is double cf) c.Add($"SET_UPMIX_PARAM CENTRE_FLOOR {F(cf)}");
        if (p.DialogBoost is double db) c.Add($"SET_UPMIX_PARAM DIALOG {F(db)}");
        if (p.SurroundLevel is double sl) c.Add($"SET_UPMIX_PARAM SURROUND {F(sl)}");
        if (p.SurroundDelayMs is double sd) c.Add($"SET_UPMIX_PARAM DELAY {F(sd)}");
        if (p.MasterDb is double master) c.Add($"SET_MASTER {F(master)}");
        for (int i = 0; i < 6; i++)
        {
            if (p.GainsDb is { Length: 6 }) c.Add($"SET_GAIN {i} {F(p.GainsDb[i])}");
            if (p.Mutes is { Length: 6 }) c.Add($"SET_MUTE {i} {(p.Mutes[i] ? 1 : 0)}");
        }
        if (p.Eq is { } eq)
        {
            var n = EqModel.Normalise(eq);
            for (int ch = 0; ch < EqModel.Channels; ch++)
            {
                for (int b = 0; b < EqModel.Bands; b++) c.Add(EqModel.BandCommand(ch, b, n.Channels[ch].Bands[b]));
                c.Add($"SET_EQ_CHANNEL {ch} {(n.Channels[ch].Enabled ? 1 : 0)}");
            }
            c.Add($"SET_EQ {(n.Enabled ? 1 : 0)}");
        }
        return c;
    }

    /// <summary>
    /// The commands that actually change something compared with <paramref name="current"/>
    /// (a complete capture of the engine state). Avoids needless output re-opens, e.g. when
    /// the last profile is re-applied at startup and nothing differs.
    /// </summary>
    public static List<string> ChangedCommands(Profile p, Profile current, string? switchToDeviceId)
    {
        var now = new HashSet<string>(Commands(current, null), StringComparer.Ordinal);
        var all = Commands(p, switchToDeviceId);
        bool codecSent = false;
        var result = new List<string>();
        foreach (var cmd in all)
        {
            bool isBitrate = cmd.StartsWith("SET_BITRATE ", StringComparison.Ordinal);
            if (cmd.StartsWith("SET_OUTPUT_DEVICE ", StringComparison.Ordinal) || !now.Contains(cmd) || (isBitrate && codecSent))
            {
                result.Add(cmd);
                if (cmd.StartsWith("SET_CODEC ", StringComparison.Ordinal)) codecSent = true;
            }
        }
        return result;
    }

    /// <summary>Sends the commands; restart-level ones are coalesced by the engine into one re-open.</summary>
    public static async Task<string?> SendAsync(EngineClient client, IEnumerable<string> commands)
    {
        foreach (var cmd in commands)
        {
            var r = await client.SendAsync(cmd);
            if (!r.Ok) return $"{cmd.Split(' ')[0]}: {r.Message}";
        }
        return null;
    }

    // ------------------------------------------------------------------ protocol tokens
    public static string LfeGenToken(LfeGenKind k) => k switch
    {
        LfeGenKind.Off => "OFF",
        LfeGenKind.Always => "ALWAYS",
        _ => "AUTO",
    };

    public static string GamingToken(GamingKind k) => k switch
    {
        GamingKind.On => "ON",
        GamingKind.Off => "OFF",
        _ => "AUTO",
    };

    public static string UpmixToken(UpmixKind u) => u switch
    {
        UpmixKind.Passthrough => "PASSTHROUGH",
        UpmixKind.Upmix31 => "UPMIX31",
        UpmixKind.PcmDirect => "PCM_DIRECT",
        _ => "UPMIX51",
    };
}
