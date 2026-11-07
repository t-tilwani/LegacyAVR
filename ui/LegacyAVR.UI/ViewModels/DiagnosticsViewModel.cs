using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Input;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.Services;

namespace LegacyAVR.UI.ViewModels;

public sealed class DiagnosticStep
{
    public required string Title { get; init; }
    public required string Instructions { get; init; }
    public required string ExpectedResult { get; init; }
    public required TestSignalKind Signal { get; init; }
    public override string ToString() => Title;
}

public sealed class SpeakerToggle : ObservableObject
{
    private bool _enabled = true;
    private string _label;
    public SpeakerToggle(int index, string name) { Index = index; Name = name; _label = name; }
    public int Index { get; }
    public string Name { get; }
    /// <summary>Text on the chip (the LFE chip is relabelled without a subwoofer).</summary>
    public string Label { get => _label; set => Set(ref _label, value); }
    public bool Enabled { get => _enabled; set => Set(ref _enabled, value); }
}

/// <summary>Audio validation wizard: pink noise, channel ID, centre bleed, LFE isolation, phase.</summary>
public sealed class DiagnosticsViewModel : ObservableObject
{
    private readonly EngineClient _client;
    private int _stepIndex;
    private TestSignalKind _active = TestSignalKind.Off;
    private string _liveStatus = "No test running.";

    public DiagnosticsViewModel(EngineClient client)
    {
        _client = client;
        Steps = new ObservableCollection<DiagnosticStep>
        {
            new()
            {
                Title = "1. Speaker level calibration",
                Signal = TestSignalKind.PinkNoise,
                Instructions = "Plays -20 dBFS pink noise on the selected speakers (LFE is band-limited to 120 Hz). " +
                               "Enable one speaker at a time and use an SPL meter (or phone app, C-weighted, slow) at the " +
                               "listening position. Trim each speaker with its gain slider on the Mixer tab until all read the same.",
                ExpectedResult = "Every speaker reads within +/-1 dB of the others (the subwoofer typically 0 to +3 dB).",
            },
            new()
            {
                Title = "2. Channel identification",
                Signal = TestSignalKind.ChannelId,
                Instructions = "A voice (or N beeps for channel N when speech is unavailable) announces each channel in AVR set-up " +
                               "order: Front left, Centre, Front right, Surround right, Surround left, Subwoofer.",
                ExpectedResult = "Each announcement comes from the speaker it names. The AVR's front panel should show " +
                                 "'Dolby Digital 3/2.1' (or DTS 3/2.1).",
            },
            new()
            {
                Title = "3. Centre channel bleed test",
                Signal = TestSignalKind.CenterBleed,
                Instructions = "Alternates every 3 s: identical noise in Front L + R (a 'phantom' centre), then the discrete Centre " +
                               "speaker alone.",
                ExpectedResult = "Both phases image at the centre of the screen. If the discrete phase comes from the left/right " +
                                 "speakers, the centre is mis-mapped or the AVR is set to 'Centre: None' / Phantom.",
            },
            new()
            {
                Title = "4. LFE low-pass isolation",
                Signal = TestSignalKind.LfeSweep,
                Instructions = "Sweeps a tone from 20 Hz to 200 Hz on the LFE channel only (8 s, repeating). Watch the live " +
                               "frequency readout.",
                ExpectedResult = "Only the subwoofer plays, and the tone fades out around the AVR's LFE low-pass (80-120 Hz). Sound " +
                                 "from the main speakers means LFE is mapped wrongly or 'LFE + Main' is enabled on the AVR.",
            },
            new()
            {
                Title = "5. Surround phase / polarity check",
                Signal = TestSignalKind.PhaseCheck,
                Instructions = "3 s steps: Front pair in-phase, Front pair with Right inverted, Surround pair in-phase, Surround pair " +
                               "with Right inverted.",
                ExpectedResult = "In-phase steps sound solid and centred between the pair; inverted steps sound diffuse/phasey with " +
                                 "less bass. If an 'inverted' step sounds MORE focused, one speaker of that pair is wired with " +
                                 "reversed polarity (+/- swapped).",
            },
        };
        Speakers = new ObservableCollection<SpeakerToggle>(
            new[] { "FL", "FR", "C", "LFE", "SL", "SR" }.Select((n, i) => new SpeakerToggle(i, n)));
        foreach (var s in Speakers)
            s.PropertyChanged += async (_, e) =>
            {
                if (e.PropertyName == nameof(SpeakerToggle.Enabled) && _active == TestSignalKind.PinkNoise) await StartAsync();
            };

        StartCommand = new AsyncRelayCommand(StartAsync);
        StopCommand = new AsyncRelayCommand(StopAsync);
        NextCommand = new RelayCommand(() => StepIndex = Math.Min(StepIndex + 1, Steps.Count - 1));
        BackCommand = new RelayCommand(() => StepIndex = Math.Max(StepIndex - 1, 0));
    }

    // v1.2.0: wording for setups without a subwoofer (dedicated subwoofer inactive).
    private bool _noSub;
    private DiagnosticStep[]? _subSteps;

    public void SetNoSubwoofer(bool noSub)
    {
        if (_noSub == noSub) return;
        _subSteps ??= Steps.ToArray();
        _noSub = noSub;
        var idx = _stepIndex;
        var updated = noSub ? NoSubSteps(_subSteps) : _subSteps;
        for (int i = 0; i < Steps.Count && i < updated.Length; i++) Steps[i] = updated[i];
        Speakers[3].Label = noSub ? "LFE\u2192fronts" : "LFE";
        _stepIndex = -1;
        StepIndex = idx;
        OnPropertyChanged(nameof(IsNoSub));
    }
    public bool IsNoSub => _noSub;

    private static DiagnosticStep[] NoSubSteps(DiagnosticStep[] s) => new[]
    {
        new DiagnosticStep
        {
            Title = s[0].Title,
            Signal = s[0].Signal,
            Instructions = "Plays -20 dBFS pink noise on the selected speakers. There is no subwoofer, so the LFE noise is " +
                           "folded into the front left and right speakers (low-passed at 120 Hz, -3 dB each; silent if the LFE " +
                           "setting is 'discard'). Enable one speaker at a time and use an SPL meter " +
                           "(or phone app, C-weighted, slow) at the listening position, then trim each speaker on the Mixer tab.",
            ExpectedResult = "Every speaker reads within +/-1 dB of the others. With only 'LFE\u2192fronts' enabled, the bass " +
                             "comes from the front pair.",
        },
        new DiagnosticStep
        {
            Title = s[1].Title,
            Signal = s[1].Signal,
            Instructions = "A voice (or N beeps for channel N when speech is unavailable) announces each channel: Front left, " +
                           "Centre, Front right, Surround right, Surround left, Subwoofer. Without a subwoofer the " +
                           "'Subwoofer' announcement plays from the front pair.",
            ExpectedResult = "Each announcement comes from the speaker it names; 'Subwoofer' from both front speakers. The AVR " +
                             "should show Dolby Digital 3/2 (5.0) - there is no LFE channel in the stream.",
        },
        s[2],
        new DiagnosticStep
        {
            Title = "4. Bass redirect check",
            Signal = s[3].Signal,
            Instructions = "Sweeps a tone from 20 Hz to 200 Hz on the LFE channel. With no subwoofer it is folded into the front " +
                           "left and right speakers.",
            ExpectedResult = "Both front speakers play the sweep evenly; nothing comes from the centre or surrounds. Very low " +
                             "notes fading out is normal for small speakers.",
        },
        s[4],
    };

    public ObservableCollection<DiagnosticStep> Steps { get; }
    public ObservableCollection<SpeakerToggle> Speakers { get; }
    public ICommand StartCommand { get; }
    public ICommand StopCommand { get; }
    public ICommand NextCommand { get; }
    public ICommand BackCommand { get; }

    public int StepIndex
    {
        get => _stepIndex;
        set
        {
            if (!Set(ref _stepIndex, Math.Clamp(value, 0, Steps.Count - 1))) return;
            OnPropertyChanged(nameof(CurrentStep));
            OnPropertyChanged(nameof(ShowSpeakerToggles));
            if (_active != TestSignalKind.Off) _ = StartAsync();   // follow the wizard while running
        }
    }

    public DiagnosticStep CurrentStep => Steps[_stepIndex];
    public bool ShowSpeakerToggles => CurrentStep.Signal == TestSignalKind.PinkNoise;
    public bool IsRunning => _active != TestSignalKind.Off;
    public string LiveStatus { get => _liveStatus; private set => Set(ref _liveStatus, value); }

    private async Task StartAsync()
    {
        var sig = CurrentStep.Signal;
        string token = sig switch
        {
            TestSignalKind.PinkNoise => "PINK",
            TestSignalKind.ChannelId => "CHANNEL_ID",
            TestSignalKind.CenterBleed => "CENTER_BLEED",
            TestSignalKind.LfeSweep => "LFE_SWEEP",
            TestSignalKind.PhaseCheck => "PHASE",
            _ => "OFF",
        };
        int mask = Speakers.Where(s => s.Enabled).Aggregate(0, (m, s) => m | (1 << s.Index));
        var r = await _client.SendAsync(sig == TestSignalKind.PinkNoise ? $"TEST {token} {mask:X}" : $"TEST {token}");
        if (r.Ok) SetActive(sig);
        else LiveStatus = "Could not start test: " + r.Message;
    }

    private async Task StopAsync()
    {
        await _client.SendAsync("TEST OFF");
        SetActive(TestSignalKind.Off);
    }

    private void SetActive(TestSignalKind k)
    {
        _active = k;
        OnPropertyChanged(nameof(IsRunning));
    }

    private static readonly string[] Names = { "Front left", "Front right", "Centre", "Subwoofer (LFE)", "Surround left", "Surround right" };

    /// <summary>Called at telemetry rate with the engine's view of the running test.</summary>
    public void UpdateFromTelemetry(in TelemetryBlock t)
    {
        var sig = (TestSignalKind)t.TestSignal;
        if (sig != _active) SetActive(sig);   // e.g. stopped from another client
        LiveStatus = sig switch
        {
            TestSignalKind.Off => "No test running.",
            TestSignalKind.PinkNoise => "Pink noise playing on the selected speakers.",
            TestSignalKind.ChannelId => t.TestChannel is >= 0 and < 6 ? $"Now: {Names[t.TestChannel]}" : "Channel ID",
            TestSignalKind.CenterBleed => t.TestStep == 0 ? "Now: phantom centre (Front L + R)" : "Now: discrete Centre speaker",
            TestSignalKind.LfeSweep => $"LFE sweep: {t.TestFrequencyHz:0} Hz",
            TestSignalKind.PhaseCheck => t.TestStep switch
            {
                0 => "Front pair - IN phase",
                1 => "Front pair - Right INVERTED",
                2 => "Surround pair - IN phase",
                _ => "Surround pair - Right INVERTED",
            },
            _ => string.Empty,
        };
    }
}
