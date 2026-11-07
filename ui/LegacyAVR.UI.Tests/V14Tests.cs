using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.Services;
using LegacyAVR.UI.ViewModels;
using Xunit;

namespace LegacyAVR.UI.Tests;

/// <summary>A fresh temporary folder per test, removed afterwards.</summary>
public sealed class TempFolder : IDisposable
{
    public string Path { get; } = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "legacyavr-test-" + Guid.NewGuid().ToString("N"));
    public TempFolder() => Directory.CreateDirectory(Path);
    public string File(string name) => System.IO.Path.Combine(Path, name);
    public void Dispose()
    {
        try { Directory.Delete(Path, recursive: true); } catch (Exception) { }
    }
}

internal static class Samples
{
    /// <summary>A complete profile as 1.4 saves it, with a non-flat EQ.</summary>
    public static Profile Full(string name = "Living room", string? deviceId = "dev-A", string? deviceName = "Denon AVR")
    {
        var eq = EqModel.Default();
        var ch0 = eq.Channels[0];
        var bands = ch0.Bands.ToArray();
        bands[2] = new EqBand(1200, -3.5, 1.4);
        var chans = eq.Channels.ToArray();
        chans[0] = ch0 with { Bands = bands };
        chans[5] = chans[5] with { Enabled = false };
        return new Profile
        {
            Name = name, DeviceId = deviceId, DeviceName = deviceName,
            Codec = CodecKind.DTS, Bitrate = 1509750, OutputMode = OutputModeKind.Exclusive, BufferMs = 20,
            Upmix = UpmixKind.Upmix31, StereoAs20 = false, NightMode = true, LfeGen = LfeGenKind.Always,
            Gaming = GamingKind.On, GameLfeGen = true, LfeResetSeconds = 45, LfeFold = false, BassDb = -2.5,
            CentreLevel = 0.7, CentreFloor = 0.2, DialogBoost = 0.5, SurroundLevel = 0.9, SurroundDelayMs = 15,
            DedicatedSub = false, LfeCrossoverHz = 100, LfeLevelDb = -6, MasterDb = -3,
            GainsDb = new[] { 0.0, -1.5, 2.0, -48.5, 0.5, -3.0 }, Mutes = new[] { false, false, true, false, false, true },
            Eq = new EqSettings(true, chans),
        };
    }
}

// ============================================================================ profiles
public class ProfileStoreTests
{
    [Fact]
    public void ExportImportRoundTripKeepsEverySetting()
    {
        using var tmp = new TempFolder();
        var p = Samples.Full();
        Assert.True(p.IsComplete);
        ProfileStore.Export(tmp.File("one.json"), new[] { p });
        var back = ProfileStore.ReadFile(tmp.File("one.json"));
        Assert.Single(back);
        Assert.True(back[0].IsComplete);
        Assert.True(back[0].SameSettings(p));
        Assert.Equal(-3.5, back[0].Eq!.Channels[0].Bands[2].GainDb);
        Assert.False(back[0].Eq!.Channels[5].Enabled);
        Assert.Equal("Denon AVR", back[0].DeviceName);
    }

    [Fact]
    public void StorePersistsAcrossLoads()
    {
        using var tmp = new TempFolder();
        var a = new ProfileStore(tmp.Path);
        a.Load(seedDefaults: false);
        a.Upsert(Samples.Full("Movies"));
        var b = new ProfileStore(tmp.Path);
        b.Load(seedDefaults: false);
        Assert.Single(b.Profiles);
        Assert.True(b.Find("movies")!.SameSettings(Samples.Full("Movies")));   // names are case-insensitive
    }

    [Fact]
    public void ImportReadsLegacyPresetFiles()
    {
        using var tmp = new TempFolder();
        // presets.json as written by LegacyAVR 1.1 - 1.3 (string enums, optional fields)
        System.IO.File.WriteAllText(tmp.File("presets.json"), """
        [
          { "Name": "Movies", "Codec": "AC3", "Bitrate": 640000, "OutputMode": "Exclusive", "Upmix": "Upmix51",
            "NightMode": false, "StereoAs20": true, "BufferMs": 0, "LfeGen": "Auto", "BassDb": 2 },
          { "Name": "Old", "Codec": "DTS", "Bitrate": 1509750, "OutputMode": "Shared", "Upmix": "Passthrough",
            "NightMode": true, "StereoAs20": false, "LfeResetSeconds": 600 }
        ]
        """);
        var list = ProfileStore.ReadFile(tmp.File("presets.json"));
        Assert.Equal(2, list.Count);
        Assert.Equal(CodecKind.AC3, list[0].Codec);
        Assert.Equal(2.0, list[0].BassDb);
        Assert.False(list[0].IsComplete);                  // partial: no speakers, trims or EQ
        Assert.False(list[0].HasDevice);                   // works on any output
        Assert.Null(list[0].GainsDb);
        Assert.Equal(300.0, list[1].LfeResetSeconds);      // 1.3 allowed 600 s; 1.4 allows 1..300
        // A partial profile leaves what it does not store alone.
        var cmds = ProfileStore.Commands(list[0], null);
        Assert.DoesNotContain(cmds, c => c.StartsWith("SET_GAIN") || c.StartsWith("SET_EQ") || c.StartsWith("SET_MASTER") || c.StartsWith("SET_SUB"));
        Assert.Contains("SET_CODEC AC3", cmds);
    }

    [Fact]
    public void ImportReadsLegacyDeviceProfiles()
    {
        using var tmp = new TempFolder();
        // device-profiles.json as written by LegacyAVR 1.3 (includes the computed "Display")
        System.IO.File.WriteAllText(tmp.File("device-profiles.json"), """
        [ { "Name": "Movies", "DeviceId": "{0.0.0.00000000}.{abc}", "DeviceName": "Denon AVR", "Codec": "EAC3",
            "Bitrate": 768000, "OutputMode": "Exclusive", "Upmix": "Upmix51", "StereoAs20": true, "BufferMs": 0,
            "NightMode": false, "LfeGen": "Auto", "Gaming": "Auto", "GameLfeGen": false, "LfeResetSeconds": 10,
            "BassDb": 0, "DedicatedSub": true, "LfeCrossoverHz": 80, "LfeLevelDb": -10, "CentreLevel": 0.85,
            "CentreFloor": 0.3, "DialogBoost": 0.6, "SurroundLevel": 0.8, "SurroundDelayMs": 12, "MasterDb": -2,
            "GainsDb": [0, 0, 1.5, 0, -2, -2], "Mutes": [false, false, false, false, false, false],
            "Display": "Movies  (Denon AVR)" } ]
        """);
        var p = ProfileStore.ReadFile(tmp.File("device-profiles.json")).Single();
        Assert.Equal("{0.0.0.00000000}.{abc}", p.DeviceId);
        Assert.Equal(1.5, p.GainsDb![2]);
        Assert.Null(p.Eq);                                  // 1.3 had no EQ: left unchanged when applied
        Assert.False(p.IsComplete);
    }

    [Fact]
    public void FirstRunMigratesPresetsAndDeviceProfilesAndKeepsTheOldFiles()
    {
        using var tmp = new TempFolder();
        System.IO.File.WriteAllText(tmp.File("presets.json"),
            """[ { "Name": "Movies", "Codec": "AC3", "Bitrate": 640000, "OutputMode": "Exclusive", "Upmix": "Upmix51", "NightMode": false, "StereoAs20": true } ]""");
        System.IO.File.WriteAllText(tmp.File("device-profiles.json"),
            """[ { "Name": "Movies", "DeviceId": "dev-A", "DeviceName": "Soundbar", "MasterDb": -4, "GainsDb": [0,0,0,0,0,0], "Mutes": [false,false,false,false,false,false] } ]""");
        var store = new ProfileStore(tmp.Path);
        store.Load(seedDefaults: true);
        Assert.Equal(2, store.Profiles.Count);              // nothing seeded: there was data to migrate
        Assert.NotNull(store.Find("Movies"));
        Assert.NotNull(store.Find("Movies (Soundbar)"));     // device profile renamed, nothing lost
        Assert.Contains("converted", store.MigrationNote);
        Assert.True(System.IO.File.Exists(tmp.File("profiles.json")));
        Assert.True(System.IO.File.Exists(tmp.File("presets.json")));            // left for older versions
        Assert.True(System.IO.File.Exists(tmp.File("device-profiles.json")));
        // Second start: read from profiles.json, no new migration.
        var again = new ProfileStore(tmp.Path);
        again.Load(seedDefaults: true);
        Assert.Equal(2, again.Profiles.Count);
        Assert.Equal(string.Empty, again.MigrationNote);
    }

    [Fact]
    public void NewInstallGetsTheStartingProfiles()
    {
        using var tmp = new TempFolder();
        var store = new ProfileStore(tmp.Path);
        store.Load(seedDefaults: true);
        Assert.Equal(ProfileStore.Defaults.Count, store.Profiles.Count);
        Assert.All(store.Profiles, p => Assert.False(p.HasDevice));
    }

    [Fact]
    public void ImportNeverOverwritesExistingProfiles()
    {
        using var tmp = new TempFolder();
        var store = new ProfileStore(tmp.Path, persist: false);
        store.Upsert(Samples.Full("Movies"));
        var r = store.Import(new[] { Samples.Full("Movies") with { MasterDb = -10 }, Samples.Full("Games") });
        Assert.Equal(2, r.Added);
        Assert.Equal(1, r.Renamed);
        Assert.Equal(-3.0, store.Find("Movies")!.MasterDb);
        Assert.Equal(-10.0, store.Find("Movies (2)")!.MasterDb);
        Assert.NotNull(store.Find("Games"));
    }

    [Fact]
    public void DamagedOrForeignFilesAreRejected()
    {
        Assert.ThrowsAny<Exception>(() => ProfileStore.Parse("not json"));
        Assert.ThrowsAny<Exception>(() => ProfileStore.Parse("""{ "something": 1 }"""));
        Assert.ThrowsAny<Exception>(() => ProfileStore.Parse("[]"));
    }

    [Fact]
    public void ValuesAreBroughtIntoRange()
    {
        var p = ProfileStore.Normalise(Samples.Full() with
        {
            LfeResetSeconds = 0, BassDb = 40, MasterDb = 99, BufferMs = 3, GainsDb = new[] { 99.0, 0, 0, 0, 0, double.NaN },
        });
        Assert.Equal(1.0, p.LfeResetSeconds);
        Assert.Equal(12.0, p.BassDb);
        Assert.Equal(12.0, p.MasterDb);
        Assert.Equal(0, p.BufferMs);
        Assert.Equal(6.0, p.GainsDb![0]);
        Assert.Equal(0.0, p.GainsDb![5]);
    }
}

public class ProfileApplyTests
{
    [Fact]
    public void CommandsCoverEverySetting()
    {
        var cmds = ProfileStore.Commands(Samples.Full(), "dev-A");
        Assert.StartsWith("SET_OUTPUT_DEVICE", cmds[0]);
        foreach (var verb in new[] { "SET_SUB 0", "SET_CODEC DTS", "SET_BITRATE 1509750", "SET_OUTPUT_MODE EXCLUSIVE", "SET_BUFFER_MS 20",
                                     "SET_UPMIX UPMIX31", "SET_STEREO_AS_20 0", "SET_NIGHT_MODE 1", "SET_LFE_GEN ALWAYS", "SET_GAMING ON",
                                     "SET_GAME_LFE 1", "SET_LFE_RESET 45", "SET_LFE_FOLD 0", "SET_BASS -2.5", "SET_UPMIX_PARAM LFE_XOVER 100",
                                     "SET_UPMIX_PARAM LFE_LEVEL -6", "SET_UPMIX_PARAM CENTRE 0.7", "SET_MASTER -3", "SET_GAIN 3 -48.5",
                                     "SET_MUTE 5 1", "SET_EQ_BAND 0 2 1200 -3.5 1.4", "SET_EQ_CHANNEL 5 0", "SET_EQ 1" })
            Assert.Contains(cmds, c => c.StartsWith(verb));
        Assert.Equal(EqModel.Channels * EqModel.Bands, cmds.Count(c => c.StartsWith("SET_EQ_BAND")));
    }

    [Fact]
    public void ApplyingToTheCurrentOutputNeverNeedsTheOriginalDevice()
    {
        // A profile imported from another PC: its device does not exist here.
        var p = Samples.Full(deviceId: "{other-pc}", deviceName: "Other AVR");
        Assert.Equal(ProfileTarget.OutputMissing, ProfileStore.TargetOf(p, "dev-B", new[] { "dev-B", "dev-C" }));
        var cmds = ProfileStore.Commands(p, switchToDeviceId: null);
        Assert.DoesNotContain(cmds, c => c.StartsWith("SET_OUTPUT_DEVICE"));   // stays on dev-B
        Assert.Contains("SET_CODEC DTS", cmds);
    }

    [Theory]
    [InlineData(null, "dev-A", ProfileTarget.AnyOutput)]
    [InlineData("dev-A", "dev-A", ProfileTarget.SameOutput)]
    [InlineData("DEV-A", "dev-a", ProfileTarget.SameOutput)]
    [InlineData("dev-B", "dev-A", ProfileTarget.OtherOutput)]
    [InlineData("dev-X", "dev-A", ProfileTarget.OutputMissing)]
    public void Targets(string? profileDevice, string current, ProfileTarget expected) =>
        Assert.Equal(expected, ProfileStore.TargetOf(Samples.Full(deviceId: profileDevice), current, new[] { "dev-A", "dev-B" }));

    [Fact]
    public void OnlyChangedSettingsAreSent()
    {
        var current = Samples.Full();
        Assert.Empty(ProfileStore.ChangedCommands(current, current, null));
        var louder = current with { MasterDb = 0 };
        Assert.Equal(new[] { "SET_MASTER 0" }, ProfileStore.ChangedCommands(louder, current, null));
        // SET_CODEC resets the engine's bitrate, so the bitrate is re-sent with it even if equal.
        var ac3 = current with { Codec = CodecKind.AC3, Bitrate = 1509750 };
        var changed = ProfileStore.ChangedCommands(ac3 with { Bitrate = 640000 }, current with { Bitrate = 640000 }, null);
        Assert.Contains("SET_CODEC AC3", changed);
        Assert.Contains("SET_BITRATE 640000", changed);
        // A device switch is always sent.
        Assert.StartsWith("SET_OUTPUT_DEVICE", ProfileStore.ChangedCommands(current, current, "dev-B")[0]);
    }
}

// ============================================================================ startup
public class StartupPolicyTests
{
    private static ProfileStore StoreWith(params Profile[] ps)
    {
        var s = new ProfileStore(Path.GetTempPath(), persist: false);
        foreach (var p in ps) s.Upsert(p);
        return s;
    }

    private static readonly string[] Avail = { "dev-A", "dev-B" };

    [Fact]
    public void LastProfileIsAppliedWhenItsOutputIsConnected()
    {
        var store = StoreWith(Samples.Full("Movies", "dev-A"), Samples.Full("Anywhere", null, null), Samples.Full("Bedroom", "dev-B"));
        var same = StartupPolicy.ResolveLastProfile(true, "Movies", store, "dev-A", Avail);
        Assert.Equal(StartupPolicy.ProfileAction.Apply, same.Action);
        Assert.Null(same.SwitchToId);
        var any = StartupPolicy.ResolveLastProfile(true, "Anywhere", store, "dev-A", Avail);
        Assert.Equal(StartupPolicy.ProfileAction.Apply, any.Action);
        Assert.Null(any.SwitchToId);
        var other = StartupPolicy.ResolveLastProfile(true, "Bedroom", store, "dev-A", Avail);
        Assert.Equal(StartupPolicy.ProfileAction.Apply, other.Action);
        Assert.Equal("dev-B", other.SwitchToId);           // switches to the profile's own output
    }

    [Fact]
    public void MissingOutputIsReportedNotReplaced()
    {
        var store = StoreWith(Samples.Full("Movies", "dev-X", "Denon AVR"));
        var d = StartupPolicy.ResolveLastProfile(true, "Movies", store, "dev-A", Avail);
        Assert.Equal(StartupPolicy.ProfileAction.Report, d.Action);
        Assert.Null(d.SwitchToId);
        Assert.Contains("Denon AVR", d.Message);
        Assert.Contains("not connected", d.Message);
    }

    [Fact]
    public void NothingHappensWhenDisabledOrUnknown()
    {
        var store = StoreWith(Samples.Full("Movies", "dev-A"));
        Assert.Equal(StartupPolicy.ProfileAction.None, StartupPolicy.ResolveLastProfile(false, "Movies", store, "dev-A", Avail).Action);
        Assert.Equal(StartupPolicy.ProfileAction.None, StartupPolicy.ResolveLastProfile(true, null, store, "dev-A", Avail).Action);
        var gone = StartupPolicy.ResolveLastProfile(true, "Deleted", store, "dev-A", Avail);
        Assert.Equal(StartupPolicy.ProfileAction.Report, gone.Action);
        Assert.Contains("no longer exists", gone.Message);
    }
}

public class OutputDefaultTests
{
    private static readonly OutputInfo[] Devices = { new("dev-A", "Denon AVR"), new("dev-B", "Speakers") };

    [Fact]
    public void ConnectedDefaultIsSelectedAtStartup()
    {
        var d = StartupPolicy.ResolveOutput("dev-B", Devices, "dev-A", "Denon AVR");
        Assert.Equal("dev-A", d.SwitchToId);
        Assert.Equal(string.Empty, d.Message);
        Assert.Null(StartupPolicy.ResolveOutput("dev-A", Devices, "dev-A", "Denon AVR").SwitchToId);   // already on it
    }

    [Fact]
    public void MissingDefaultKeepsTheCurrentOutputAndSaysSo()
    {
        var d = StartupPolicy.ResolveOutput("dev-B", Devices, "dev-X", "Old AVR");
        Assert.Null(d.SwitchToId);
        Assert.Null(d.MissingId);
        Assert.Contains("Old AVR", d.Message);
        Assert.Contains("Speakers", d.Message);
    }

    [Fact]
    public void SelectionIsNeverSilentlyBlank()
    {
        // Nothing configured at all
        var none = StartupPolicy.ResolveOutput("", Devices, null, null);
        Assert.Null(none.SwitchToId);
        Assert.Contains("No output device", none.Message);
        // Configured device unplugged: shown as a "(not connected)" entry, not replaced
        var gone = StartupPolicy.ResolveOutput("dev-X", Devices, null, null, "dev-X", "Old AVR");
        Assert.Null(gone.SwitchToId);
        Assert.Equal("dev-X", gone.MissingId);
        Assert.Equal("Old AVR", gone.MissingName);
        // Default and configured device both missing
        var both = StartupPolicy.ResolveOutput("", Devices, "dev-X", "Old AVR");
        Assert.Equal("dev-X", both.MissingId);
    }

    [Theory]
    [InlineData("dev-A", true, null, true)]       // no default yet
    [InlineData("dev-A", true, "dev-B", true)]    // another output is the default
    [InlineData("dev-A", true, "DEV-A", false)]   // already the default
    [InlineData("dev-X", false, null, false)]     // not connected: cannot become the default
    [InlineData(null, false, null, false)]
    public void DefaultButtonVisibility(string? selected, bool connected, string? def, bool visible) =>
        Assert.Equal(visible, StartupPolicy.ShowDefaultButton(selected, connected, def));

    [Fact]
    public void DefaultIsMarkedInTheList()
    {
        var d = new DeviceItem { Id = "dev-A", Name = "Denon AVR", IsRender = true, Channels = 2, SampleRate = 48000, IecCaps = 0x81 };
        Assert.DoesNotContain("(Default)", d.Display);
        Assert.Contains("Denon AVR (Default)", d.With(appDefault: true).Display);
        var w = new DeviceItem { Id = "x", Name = "CABLE Input", IsRender = true, IsDefault = true };
        Assert.Contains("Windows default", w.Display);   // Windows' default is labelled differently
        var missing = new DeviceItem { Id = "dev-X", Name = "Old AVR", IsRender = true, IsMissing = true, IsAppDefault = true };
        Assert.Equal("Old AVR (not connected) (Default)", missing.Display);
    }
}

// ============================================================================ no-subwoofer labels
public class NoSubwooferTests
{
    [Fact]
    public void DynamicModesAreShownAs30And50WithoutASubwoofer()
    {
        var withSub = MainViewModel.BuildUpmixOptions(true).Select(o => o.Label).ToList();
        var noSub = MainViewModel.BuildUpmixOptions(false).Select(o => o.Label).ToList();
        Assert.Contains("Dynamic 3.1 upmix", withSub);
        Assert.Contains("Dynamic 5.1 upmix", withSub);
        Assert.Contains("Dynamic 3.0 upmix", noSub);
        Assert.Contains("Dynamic 5.0 upmix", noSub);
        Assert.DoesNotContain(noSub, l => l.Contains(".1"));
        // Same modes in the same order - only the labels change.
        Assert.Equal(MainViewModel.BuildUpmixOptions(true).Select(o => o.Value), MainViewModel.BuildUpmixOptions(false).Select(o => o.Value));
    }

    [Fact]
    public void LfeSwitchIsGenerationWithASubAndFoldWithout()
    {
        // v1.5.0: with a subwoofer the switch generates an LFE; without one it never generates
        // and only decides whether the source's own LFE is folded into the fronts.
        var gen = MainViewModel.BuildLfeGenOptions();
        Assert.Equal(new[] { LfeGenKind.Auto, LfeGenKind.Always, LfeGenKind.Off }, gen.Select(o => o.Value));
        var fold = MainViewModel.BuildLfeFoldOptions();
        Assert.Equal(new[] { true, false }, fold.Select(o => o.Value));   // fold is the first (recommended) choice
        Assert.All(fold, o => Assert.DoesNotContain("generat", o.Label, StringComparison.OrdinalIgnoreCase));
        Assert.Contains("front", fold[0].Label);
        Assert.Contains("discard", fold[1].Label);
    }

    [Fact]
    public void NewInstallDefaults()
    {
        // v1.5.0: game mode off, LFE generation while gaming on, source LFE folded without a sub.
        Assert.Equal(GamingKind.Off, FactoryDefaults.Gaming);
        Assert.True(FactoryDefaults.GameLfeGen);
        Assert.True(FactoryDefaults.LfeFold);
    }

    [Fact]
    public void LfeFoldIsOptionalInProfiles()
    {
        // A 1.4 profile has no LfeFold: still complete, and applying it leaves the setting alone.
        var old = Samples.Full() with { LfeFold = null };
        Assert.True(old.IsComplete);
        Assert.DoesNotContain(ProfileStore.Commands(old, null), c => c.StartsWith("SET_LFE_FOLD"));
        Assert.Contains("SET_LFE_FOLD 1", ProfileStore.Commands(old with { LfeFold = true }, null));
    }

    [Fact]
    public void StartingProfilesLeaveGameModeAloneExceptTheOneThatNeedsIt()
    {
        foreach (var p in ProfileStore.Defaults)
        {
            if (p.Upmix == UpmixKind.PcmDirect) Assert.Equal(GamingKind.Auto, p.Gaming);   // relies on game mode
            else Assert.Null(p.Gaming);
        }
    }
}

// ============================================================================ resume after silence
public class LfeResetRangeTests
{
    [Theory]
    [InlineData(0.0, 1.0)]
    [InlineData(1.0, 1.0)]
    [InlineData(10.0, 10.0)]
    [InlineData(10.4, 10.0)]
    [InlineData(300.0, 300.0)]
    [InlineData(301.0, 300.0)]
    [InlineData(600.0, 300.0)]
    [InlineData(double.NaN, 10.0)]
    public void WholeSecondsFrom1To300(double input, double expected) =>
        Assert.Equal(expected, FactoryDefaults.ClampLfeReset(input));

    [Fact]
    public void DefaultIsTen()
    {
        Assert.Equal(10.0, FactoryDefaults.LfeResetSeconds);
        Assert.Equal(1.0, FactoryDefaults.LfeResetMinSeconds);
        Assert.Equal(300.0, FactoryDefaults.LfeResetMaxSeconds);
    }

    [Theory]
    [InlineData(1.0, "SET_LFE_RESET 1")]
    [InlineData(10.0, "SET_LFE_RESET 10")]
    [InlineData(300.0, "SET_LFE_RESET 300")]
    [InlineData(1000.0, "SET_LFE_RESET 300")]
    public void ProfilesSendTheClampedValue(double stored, string expected) =>
        Assert.Contains(expected, ProfileStore.Commands(new Profile { Name = "x", LfeResetSeconds = stored }, null));
}

// ============================================================================ custom EQ
public class EqModelTests
{
    [Fact]
    public void DefaultsAreFlat()
    {
        var d = EqModel.Default();
        Assert.False(d.Enabled);
        Assert.Equal(6, d.Channels.Length);
        Assert.All(d.Channels, c => { Assert.True(c.Enabled); Assert.True(EqModel.IsFlat(c)); Assert.Equal(5, c.Bands.Length); });
        Assert.All(d.Channels[EqModel.LfeChannel].Bands, b => Assert.InRange(b.FreqHz, 20, 200));
        foreach (var hz in new[] { 30.0, 1000.0, 15000.0 })
            Assert.Equal(0.0, EqModel.ResponseDb(d.Channels[0], hz), 6);
    }

    [Fact]
    public void LimitsAreEnforced()
    {
        var b = EqModel.Clamp(EqModel.LfeChannel, 2, new EqBand(5000, 12, 9));
        Assert.Equal(new EqBand(200, 6, 4), b);
        Assert.Equal(new EqBand(20, -6, 0.5), EqModel.Clamp(0, 1, new EqBand(1, -20, 0.01)));
        var n = EqModel.Normalise(new EqSettings(true, new[] { new EqChannel(false, new[] { new EqBand(100, 3, 1) }) }));
        Assert.Equal(6, n.Channels.Length);
        Assert.Equal(5, n.Channels[0].Bands.Length);
        Assert.False(n.Channels[0].Enabled);
        Assert.Equal(3.0, n.Channels[0].Bands[0].GainDb);
        Assert.True(n.Channels[1].Enabled);
    }

    [Fact]
    public void ResponseMatchesTheBandSettings()
    {
        var ch = EqModel.DefaultChannel(0);
        var bands = ch.Bands.ToArray();
        bands[2] = new EqBand(1000, 6, 1);
        Assert.Equal(6.0, EqModel.ResponseDb(ch with { Bands = bands }, 1000), 1);
        Assert.Equal(0.0, EqModel.ResponseDb(ch with { Bands = bands }, 20), 1);
    }

    [Fact]
    public void ParsesTheEngineFormat()
    {
        Assert.Equal(new EqBand(1000, -3.5, 1.4), EqModel.ParseBand("1000.0,-3.50,1.40"));
        Assert.Null(EqModel.ParseBand("1000,2"));
        Assert.Null(EqModel.ParseBand(null));
        Assert.Equal("SET_EQ_BAND 3 4 160 -2 1", EqModel.BandCommand(3, 4, new EqBand(160, -2, 1)));
    }
}

public class EqViewModelTests
{
    [Fact]
    public void StartsFlatAndSendsBandChanges()
    {
        var sent = new List<(int ch, int band)>();
        var vm = new ChannelEqViewModel(0, "Front Left", (c, b) => sent.Add((c.Index, b.Band)), _ => { });
        Assert.True(vm.IsFlat);
        Assert.Equal("EQ flat", vm.Summary);
        vm.Bands[2].GainDb = 4.26;                      // 0.5 dB steps
        Assert.Equal(4.5, vm.Bands[2].GainDb);
        vm.Bands[2].GainDb = 9;                         // +/-6 dB
        Assert.Equal(6.0, vm.Bands[2].GainDb);
        Assert.Equal(new[] { (0, 2), (0, 2) }, sent);
        Assert.False(vm.IsFlat);
    }

    [Fact]
    public void FrequencySliderIsLogarithmicAndLfeIsLimited()
    {
        var lfe = new ChannelEqViewModel(3, "LFE", (_, _) => { }, _ => { });
        Assert.Equal(Math.Log10(200), lfe.Bands[0].FreqLogMax, 6);
        lfe.Bands[0].FreqLog = Math.Log10(5000);
        Assert.Equal(200.0, lfe.Bands[0].FreqHz);
        var fl = new ChannelEqViewModel(0, "FL", (_, _) => { }, _ => { });
        fl.Bands[2].FreqLog = 3.0;
        Assert.Equal(1000.0, fl.Bands[2].FreqHz);
        Assert.False(fl.Bands[0].HasQ);                 // shelves have no Q
        Assert.True(fl.Bands[2].HasQ);
    }

    [Fact]
    public void EngineSyncDoesNotEchoAndBypassKeepsSettings()
    {
        int sends = 0, enables = 0;
        var vm = new ChannelEqViewModel(0, "FL", (_, _) => sends++, _ => enables++);
        vm.Bands[1].SetFromModel(new EqBand(250, -3, 2));
        vm.SetEnabledFromEngine(false);
        Assert.Equal(0, sends);
        Assert.Equal(0, enables);
        Assert.Equal(-3.0, vm.Bands[1].GainDb);
        Assert.Equal("EQ bypassed for this channel", vm.Summary);
        vm.Enabled = true;                               // user re-enables: settings are still there
        Assert.Equal(1, enables);
        Assert.Equal(-3.0, vm.ToModel().Bands[1].GainDb);
    }
}

// ============================================================================ resets
public class SettingResetTests
{
    private sealed class Fake
    {
        public double Centre = 0.5, Delay = 20;
    }

    private static (SettingResets, Fake) Make()
    {
        var f = new Fake();
        var r = new SettingResets();
        r.Add("centre", "Centre steering", () => "85%", () => f.Centre == FactoryDefaults.CentreLevel, () => f.Centre = FactoryDefaults.CentreLevel);
        r.Add("delay", "Surround delay", () => "12 ms", () => f.Delay == FactoryDefaults.SurroundDelayMs, () => f.Delay = FactoryDefaults.SurroundDelayMs);
        return (r, f);
    }

    [Fact]
    public void ResetsOnlyThatSettingAfterConfirmation()
    {
        var (r, f) = Make();
        string? asked = null;
        Assert.True(r.Reset("centre", q => { asked = q; return true; }));
        Assert.Equal("Reset Centre steering to its default (85%)?", asked);
        Assert.Equal(0.85, f.Centre);
        Assert.Equal(20.0, f.Delay);                     // untouched
    }

    [Fact]
    public void DecliningChangesNothing()
    {
        var (r, f) = Make();
        Assert.False(r.Reset("delay", _ => false));
        Assert.Equal(20.0, f.Delay);
    }

    [Fact]
    public void NoQuestionWhenAlreadyDefaultOrUnknown()
    {
        var (r, f) = Make();
        f.Centre = FactoryDefaults.CentreLevel;
        bool asked = false;
        Assert.False(r.Reset("centre", _ => asked = true));
        Assert.False(r.Reset("everything", _ => asked = true));   // there is no reset-all key
        Assert.False(asked);
    }

    [Fact]
    public void FactoryDefaultsMatchTheEngine()
    {
        // engine/core/LiveParams.h and engine/win/ConfigStore.h
        Assert.Equal(0.85, FactoryDefaults.CentreLevel);
        Assert.Equal(0.30, FactoryDefaults.CentreFloor);
        Assert.Equal(0.6, FactoryDefaults.DialogBoost);
        Assert.Equal(0.8, FactoryDefaults.SurroundLevel);
        Assert.Equal(12.0, FactoryDefaults.SurroundDelayMs);
        Assert.Equal(80.0, FactoryDefaults.LfeCrossoverHz);
        Assert.Equal(-10.0, FactoryDefaults.LfeLevelDb);
        Assert.Equal(0.0, FactoryDefaults.BassDb);
        Assert.Equal(0.0, FactoryDefaults.MasterDb);
    }
}
