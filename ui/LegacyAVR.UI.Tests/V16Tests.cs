using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.Services;
using LegacyAVR.UI.ViewModels;
using Xunit;

namespace LegacyAVR.UI.Tests;

// ============================================================================ v1.6.0

/// <summary>Finds files of the source tree (the tests run from a build folder inside it).</summary>
internal static class RepoFiles
{
    public static string Root
    {
        get
        {
            for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
                if (File.Exists(Path.Combine(dir.FullName, "ui", "LegacyAVR.UI", "MainWindow.xaml")))
                    return dir.FullName;
            throw new InvalidOperationException("Run the tests from inside the LegacyAVR source tree (MainWindow.xaml not found).");
        }
    }

    public static string Read(params string[] parts) => File.ReadAllText(Path.Combine(new[] { Root }.Concat(parts).ToArray()));
}

public class ExperimentalDefaultsTests
{
    [Fact]
    public void GatesAreOffAndTheChimeIsOnForNewInstalls()
    {
        Assert.False(FactoryDefaults.ExpLfeGen);
        Assert.False(FactoryDefaults.ExpLfeFold);
        Assert.True(FactoryDefaults.ActivationChime);
        Assert.True(new UiSettings().ActivationChime);
    }

    [Fact]
    public void ChimeSettingSurvivesUpgradesAndIsPersisted()
    {
        // A ui-settings.json written by 1.5 has no ActivationChime key: the chime is on.
        var old = JsonSerializer.Deserialize<UiSettings>("{\"StopEngineOnExit\": false}", SettingsService.Json)!;
        Assert.True(old.ActivationChime);
        Assert.False(old.StopEngineOnExit);
        var off = new UiSettings { ActivationChime = false };
        var back = JsonSerializer.Deserialize<UiSettings>(JsonSerializer.Serialize(off, SettingsService.Json), SettingsService.Json)!;
        Assert.False(back.ActivationChime);
    }

    [Fact]
    public void ProfilesNeverSwitchTheExperimentalGates()
    {
        // The gates are machine settings: applying any profile keeps them as they are, while the
        // LFE settings they guard are still stored in and applied from profiles (kept, not lost).
        var full = Samples.Full();
        var cmds = ProfileStore.Commands(full, "dev-A");
        Assert.DoesNotContain(cmds, c => c.StartsWith("SET_EXP", StringComparison.Ordinal));
        Assert.Contains(cmds, c => c.StartsWith("SET_LFE_GEN", StringComparison.Ordinal));
        Assert.Contains(cmds, c => c.StartsWith("SET_LFE_FOLD", StringComparison.Ordinal));
        Assert.Contains(cmds, c => c.StartsWith("SET_GAME_LFE", StringComparison.Ordinal));
        var changed = ProfileStore.ChangedCommands(full, Samples.Full(name: "Other") with { LfeFold = !full.LfeFold }, "dev-A");
        Assert.DoesNotContain(changed, c => c.StartsWith("SET_EXP", StringComparison.Ordinal));
    }

    [Fact]
    public void FoldChoicesNeverDiscardTheSourceLfe()
    {
        var fold = MainViewModel.BuildLfeFoldOptions();
        Assert.Equal(new[] { true, false }, fold.Select(o => o.Value));
        Assert.Contains("front L/R", fold[0].Label);
        Assert.Contains("pass through", fold[1].Label);
        Assert.All(fold, o => Assert.DoesNotContain("discard", o.Label, StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void TelemetryMirrorsTheEngine()
    {
        // engine/ipc/TelemetryLayout.h and engine/core/Types.h (additions only: layout v5 is unchanged)
        Assert.Equal(1u << 11, (uint)TelemetryFlags2.ExpLfeGen);
        Assert.Equal(1u << 12, (uint)TelemetryFlags2.ExpLfeFold);
        Assert.Equal(1u << 13, (uint)TelemetryFlags2.LfeFolded);
        Assert.Equal(1u << 14, (uint)TelemetryFlags2.ChimePlaying);
        Assert.Equal(7u, (uint)LfeStatusKind.Folded);
        Assert.Equal(5u, TelemetryBlock.ExpectedVersion);
        var layout = RepoFiles.Read("engine", "ipc", "TelemetryLayout.h");
        Assert.Matches(@"TF2_ExpLfeGen\s*=\s*1u\s*<<\s*11", layout);
        Assert.Matches(@"TF2_ExpLfeFold\s*=\s*1u\s*<<\s*12", layout);
        Assert.Matches(@"TF2_LfeFolded\s*=\s*1u\s*<<\s*13", layout);
        Assert.Matches(@"TF2_ChimePlaying\s*=\s*1u\s*<<\s*14", layout);
        Assert.Matches(@"Folded\s*=\s*7", RepoFiles.Read("engine", "core", "Types.h"));
    }
}

public class ChimeSchedulerTests
{
    private static readonly DateTime T0 = new(2026, 11, 11, 12, 0, 0, DateTimeKind.Utc);
    private static DateTime At(double seconds) => T0.AddSeconds(seconds);

    /// <summary>Runs ticks every 100 ms from <paramref name="from"/> to <paramref name="to"/> (s) and counts cues.</summary>
    private static int Run(ChimeScheduler s, double from, double to, bool healthy = true, bool ready = true, bool error = false)
    {
        int cues = 0;
        for (double t = from; t <= to + 1e-9; t += 0.1)
            if (s.Tick(healthy, ready, error, At(t))) cues++;
        return cues;
    }

    [Fact]
    public void StartupCuePlaysOnceAfterThePipelineHasSettled()
    {
        var s = new ChimeScheduler();
        Assert.False(s.Tick(true, true, false, At(0)));
        Assert.False(s.Tick(true, true, false, At(0.3)));             // not settled yet
        Assert.True(s.Tick(true, true, false, At(0.7)));
        Assert.True(s.StartupHandled);
        Assert.Equal(0, Run(s, 0.8, 30));                             // never again
    }

    [Fact]
    public void StartupCueWaitsForTheDashboardAndForRunningAudio()
    {
        var s = new ChimeScheduler();
        Assert.Equal(0, Run(s, 0, 3, healthy: true, ready: false));   // still starting / applying a profile
        Assert.Equal(0, Run(s, 3.1, 5, healthy: false));              // pipeline not running
        Assert.Equal(1, Run(s, 5.1, 8));
    }

    [Fact]
    public void StartupCueIsSkippedIfAudioNeverStartsDuringStartup()
    {
        var s = new ChimeScheduler();
        Assert.Equal(0, Run(s, 0, 31, healthy: false));               // stopped / no device for 30 s
        Assert.True(s.StartupHandled);
        Assert.Equal(0, Run(s, 31.1, 40));                            // starting audio later plays nothing
    }

    [Fact]
    public void InputChangeCuePlaysOnceAfterTheRestart()
    {
        var s = new ChimeScheduler();
        Assert.Equal(1, Run(s, 0, 2));                                // startup cue
        s.InputChangeAccepted(At(10));
        Assert.True(s.InputPending);
        Assert.Equal(0, Run(s, 10.1, 10.5, healthy: false));          // the engine re-opens the pipeline
        Assert.False(s.Tick(true, true, false, At(10.6)));            // back up, settling
        Assert.Equal(1, Run(s, 10.7, 12));
        Assert.False(s.InputPending);
        Assert.Equal(0, Run(s, 12.1, 20));
    }

    [Fact]
    public void InputChangeCuePlaysWhenTheRestartWasTooQuickToSee()
    {
        var s = new ChimeScheduler();
        Run(s, 0, 2);
        s.InputChangeAccepted(At(10));
        Assert.Equal(0, Run(s, 10.1, 11.4));                          // waits NoRestartWait (1.5 s)
        Assert.Equal(1, Run(s, 11.5, 12.5));
    }

    [Fact]
    public void FailedSelectionErrorAndTimeoutPlayNothing()
    {
        var s = new ChimeScheduler();
        Run(s, 0, 2);
        s.InputChangeAccepted(At(10));
        s.InputChangeFailed();                                        // the engine refused it
        Assert.Equal(0, Run(s, 10, 15));

        s.InputChangeAccepted(At(20));
        Assert.False(s.Tick(false, true, true, At(20.2)));            // error state
        Assert.False(s.InputPending);
        Assert.Equal(0, Run(s, 20.3, 25));

        s.InputChangeAccepted(At(30));
        Assert.Equal(0, Run(s, 30.1, 45.5, healthy: false));          // never came up (15 s)
        Assert.False(s.InputPending);
        Assert.Equal(0, Run(s, 45.6, 50));
    }

    [Fact]
    public void AnInputChangeDuringStartupGivesOneCueNotTwo()
    {
        var s = new ChimeScheduler();
        s.Tick(true, true, false, At(0));
        s.InputChangeAccepted(At(0.1));
        Assert.Equal(1, Run(s, 0.2, 5));
        Assert.False(s.InputPending);
    }

    [Fact]
    public void DisabledPlaysNothingAndDoesNotSaveCuesForLater()
    {
        var s = new ChimeScheduler { Enabled = false };
        Assert.Equal(0, Run(s, 0, 2));
        Assert.True(s.StartupHandled);
        s.InputChangeAccepted(At(10));
        Assert.Equal(0, Run(s, 10, 13));
        s.Enabled = true;
        Assert.Equal(0, Run(s, 13.1, 20));                            // nothing queued up
        s.InputChangeAccepted(At(30));
        Assert.Equal(1, Run(s, 30, 33));                              // a new change cues again
    }
}

public class EqEntryTests
{
    [Fact]
    public void QIsShownWithTwoDecimals()
    {
        var r = EqEntry.ParseQ("1.3");
        Assert.True(r.Ok);
        Assert.Equal(1.3, r.Value, 9);
        Assert.Null(r.Note);                                          // on a step: no rounding note
        Assert.Equal("Q 1.30", EqEntry.FormatQ(r.Value));
        Assert.True(EqEntry.ParseQ("Q 1.3").Ok);
        Assert.True(EqEntry.ParseQ("q=2").Ok);
        Assert.Equal(1.5, EqEntry.ParseQ("1,5").Value, 9);            // decimal comma
    }

    [Fact]
    public void OffStepValuesAreRoundedAndTheRoundingIsReported()
    {
        var q = EqEntry.ParseQ("1.33");
        Assert.True(q.Ok);
        Assert.Equal(1.35, q.Value, 9);
        Assert.Contains("Q 1.35", q.Note);
        var f = EqEntry.ParseFreq("1234", 0);
        Assert.True(f.Ok);
        Assert.Equal(1250, f.Value, 9);
        Assert.Contains("1.25 kHz", f.Note);
        var g = EqEntry.ParseGain("-2.4");
        Assert.Equal(-2.5, g.Value, 9);
        Assert.NotNull(g.Note);
    }

    [Theory]
    [InlineData("120", 0, 120)]
    [InlineData("120 Hz", 0, 120)]
    [InlineData("1.2k", 0, 1200)]
    [InlineData("1.2 kHz", 0, 1200)]
    [InlineData("20000", 0, 20000)]
    [InlineData("200", 3, 200)]
    [InlineData("20", 3, 20)]
    public void FrequencyFormats(string text, int channel, double hz)
    {
        var r = EqEntry.ParseFreq(text, channel);
        Assert.True(r.Ok, r.Error);
        Assert.Equal(hz, r.Value, 9);
    }

    [Theory]
    [InlineData("")]
    [InlineData("abc")]
    [InlineData("10")]          // below 20 Hz
    [InlineData("20001")]
    [InlineData("1e3")]
    [InlineData("NaN")]
    [InlineData("-100")]
    public void InvalidFrequenciesAreRefusedNotClamped(string text)
    {
        var r = EqEntry.ParseFreq(text, 0);
        Assert.False(r.Ok);
        Assert.False(string.IsNullOrEmpty(r.Error));
    }

    [Fact]
    public void RangesMatchTheSliders()
    {
        Assert.False(EqEntry.ParseFreq("250", EqModel.LfeChannel).Ok);       // LFE bands stop at 200 Hz
        Assert.Contains("LFE", EqEntry.ParseFreq("250", EqModel.LfeChannel).Error);
        Assert.False(EqEntry.ParseGain("6.5").Ok);
        Assert.False(EqEntry.ParseGain("-7 dB").Ok);
        Assert.True(EqEntry.ParseGain("+6 dB").Ok);
        Assert.False(EqEntry.ParseQ("0.4").Ok);
        Assert.False(EqEntry.ParseQ("4.5").Ok);
        Assert.True(EqEntry.ParseQ("4").Ok);
        Assert.Equal("+3.0 dB", EqEntry.FormatGain(3));
        Assert.Equal("1.2 kHz", EqEntry.FormatFreq(1200));
        Assert.Equal("80 Hz", EqEntry.FormatFreq(80));
    }
}

public class EqBandEditTests
{
    private static (ChannelEqViewModel ch, List<int> changes) Channel(int index = 0)
    {
        var changes = new List<int>();
        var ch = new ChannelEqViewModel(index, $"Channel {index}", (_, b) => changes.Add(b.Band), _ => { });
        return (ch, changes);
    }

    [Fact]
    public void ValidEntryUpdatesTheSettingAndShowsTheFormattedValue()
    {
        var (ch, changes) = Channel();
        var b = ch.Bands[2];
        b.QEntry = "1.3";
        Assert.Equal(1.3, b.Q, 9);
        Assert.Equal("Q 1.30", b.QEntry);
        Assert.False(b.HasEditMessage);
        Assert.Single(changes);                                       // sent to the engine / saved once
        b.FreqEntry = "1.2k";
        Assert.Equal(1200, b.FreqHz, 9);
        Assert.Equal("1.2 kHz", b.FreqEntry);
        Assert.Equal(Math.Log10(1200), b.FreqLog, 9);                 // the slider follows
    }

    [Fact]
    public void InvalidEntryKeepsTheLastValidValueAndSaysWhy()
    {
        var (ch, changes) = Channel();
        var b = ch.Bands[2];
        b.QEntry = "2";
        changes.Clear();
        b.QEntry = "9";
        Assert.Equal(2.0, b.Q, 9);                                    // not clamped to 4
        Assert.True(b.QInvalid);
        Assert.True(b.EditIsError);
        Assert.Contains("Q must be", b.EditMessage);
        Assert.Equal("Q 2.00", b.QEntry);                             // the box shows the valid value again
        Assert.Empty(changes);
        b.Q = 1.5;                                                    // moving the slider clears it
        Assert.False(b.QInvalid);
        Assert.False(b.HasEditMessage);
        Assert.Equal("Q 1.50", b.QEntry);
    }

    [Fact]
    public void AnErrorAboutOneFieldIsNotHiddenByChangesToAnother()
    {
        var (ch, _) = Channel();
        var b = ch.Bands[1];
        b.GainEntry = "loud";
        Assert.True(b.GainInvalid);
        b.Q = 2;
        Assert.True(b.GainInvalid);
        b.GainDb = 1;
        Assert.False(b.GainInvalid);
    }

    [Fact]
    public void RoundedEntryIsAppliedWithANote()
    {
        var (ch, _) = Channel();
        var b = ch.Bands[2];
        b.QEntry = "1.33";
        Assert.Equal(1.35, b.Q, 9);
        Assert.True(b.HasEditMessage);
        Assert.False(b.EditIsError);
        Assert.False(b.QInvalid);
    }

    [Fact]
    public void BoxesFollowProfilesAndResets()
    {
        var (ch, changes) = Channel();
        var b = ch.Bands[2];
        b.SetFromModel(new EqBand(1000, 3, 2));
        Assert.Equal("1 kHz", b.FreqEntry);
        Assert.Equal("+3.0 dB", b.GainEntry);
        Assert.Equal("Q 2.00", b.QEntry);
        Assert.Empty(changes);                                        // values from the engine / a profile are not echoed
        b.SetFromModel(EqModel.DefaultBand(0, 2));
        Assert.True(b.IsDefault);
        Assert.Equal(EqEntry.FormatQ(EqModel.QDefault), b.QEntry);
    }

    [Fact]
    public void LfeChannelRefusesFrequenciesAboveItsRange()
    {
        var (ch, _) = Channel(EqModel.LfeChannel);
        var b = ch.Bands[1];
        var before = b.FreqHz;
        b.FreqEntry = "500";
        Assert.Equal(before, b.FreqHz, 9);
        Assert.True(b.FreqInvalid);
        Assert.Contains("200 Hz", b.EditMessage);
    }
}

public class EqBandCopyTests
{
    private static readonly string[] Names = { "Front left", "Front right", "Centre", "Subwoofer (LFE)", "Surround left", "Surround right" };

    private static (ChannelEqViewModel[] chans, List<(int ch, int band)> changes) Channels()
    {
        var changes = new List<(int, int)>();
        var chans = Enumerable.Range(0, EqModel.Channels)
            .Select(i => new ChannelEqViewModel(i, Names[i], (c, b) => changes.Add((c.Index, b.Band)), _ => { }))
            .ToArray();
        return (chans, changes);
    }

    private static EqBand[] Snapshot(ChannelEqViewModel ch) => ch.Bands.Select(b => b.ToModel()).ToArray();

    [Fact]
    public void DialogNamesTheSourceAndChecksEveryApplicableChannel()
    {
        var (chans, _) = Channels();
        var src = chans[0].Bands[2];
        src.FreqHz = 1000; src.GainDb = 3; src.Q = 1.4;
        var vm = new EqBandCopyViewModel(chans[0], src, chans, _ => true);
        Assert.Contains("Bell 2", vm.SourceText);
        Assert.Contains("Front left", vm.SourceText);
        Assert.Contains("Q 1.40", vm.SourceText);
        Assert.Equal(5, vm.Targets.Length);                           // every channel but the source
        Assert.DoesNotContain(vm.Targets, t => t.Channel.Index == 0);
        var lfe = vm.Targets.Single(t => t.Channel.Index == EqModel.LfeChannel);
        Assert.False(lfe.IsApplicable);                               // 1 kHz is above the LFE's 200 Hz
        Assert.False(lfe.IsChecked);
        Assert.Contains("200 Hz", lfe.Note);
        lfe.IsChecked = true;
        Assert.False(lfe.IsChecked);
        Assert.All(vm.Targets.Where(t => t.IsApplicable), t => Assert.True(t.IsChecked));
        Assert.True(vm.CanApply);
    }

    [Fact]
    public void ApplyCopiesFrequencyGainAndQAndLeavesEverythingElse()
    {
        var (chans, changes) = Channels();
        var src = chans[0].Bands[2];
        src.FreqHz = 1000; src.GainDb = 3; src.Q = 1.4;
        var before = chans.Select(Snapshot).ToArray();
        changes.Clear();
        string? asked = null;
        var vm = new EqBandCopyViewModel(chans[0], src, chans, m => { asked = m; return true; });
        bool applied = false;
        vm.Applied += (_, _) => applied = true;
        Assert.True(vm.ApplyCommand.CanExecute(null));
        vm.ApplyCommand.Execute(null);
        Assert.True(applied);
        Assert.NotNull(asked);                                        // asked before overwriting
        Assert.Contains("Overwrite Bell 2", asked);
        Assert.Contains("Front right, Centre, Surround left and Surround right", asked);
        foreach (var i in new[] { 1, 2, 4, 5 })
        {
            Assert.Equal(new EqBand(1000, 3, 1.4), chans[i].Bands[2].ToModel());
            for (int band = 0; band < EqModel.Bands; band++)
                if (band != 2) Assert.Equal(before[i][band], chans[i].Bands[band].ToModel());   // other bands kept
            Assert.Contains((i, 2), changes);                          // sent / saved like any EQ change
        }
        Assert.Equal(before[EqModel.LfeChannel], Snapshot(chans[EqModel.LfeChannel]));      // not applicable
        Assert.Equal(before[0], Snapshot(chans[0]));                                         // source unchanged
        Assert.DoesNotContain(changes, c => c.ch == 0 || c.ch == EqModel.LfeChannel);
    }

    [Fact]
    public void DecliningTheConfirmationChangesNothing()
    {
        var (chans, changes) = Channels();
        chans[2].Bands[1].GainDb = -4;
        var before = chans.Select(Snapshot).ToArray();
        changes.Clear();
        var vm = new EqBandCopyViewModel(chans[2], chans[2].Bands[1], chans, _ => false);
        bool applied = false;
        vm.Applied += (_, _) => applied = true;
        vm.ApplyCommand.Execute(null);
        Assert.False(applied);
        Assert.Equal(before, chans.Select(Snapshot).ToArray());
        Assert.Empty(changes);
    }

    [Fact]
    public void EmptySelectionCannotBeApplied()
    {
        var (chans, changes) = Channels();
        int asked = 0;
        var vm = new EqBandCopyViewModel(chans[0], chans[0].Bands[1], chans, _ => { asked++; return true; });
        foreach (var t in vm.Targets) t.IsChecked = false;
        Assert.False(vm.CanApply);
        Assert.False(string.IsNullOrEmpty(vm.SelectionText));
        Assert.False(vm.ApplyCommand.CanExecute(null));
        Assert.False(vm.Apply());
        Assert.Equal(0, asked);
        Assert.Empty(changes);
        vm.Targets[0].IsChecked = true;
        Assert.True(vm.CanApply);
        Assert.Equal(string.Empty, vm.SelectionText);
    }

    [Fact]
    public void ShelvesCopyFrequencyAndGainAndKeepTheDestinationQ()
    {
        var (chans, _) = Channels();
        var src = chans[0].Bands[0];
        src.FreqHz = 100; src.GainDb = -2; src.Q = 0.7;
        chans[1].Bands[0].Q = 2;
        var vm = new EqBandCopyViewModel(chans[0], src, chans, _ => true);
        Assert.Contains("frequency and gain", vm.ScopeText);
        Assert.DoesNotContain("Q ", vm.SourceText);
        Assert.True(vm.Apply());
        Assert.Equal(new EqBand(100, -2, 2), chans[1].Bands[0].ToModel());
        Assert.Equal(100, chans[EqModel.LfeChannel].Bands[0].FreqHz, 9);   // 100 Hz fits the LFE channel
    }
}

public class TabOrderTests
{
    [Fact]
    public void TabsAreInTheRequiredOrder()
    {
        var doc = XDocument.Parse(RepoFiles.Read("ui", "LegacyAVR.UI", "MainWindow.xaml"));
        XNamespace p = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
        var headers = doc.Descendants(p + "TabItem").Select(t => (string?)t.Attribute("Header")).ToArray();
        Assert.Equal(new[]
        {
            "Mixer", "Output & Profiles", "Upmixing & Audio Settings", "Diagnostics",
            "Engine Settings", "Settings Guide", "Setup Guide",
        }, headers);
    }

    [Fact]
    public void SettingsGuideControlsExist()
    {
        // Every Settings Guide entry with an inline control has its template in MainWindow.xaml.
        var xaml = RepoFiles.Read("ui", "LegacyAVR.UI", "MainWindow.xaml");
        foreach (var e in HelpContent.Entries.Where(e => e.HasControl))
            Assert.True(xaml.Contains($"x:Key=\"HelpCtl.{e.Control}\"", StringComparison.Ordinal), $"HelpCtl.{e.Control} ({e.Title})");
        var titles = HelpContent.Entries.Select(e => e.Title).ToArray();
        foreach (var t in new[] { "Activation chime", "EQ: copy a band to other channels", "EQ: typing exact values",
                                  "Experimental features (LFE generation, LFE fold)", "LFE without a subwoofer (pass-through and fold)",
                                  "VB-CABLE: download and 5.1 set-up" })
            Assert.Contains(t, titles);
        Assert.DoesNotContain(HelpContent.Entries, e => e.WhatItDoes.Contains("Upmixing & Bass tab", StringComparison.Ordinal));
    }
}

public class DocumentationTests
{
    private const string Dd71Note = "DD+ 7.1 encoding is being investigated but is not currently available";

    public static IEnumerable<object[]> Readmes() => new[]
    {
        new object[] { new[] { "README.md" } },
        new object[] { new[] { "Github Releases", "README.md" } },
    };

    [Theory]
    [MemberData(nameof(Readmes))]
    public void ReadmeRecommendsA51Cable(string[] path)
    {
        var text = RepoFiles.Read(path);
        Assert.Contains(Dd71Note, text);
        Assert.Contains("5.1 Surround", text);
        Assert.Contains("https://vb-audio.com/Cable/", text);
        Assert.DoesNotMatch(new Regex(@"(choose|select|configure[^.\n]*as)\s+\**7\.1 Surround", RegexOptions.IgnoreCase), text);
        Assert.Matches(new Regex(@"7\.1[^\n]*(folded|fold-down)", RegexOptions.IgnoreCase), text);   // still explains the fold-down
        Assert.Contains("Upmixing & Audio Settings", text);
        Assert.DoesNotContain("Upmixing & Bass", text);
    }

    [Fact]
    public void InAppGuidesRecommendA51Cable()
    {
        var xaml = RepoFiles.Read("ui", "LegacyAVR.UI", "MainWindow.xaml");
        Assert.Contains("choose 5.1 Surround", xaml);
        Assert.DoesNotContain("choose 7.1 Surround", xaml);
        Assert.Contains(Dd71Note, xaml);
        Assert.Contains("OpenVbCableCommand", xaml);
        Assert.Equal("https://vb-audio.com/Cable/", MainViewModel.VbCableUrl);
        Assert.Contains(HelpContent.Entries, e => (e.WhatItDoes + e.Tradeoffs).Contains(Dd71Note, StringComparison.Ordinal));
        Assert.DoesNotContain(HelpContent.Entries, e => e.StartingPoint.Contains("7.1 Surround", StringComparison.Ordinal));

        var iss = RepoFiles.Read("scripts", "installer.iss");
        Assert.Contains("5.1 Surround", iss);
        Assert.DoesNotContain("as 7.1 Surround", iss);
        Assert.All(iss, c => Assert.True(c < 128, "installer.iss must stay ASCII"));
    }

    [Fact]
    public void ReleaseNotesDescribeThisVersion()
    {
        var notes = RepoFiles.Read("Github Releases", "RELEASE_NOTES.md");
        Assert.Contains("1.6.0", notes);
        Assert.Contains(Dd71Note, notes);
        Assert.Contains("experimental", notes, StringComparison.OrdinalIgnoreCase);
    }
}

public class VersionTests
{
    [Fact]
    public void EngineAndDashboardVersionsMatch()
    {
        // engine/core/Version.h stamps the experimental gates in engine.cfg; it must move with the
        // product version, or an upgrade would not switch the gates off.
        var v = Regex.Match(RepoFiles.Read("engine", "core", "Version.h"), "kVersionString = \"([0-9.]+)\"").Groups[1].Value;
        Assert.Matches(@"^\d+\.\d+\.\d+$", v);
        Assert.Contains($"<Version>{v}</Version>", RepoFiles.Read("ui", "LegacyAVR.UI", "LegacyAVR.UI.csproj"));
        Assert.Contains($"project(LegacyAVR VERSION {v} ", RepoFiles.Read("CMakeLists.txt"));
        Assert.Contains($"\"version-string\": \"{v}\"", RepoFiles.Read("vcpkg.json"));
        Assert.Contains($"version=\"{v}.0\"", RepoFiles.Read("ui", "LegacyAVR.UI", "app.manifest"));
        var rc = RepoFiles.Read("engine", "app", "AVREngine", "AVREngine.rc");
        Assert.Contains($"FILEVERSION {v.Replace('.', ',')},0", rc);
        Assert.Contains($"\"FileVersion\", \"{v}.0\"", rc);
    }
}
