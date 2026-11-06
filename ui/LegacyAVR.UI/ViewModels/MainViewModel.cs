using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.Services;
using Microsoft.Win32;

namespace LegacyAVR.UI.ViewModels;

public sealed class MainViewModel : ObservableObject, IDisposable
{
    private static readonly Brush GoodBrush = Frozen(0x22, 0xC5, 0x5E);
    private static readonly Brush IdleBrush = Frozen(0x8B, 0x93, 0xA1);
    private static readonly Brush WarnBrush = Frozen(0xF5, 0x9E, 0x0B);
    private static readonly Brush ErrorBrush = Frozen(0xEF, 0x44, 0x44);
    private static readonly Brush InfoBrush = Frozen(0x4F, 0x8C, 0xFF);

    private static readonly string[] SpeakerNames =
        { "FL", "FR", "C", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR" };

    private readonly EngineClient _client;
    private readonly TelemetryReader _reader;
    private readonly SettingsService _settings;
    private readonly ProfileStore _profiles;
    private readonly SettingResets _resets = new();
    private readonly CommandThrottle _throttle;
    private readonly DispatcherTimer _timer;
    private readonly Queue<(float cpu, float lat)> _statHistory = new();
    private const int StatWindow = 300;   // 10 s at 30 Hz
    private double _cpuSum, _latSum;
    private bool _syncing;
    private bool _userShutdown;           // user shut the engine down: don't auto-relaunch
    private uint _lastConfigRevision;
    private uint _lastInMask = uint.MaxValue;
    private DateTime _nextReconnect = DateTime.MinValue;
    private bool _launching;
    private uint _lastEmitMask = uint.MaxValue, _lastEmitChannels = uint.MaxValue;
    private readonly DispatcherTimer _gameTimer;
    private bool _gameReported;          // last GAME_DETECTED value sent to the engine
    private DateTime _lastGameBeat = DateTime.MinValue;
    private bool _gamingActive;                 // from telemetry
    private string _outputLayout = string.Empty;
    private bool _startupDone;                  // v1.4.0: default output / last profile handled once per session
    private List<DeviceItem> _rawDevices = new();
    private string? _engineOutputId;            // what the engine has configured (may be disconnected)

    /// <summary>Asks the user a yes/no question (replaceable in tests).</summary>
    public Func<string, bool> Confirm { get; set; } = q =>
        MessageBox.Show(q, "LegacyAVR", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK;

    public MainViewModel(EngineClient client, TelemetryReader reader, SettingsService settings, ProfileStore profiles)
    {
        _client = client;
        _reader = reader;
        _settings = settings;
        _profiles = profiles;
        _throttle = new CommandThrottle(client);
        _throttle.Error += msg => Application.Current?.Dispatcher.BeginInvoke(() => LastCommandError = msg);

        string[] longNames = { "Front Left", "Front Right", "Centre", "Subwoofer (LFE)", "Surround Left", "Surround Right" };
        string[] shortNames = { "FL", "FR", "C", "LFE", "SL", "SR" };
        Channels = new ObservableCollection<ChannelStripViewModel>(Enumerable.Range(0, 6).Select(i =>
            new ChannelStripViewModel(i, shortNames[i], longNames[i],
                ch => _throttle.Post($"gain{ch.Index}", $"SET_GAIN {ch.Index} {Proto.F(ch.GainDb)}"),
                ch => _throttle.Post($"mute{ch.Index}", $"SET_MUTE {ch.Index} {(ch.IsMuted ? 1 : 0)}"))));
        InputMeters = new ObservableCollection<InputMeterViewModel>(
            new[] { "FL", "FR", "C", "LFE", "BL", "BR", "SL", "SR" }.Select(n => new InputMeterViewModel(n)));
        OutputMeters = new ObservableCollection<InputMeterViewModel>(
            new[] { "FL", "FR", "C", "LFE", "SL", "SR", "-", "-" }.Select(n => new InputMeterViewModel(n)));

        Diagnostics = new DiagnosticsViewModel(client);
        Profiles = new ObservableCollection<Profile>();
        _activeProfile = _settings.Settings.LastProfile;
        ProfileStatus = _profiles.MigrationNote;

        // v1.4.0 custom EQ, one per output channel
        EqChannels = new ObservableCollection<ChannelEqViewModel>(Enumerable.Range(0, 6).Select(i =>
            new ChannelEqViewModel(i, longNames[i],
                (ch, band) => _throttle.Post($"eq{ch.Index}_{band.Band}", EqModel.BandCommand(ch.Index, band.Band, band.ToModel())),
                ch => Send($"SET_EQ_CHANNEL {ch.Index} {(ch.Enabled ? 1 : 0)}"))));
        foreach (var ch in Channels) ch.Eq = EqChannels[ch.Index];

        RefreshDevicesCommand = new AsyncRelayCommand(LoadDevicesAsync);
        RestartEngineCommand = new AsyncRelayCommand(async () => { await _client.SendAsync("RESTART"); });
        ToggleAudioCommand = new AsyncRelayCommand(ToggleAudioAsync);
        ShutdownEngineCommand = new AsyncRelayCommand(ShutdownEngineAsync);
        StartEngineCommand = new AsyncRelayCommand(StartEngineAsync);

        ApplyProfileCommand = new AsyncRelayCommand(async p => { if ((p as Profile ?? SelectedProfile) is { } pr) await ApplyProfileInteractiveAsync(pr); });
        SaveProfileCommand = new RelayCommand(OverwriteSelectedProfile, () => SelectedProfile != null);
        SaveAsNewProfileCommand = new RelayCommand(SaveAsNewProfile, () => !string.IsNullOrWhiteSpace(NewProfileName));
        DeleteProfileCommand = new RelayCommand(DeleteSelectedProfile, () => SelectedProfile != null);
        RestoreDefaultProfilesCommand = new RelayCommand(RestoreDefaultProfiles);
        ImportProfilesCommand = new RelayCommand(ImportProfiles);
        ExportProfilesCommand = new RelayCommand(ExportProfiles);
        ResetCommand = new RelayCommand(p => { if (p is string key) _resets.Reset(key, Confirm); });
        OpenEqCommand = new RelayCommand(p =>
        {
            int ch = p switch { int n => n, string t when int.TryParse(t, out var n2) => n2, _ => -1 };
            if (ch >= 0 && ch < 6) OpenEqRequested?.Invoke(this, ch);
        });
        SetDefaultOutputCommand = new RelayCommand(SetDefaultOutput, () => ShowDefaultOutputButton);
        RegisterResets();

        OpenLogCommand = new RelayCommand(() => Shell(Environment.ExpandEnvironmentVariables(
            _reader.IsServiceMapping ? @"%ProgramData%\LegacyAVR" : @"%LOCALAPPDATA%\LegacyAVR")));
        OpenSoundSettingsCommand = new RelayCommand(() => Shell("ms-settings:sound"));
        OpenClassicSoundCommand = new RelayCommand(() => Shell("mmsys.cpl"));
        SetStatsModeCommand = new RelayCommand(p => { if (p is string s && Enum.TryParse<StatsDisplayMode>(s, out var m)) StatsMode = m; });

        _timer = new DispatcherTimer(DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(33) };
        _timer.Tick += (_, _) => OnTick();
        _gameTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(1) };
        _gameTimer.Tick += (_, _) => OnGameTick();
        OpenStreamingExtensionCommand = new RelayCommand(() => Shell(StreamingExtensionUrl));
        RebuildUpmixOptions();
        RefreshProfiles(_activeProfile);
    }

    // ===================================================================== collections
    public ObservableCollection<ChannelStripViewModel> Channels { get; }
    public ObservableCollection<InputMeterViewModel> InputMeters { get; }
    /// <summary>v1.2.0: the channels that actually leave the engine (output visualiser).</summary>
    public ObservableCollection<InputMeterViewModel> OutputMeters { get; }
    public ObservableCollection<DeviceItem> InputDevices { get; } = new();
    public ObservableCollection<DeviceItem> OutputDevices { get; } = new();
    public ObservableCollection<Profile> Profiles { get; }
    /// <summary>v1.4.0: custom EQ of FL FR C LFE SL SR.</summary>
    public ObservableCollection<ChannelEqViewModel> EqChannels { get; }
    public DiagnosticsViewModel Diagnostics { get; }
    public HelpViewModel Help { get; } = new();

    public IReadOnlyList<CodecChoice> CodecOptions { get; } = new[]
    {
        new CodecChoice("Dolby Digital Live (AC-3)", CodecKind.AC3),
        new CodecChoice("DTS Interactive", CodecKind.DTS),
        new CodecChoice("Dolby Digital Plus (E-AC-3)", CodecKind.EAC3),
        new CodecChoice("PCM (no encoding)", CodecKind.PCM),
    };
    public IReadOnlyList<Option<OutputModeKind>> OutputModeOptions { get; } = new[]
    {
        new Option<OutputModeKind>("WASAPI Exclusive - bitstream", OutputModeKind.Exclusive),
        new Option<OutputModeKind>("WASAPI Shared - compatibility (PCM)", OutputModeKind.Shared),
    };
    // v1.4.0: without a dedicated subwoofer the dynamic modes create no LFE, so they are
    // shown as 3.0 / 5.0. The list is rebuilt when the subwoofer switch changes.
    private IReadOnlyList<Option<UpmixKind>> _upmixOptions = BuildUpmixOptions(true);
    public IReadOnlyList<Option<UpmixKind>> UpmixOptions { get => _upmixOptions; private set => Set(ref _upmixOptions, value); }

    public static string UpmixLabel(UpmixKind k, bool dedicatedSub) => k switch
    {
        UpmixKind.Passthrough => "Keep stereo - encode in the chosen format",
        UpmixKind.Upmix31 => dedicatedSub ? "Dynamic 3.1 upmix" : "Dynamic 3.0 upmix",
        UpmixKind.Upmix51 => dedicatedSub ? "Dynamic 5.1 upmix" : "Dynamic 5.0 upmix",
        _ => "Lossless PCM 2.0 for stereo (auto-switch for surround)",
    };

    public static IReadOnlyList<Option<UpmixKind>> BuildUpmixOptions(bool dedicatedSub) =>
        new[] { UpmixKind.Passthrough, UpmixKind.Upmix31, UpmixKind.Upmix51, UpmixKind.PcmDirect }
            .Select(k => new Option<UpmixKind>(UpmixLabel(k, dedicatedSub), k)).ToArray();

    private void RebuildUpmixOptions()
    {
        var current = _upmix?.Value;
        var wasSyncing = _syncing;
        _syncing = true;   // re-selecting the same mode must not send anything
        UpmixOptions = BuildUpmixOptions(_dedicatedSub);
        LfeGenOptions = BuildLfeGenOptions(_dedicatedSub);
        if (current is { } c) SelectedUpmix = UpmixOptions.First(o => o.Value == c);
        if (_lfeGen?.Value is { } lg) SelectedLfeGen = LfeGenOptions.First(o => o.Value == lg);
        _syncing = wasSyncing;
    }
    // v1.2.0: the setting is a safety margin on top of what the device pair needs. Values
    // below the computed floor (capture packet + half a render period + 2 ms) are raised
    // to it, so "6 ms" / "10 ms" choices that could never be honoured are gone.
    private IReadOnlyList<Option<int>> _bufferOptions = BuildBufferOptions(0);
    public IReadOnlyList<Option<int>> BufferOptions { get => _bufferOptions; private set => Set(ref _bufferOptions, value); }
    private static IReadOnlyList<Option<int>> BuildBufferOptions(int current)
    {
        var list = new List<Option<int>> { new("Automatic - lowest safe margin (recommended)", 0) };
        foreach (var ms in new[] { 15, 20, 30, 50, 80 }) list.Add(new($"{ms} ms safety margin", ms));
        if (current != 0 && list.All(o => o.Value != current)) list.Add(new($"{current} ms safety margin", current));
        return list;
    }
    private IReadOnlyList<Option<LfeGenKind>> _lfeGenOptions = BuildLfeGenOptions(true);
    public IReadOnlyList<Option<LfeGenKind>> LfeGenOptions { get => _lfeGenOptions; private set => Set(ref _lfeGenOptions, value); }

    public static IReadOnlyList<Option<LfeGenKind>> BuildLfeGenOptions(bool dedicatedSub) => dedicatedSub
        ? new[]
        {
            new Option<LfeGenKind>("Auto - only for 5.0 / 7.0 sources without an LFE", LfeGenKind.Auto),
            new Option<LfeGenKind>("Always - also stereo kept as stereo (2.0 -> 2.1)", LfeGenKind.Always),
            new Option<LfeGenKind>("Off", LfeGenKind.Off),
        }
        : new[]
        {
            // No subwoofer: "generation" moves centre/surround bass to the front speakers.
            new Option<LfeGenKind>("Auto - 5.0 / 7.0 sources: centre and surround bass to the fronts", LfeGenKind.Auto),
            new Option<LfeGenKind>("Always - every multichannel source: centre and surround bass to the fronts", LfeGenKind.Always),
            new Option<LfeGenKind>("Off", LfeGenKind.Off),
        };
    public IReadOnlyList<Option<GamingKind>> GamingOptions { get; } = new[]
    {
        new Option<GamingKind>("Auto-detect full-screen games", GamingKind.Auto),
        new Option<GamingKind>("Always on (game mode)", GamingKind.On),
        new Option<GamingKind>("Off", GamingKind.Off),
    };

    private IReadOnlyList<Option<uint>> _bitrateOptions = Array.Empty<Option<uint>>();
    public IReadOnlyList<Option<uint>> BitrateOptions { get => _bitrateOptions; private set => Set(ref _bitrateOptions, value); }

    private static IReadOnlyList<Option<uint>> BitratesFor(CodecKind c) => c switch
    {
        CodecKind.AC3 => new[] { 640, 576, 512, 448, 384 }.Select(k => new Option<uint>($"{k} kbps", (uint)k * 1000)).ToArray(),
        CodecKind.DTS => new[] { new Option<uint>("1509.75 kbps (full rate)", 1509750), new Option<uint>("754.5 kbps (half rate)", 754500) },
        CodecKind.EAC3 => new[] { 1024, 768, 640, 512, 384, 256 }.Select(k => new Option<uint>($"{k} kbps", (uint)k * 1000)).ToArray(),
        _ => Array.Empty<Option<uint>>(),
    };

    // ===================================================================== commands
    public ICommand RefreshDevicesCommand { get; }
    public ICommand RestartEngineCommand { get; }
    public ICommand ToggleAudioCommand { get; }
    public ICommand ShutdownEngineCommand { get; }
    public ICommand StartEngineCommand { get; }
    public ICommand ApplyProfileCommand { get; }
    public ICommand SaveProfileCommand { get; }
    public ICommand SaveAsNewProfileCommand { get; }
    public ICommand DeleteProfileCommand { get; }
    public ICommand RestoreDefaultProfilesCommand { get; }
    public ICommand ImportProfilesCommand { get; }
    public ICommand ExportProfilesCommand { get; }
    /// <summary>v1.4.0: resets one setting to its factory default (parameter = key, see RegisterResets).</summary>
    public ICommand ResetCommand { get; }
    /// <summary>v1.4.0: opens the EQ dialog of a channel (parameter = channel index).</summary>
    public ICommand OpenEqCommand { get; }
    /// <summary>v1.4.0: makes the selected output LegacyAVR's default output.</summary>
    public ICommand SetDefaultOutputCommand { get; }
    public event EventHandler<int>? OpenEqRequested;
    public ICommand OpenLogCommand { get; }
    public ICommand OpenSoundSettingsCommand { get; }
    public ICommand OpenClassicSoundCommand { get; }
    public ICommand SetStatsModeCommand { get; }
    public ICommand OpenStreamingExtensionCommand { get; }

    /// <summary>Optional third-party Edge add-on mentioned in the Setup guide (not made or endorsed by LegacyAVR).</summary>
    public const string StreamingExtensionUrl =
        "https://microsoftedge.microsoft.com/addons/detail/51-surround-for-netflix/binkliomfaficonojepgobcoekpfapnj";

    // ===================================================================== settings (-> engine)
    private DeviceItem? _selectedInput, _selectedOutput;
    public DeviceItem? SelectedInputDevice
    {
        get => _selectedInput;
        set { if (Set(ref _selectedInput, value) && !_syncing && value != null) Send($"SET_INPUT_DEVICE {Proto.Quote(value.Id)}"); }
    }
    public DeviceItem? SelectedOutputDevice
    {
        get => _selectedOutput;
        set
        {
            // A WPF ComboBox writes null back while its list is being rebuilt: ignore that, the
            // selection is restored right after (this left the box blank before 1.4).
            if (value is null && _syncing) return;
            if (!Set(ref _selectedOutput, value)) return;
            UpdateCodecSupport();
            OnPropertyChanged(nameof(ShowDefaultOutputButton));
            OnPropertyChanged(nameof(ProfilesTitle));
            if (_onlyThisOutput) RefreshProfiles(_selectedProfile?.Name);
            if (value is null || value.IsMissing) return;     // a disconnected device cannot be selected
            if (!string.Equals(_settings.Settings.LastOutputDeviceId, value.Id, StringComparison.OrdinalIgnoreCase) ||
                _settings.Settings.LastOutputDeviceName != value.Name)
            {
                _settings.Settings.LastOutputDeviceId = value.Id;
                _settings.Settings.LastOutputDeviceName = value.Name;
                _settings.Save();
            }
            if (!_syncing)
            {
                _engineOutputId = value.Id;
                _startupOutputNotice = string.Empty;
                OutputNotice = string.Empty;
                Send($"SET_OUTPUT_DEVICE {Proto.Quote(value.Id)}");
            }
        }
    }

    // ----- v1.4.0: LegacyAVR's own default output ("Default" button). Windows' default is never touched.
    private string _outputNotice = string.Empty;
    private string _startupOutputNotice = string.Empty;   // e.g. "Default output ... is not connected" (until the user picks one)
    /// <summary>Why the output box shows what it shows (not connected, nothing selected...).</summary>
    public string OutputNotice
    {
        get => _outputNotice;
        private set { if (Set(ref _outputNotice, value)) OnPropertyChanged(nameof(HasOutputNotice)); }
    }
    public bool HasOutputNotice => !string.IsNullOrEmpty(_outputNotice);
    public bool ShowDefaultOutputButton =>
        StartupPolicy.ShowDefaultButton(_selectedOutput?.Id, _selectedOutput is { IsMissing: false }, _settings.Settings.DefaultOutputDeviceId);
    public string DefaultOutputText => string.IsNullOrEmpty(_settings.Settings.DefaultOutputDeviceId)
        ? "No default output yet: select your AVR / TV output and press Default to have it chosen every time LegacyAVR starts."
        : $"Default output: {_settings.Settings.DefaultOutputDeviceName ?? _settings.Settings.DefaultOutputDeviceId} - chosen every time LegacyAVR starts (Windows' own default device is not changed).";

    private void SetDefaultOutput()
    {
        if (_selectedOutput is not { IsMissing: false } d) return;
        _settings.Settings.DefaultOutputDeviceId = d.Id;
        _settings.Settings.DefaultOutputDeviceName = d.Name;
        _settings.Save();
        RebuildDeviceLists();
        OnPropertyChanged(nameof(DefaultOutputText));
    }

    private void UpdateCodecSupport()
    {
        var caps = _selectedOutput?.IecCaps ?? 0u;
        foreach (var c in CodecOptions) c.Update(caps);
        OnPropertyChanged(nameof(FormatSupportText));
        OnPropertyChanged(nameof(IsSelectedFormatUnsupported));
    }

    /// <summary>Line under the Format drop-down.</summary>
    public string FormatSupportText
    {
        get
        {
            var caps = _selectedOutput?.IecCaps ?? 0u;
            if (_selectedOutput is null) return "Pick the output device first.";
            if (_selectedOutput.IsMissing) return "This output is not connected, so its formats are unknown.";
            if ((caps & CodecChoice.CapsKnown) == 0)
                return "This output could not be asked which formats it accepts (it was busy). Formats are not marked.";
            var ok = CodecOptions.Where(c => c.Value != CodecKind.PCM && c.Supported).Select(c => c.Value switch
            {
                CodecKind.AC3 => "Dolby Digital",
                CodecKind.DTS => "DTS",
                _ => "Dolby Digital Plus",
            }).ToList();
            return ok.Count == 0
                ? "This output accepts no bitstream format (PCM only) - check the TV/AVR connection and Windows Supported Formats."
                : "This output accepts: " + string.Join(", ", ok) + ". Others are marked and would fall back to PCM.";
        }
    }
    public bool IsSelectedFormatUnsupported => _codec is { Supported: false };

    private CodecChoice? _codec;
    public CodecChoice? SelectedCodec
    {
        get => _codec;
        set
        {
            if (!Set(ref _codec, value) || value is null) return;
            BitrateOptions = BitratesFor(value.Value);
            OnPropertyChanged(nameof(IsBitrateEnabled));
            OnPropertyChanged(nameof(IsSelectedFormatUnsupported));
            if (_syncing) return;
            Send($"SET_CODEC {value.Value}");
            _bitrate = BitrateOptions.FirstOrDefault();   // engine resets to the codec default
            OnPropertyChanged(nameof(SelectedBitrate));
        }
    }
    public bool IsBitrateEnabled => _codec?.Value != CodecKind.PCM;

    private Option<uint>? _bitrate;
    public Option<uint>? SelectedBitrate
    {
        get => _bitrate;
        set { if (Set(ref _bitrate, value) && !_syncing && value != null) Send($"SET_BITRATE {value.Value}"); }
    }

    private Option<OutputModeKind>? _outputMode;
    public Option<OutputModeKind>? SelectedOutputMode
    {
        get => _outputMode;
        set { if (Set(ref _outputMode, value) && !_syncing && value != null) Send($"SET_OUTPUT_MODE {(value.Value == OutputModeKind.Shared ? "SHARED" : "EXCLUSIVE")}"); }
    }

    private Option<UpmixKind>? _upmix;
    public Option<UpmixKind>? SelectedUpmix
    {
        get => _upmix;
        set
        {
            // Only the mode changes: every upmix slider keeps its value, so switching to
            // Lossless PCM and back restores the previous dynamic upmix exactly.
            if (!Set(ref _upmix, value) || value is null) return;
            OnPropertyChanged(nameof(IsKeepStereoMode));
            OnPropertyChanged(nameof(IsUpmixMode));
            if (!_syncing) Send($"SET_UPMIX {ProfileStore.UpmixToken(value.Value)}");
        }
    }
    public bool IsKeepStereoMode => _upmix?.Value == UpmixKind.Passthrough;
    public bool IsUpmixMode => _upmix?.Value is UpmixKind.Upmix31 or UpmixKind.Upmix51;

    private int _bufferMs;
    public int SelectedBufferMs
    {
        get => _bufferMs;
        set
        {
            if (!Set(ref _bufferMs, value)) return;
            if (_bufferOptions.All(o => o.Value != value)) BufferOptions = BuildBufferOptions(value);
            OnPropertyChanged(nameof(SelectedBufferOption));
            if (!_syncing) Send($"SET_BUFFER_MS {value}");
        }
    }
    public Option<int>? SelectedBufferOption
    {
        get => _bufferOptions.FirstOrDefault(o => o.Value == _bufferMs);
        set { if (value != null) SelectedBufferMs = value.Value; }
    }

    // ----- v1.2.0: bass, subwoofer, LFE generation, game mode
    private double _bassDb;
    public double BassDb
    {
        get => _bassDb;
        set
        {
            if (!Set(ref _bassDb, Math.Round(Math.Clamp(value, -12, 12) * 2) / 2)) return;
            OnPropertyChanged(nameof(BassText));
            if (!_syncing) _throttle.Post("bass", $"SET_BASS {Proto.F(_bassDb)}");
        }
    }
    public string BassText => _bassDb.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture) + " dB";

    // ----- v1.4.0: custom per-channel EQ (master switch). It replaces the single bass control.
    private bool _eqEnabled;
    public bool EqEnabled
    {
        get => _eqEnabled;
        set
        {
            if (!Set(ref _eqEnabled, value)) return;
            OnPropertyChanged(nameof(IsBassEnabled));
            OnPropertyChanged(nameof(BassDisabledText));
            OnPropertyChanged(nameof(EqStatusText));
            if (!_syncing) Send($"SET_EQ {(value ? 1 : 0)}");
        }
    }
    /// <summary>The Bass slider is unavailable while Custom EQ is on (the EQ replaces it).</summary>
    public bool IsBassEnabled => !_eqEnabled;
    public string BassDisabledText => _eqEnabled
        ? "Custom EQ is on and replaces the Bass control (its setting is kept and returns when Custom EQ is off). Use the low shelf of each channel's EQ instead."
        : string.Empty;
    private float _eqHeadroomDb;
    private int _auditionChannel = -1;
    public string EqStatusText => !_eqEnabled
        ? "Custom EQ is off (bypassed): every channel is flat, your EQ settings are kept."
        : _eqHeadroomDb < -0.05f
            ? $"Custom EQ is on. {-_eqHeadroomDb:0.0} dB is taken off every channel so the boosts cannot clip (the safety limiter stays active)."
            : "Custom EQ is on.";
    public int AuditionChannel => _auditionChannel;
    public string EqHeadroomText => _eqHeadroomDb < -0.05f
        ? $"Headroom: {-_eqHeadroomDb:0.0} dB is taken off every channel so the boosts cannot clip."
        : "Headroom: no reduction needed (no band boosts the signal).";

    /// <summary>Solo one channel while tuning its EQ (-1 = off). Temporary: never saved, saved mutes untouched.</summary>
    public void SetAudition(int channel)
    {
        _auditionChannel = channel;
        Send(channel >= 0 ? $"SET_AUDITION {channel}" : "SET_AUDITION OFF");
    }

    /// <summary>Restores one channel's EQ to flat (keeps its enable switch).</summary>
    public void ResetEqChannel(int channel)
    {
        if (channel < 0 || channel >= EqChannels.Count) return;
        foreach (var b in EqChannels[channel].Bands) b.SetFromModel(EqModel.DefaultBand(channel, b.Band));
        Send($"RESET_EQ_CHANNEL {channel}");
    }

    public bool ConfirmAndResetEqChannel(int channel)
    {
        if (channel < 0 || channel >= EqChannels.Count || EqChannels[channel].IsFlat) return false;
        if (!Confirm($"Reset the EQ of {EqChannels[channel].Name} to flat?")) return false;
        ResetEqChannel(channel);
        return true;
    }

    private bool _dedicatedSub = true;
    private DateTime _subChangedUtc = DateTime.MinValue;
    /// <summary>Dedicated subwoofer active/inactive switch (Bass &amp; Subwoofer card). Off = no subwoofer: LFE is folded into the fronts.</summary>
    public bool DedicatedSub
    {
        get => _dedicatedSub;
        set
        {
            if (!Set(ref _dedicatedSub, value)) return;
            OnPropertyChanged(nameof(IsNoSub));
            OnPropertyChanged(nameof(SubToggleText));
            OnPropertyChanged(nameof(LfeControlsNote));
            ApplySubLayout();
            RebuildUpmixOptions();
            if (_syncing) return;
            _subChangedUtc = DateTime.UtcNow;
            Send($"SET_SUB {(value ? 1 : 0)}");
        }
    }
    public bool IsNoSub => !_dedicatedSub;
    /// <summary>v1.4.0: the LFE crossover / level sliders stay available without a subwoofer
    /// because LFE generation (bass redirect to the fronts) still uses them.</summary>
    public string LfeControlsNote => _dedicatedSub
        ? "LFE crossover and LFE level set the subwoofer channel of the Dynamic upmix and of LFE generation."
        : "No subwoofer: the upmix creates no LFE, but LFE crossover and LFE level still set how much centre / surround bass LFE generation moves to the front speakers.";
    /// <summary>"Dedicated subwoofer active (5.1)" / "Dedicated subwoofer inactive (5.0)" - the
    /// layout in brackets is what is actually being output right now.</summary>
    public string SubToggleText => SubLabel(_dedicatedSub, _outputLayout);

    public static string SubLabel(bool sub, string layout) =>
        (sub ? "Dedicated subwoofer active" : "Dedicated subwoofer inactive") +
        (string.IsNullOrEmpty(layout) ? string.Empty : $" ({layout})");

    /// <summary>"5.1", "5.0", "2.0", "7.1" for the channels leaving the engine ("" = unknown).</summary>
    public static string OutputLayout(uint emitMask, uint emitChannels, bool dedicatedSub)
    {
        if (emitChannels == 0) return string.Empty;
        uint full = emitMask & ~0x8u;
        int n = 0;
        while (full != 0) { full &= full - 1; n++; }
        bool lfe = (emitMask & 0x8u) != 0 && dedicatedSub;
        return $"{n}.{(lfe ? 1 : 0)}";
    }

    private void ApplySubLayout()
    {
        if (Channels.Count > 3)
        {
            if (_dedicatedSub) Channels[3].Relabel("LFE", "Subwoofer (LFE)");
            else Channels[3].Relabel("LFE \u2192 FL/FR", "Bass to front speakers");
            if (EqChannels is { Count: > 3 }) EqChannels[3].Name = _dedicatedSub ? "Subwoofer (LFE)" : "LFE \u2192 FL/FR (no subwoofer)";
        }
        Diagnostics.SetNoSubwoofer(!_dedicatedSub);
    }

    private Option<LfeGenKind>? _lfeGen;
    public Option<LfeGenKind>? SelectedLfeGen
    {
        get => _lfeGen;
        set
        {
            if (!Set(ref _lfeGen, value) || value is null) return;
            if (!_syncing) Send($"SET_LFE_GEN {ProfileStore.LfeGenToken(value.Value)}");
        }
    }

    /// <summary>The general LFE-generation control is disabled while Game Mode's own LFE toggle
    /// is on AND a game is active, because that toggle overrides it.</summary>
    public bool IsGeneralLfeGenEnabled => !(_gameLfeGen && _gamingActive);
    public string LfeOverrideText => IsGeneralLfeGenEnabled
        ? string.Empty
        : "Overridden by Game Mode: LFE generation is on while this game is running.";

    private bool _gameLfeGen;
    /// <summary>Game Mode's own LFE generation toggle (v1.3.0).</summary>
    public bool GameLfeGen
    {
        get => _gameLfeGen;
        set
        {
            if (!Set(ref _gameLfeGen, value)) return;
            OnPropertyChanged(nameof(IsGeneralLfeGenEnabled));
            OnPropertyChanged(nameof(LfeOverrideText));
            if (!_syncing) Send($"SET_GAME_LFE {(value ? 1 : 0)}");
        }
    }

    private double _lfeResetSeconds = 10;
    /// <summary>Silence needed before Auto LFE generation may resume after the source used its own LFE.</summary>
    public double LfeResetSeconds
    {
        get => _lfeResetSeconds;
        set
        {
            if (!Set(ref _lfeResetSeconds, FactoryDefaults.ClampLfeReset(value))) return;   // whole seconds 1..300
            OnPropertyChanged(nameof(LfeResetText));
            if (!_syncing) _throttle.Post("lfereset", $"SET_LFE_RESET {Proto.F(_lfeResetSeconds)}");
        }
    }
    public string LfeResetText => $"{_lfeResetSeconds:0} s";
    public double LfeResetMin => FactoryDefaults.LfeResetMinSeconds;
    public double LfeResetMax => FactoryDefaults.LfeResetMaxSeconds;

    private string _lfeStatusText = "-";
    private Brush _lfeStatusBrush = IdleBrush;
    /// <summary>LFE status indicator: source / generated / folded into the fronts / inhibited.</summary>
    public string LfeStatusText { get => _lfeStatusText; private set => Set(ref _lfeStatusText, value); }
    public Brush LfeStatusBrush { get => _lfeStatusBrush; private set => Set(ref _lfeStatusBrush, value); }

    private Option<GamingKind>? _gaming;
    public Option<GamingKind>? SelectedGaming
    {
        get => _gaming;
        set
        {
            if (!Set(ref _gaming, value) || value is null) return;
            if (!_syncing) Send($"SET_GAMING {ProfileStore.GamingToken(value.Value)}");
            if (value.Value != GamingKind.Auto && _gameReported) ReportGame(false, string.Empty);
        }
    }

    private bool _nightMode, _stereoAs20 = true, _muteAll;
    public bool NightMode { get => _nightMode; set { if (Set(ref _nightMode, value) && !_syncing) Send($"SET_NIGHT_MODE {(value ? 1 : 0)}"); } }
    public bool StereoAs20 { get => _stereoAs20; set { if (Set(ref _stereoAs20, value) && !_syncing) Send($"SET_STEREO_AS_20 {(value ? 1 : 0)}"); } }
    public bool MuteAll
    {
        get => _muteAll;
        set
        {
            if (!Set(ref _muteAll, value)) return;
            OnPropertyChanged(nameof(TrayTooltip));
            if (!_syncing) Send($"SET_MUTE_ALL {(value ? 1 : 0)}");
        }
    }

    private double _masterDb;
    public double MasterDb
    {
        get => _masterDb;
        set
        {
            if (!Set(ref _masterDb, Math.Round(value * 2) / 2)) return;
            OnPropertyChanged(nameof(MasterText));
            if (!_syncing) _throttle.Post("master", $"SET_MASTER {Proto.F(_masterDb)}");
        }
    }
    public string MasterText => _masterDb.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture) + " dB";

    private double _centre = 0.85, _centreFloor = 0.3, _dialog = 0.6, _surround = 0.8, _delay = 12, _lfeXover = 80, _lfeLevel = -10;
    public double CentreLevel { get => _centre; set { if (Set(ref _centre, value) && !_syncing) Param("CENTRE", value); } }
    public double CentreFloor { get => _centreFloor; set { if (Set(ref _centreFloor, value) && !_syncing) Param("CENTRE_FLOOR", value); } }
    public double DialogBoost { get => _dialog; set { if (Set(ref _dialog, value) && !_syncing) Param("DIALOG", value); } }
    public double SurroundLevel { get => _surround; set { if (Set(ref _surround, value) && !_syncing) Param("SURROUND", value); } }
    public double SurroundDelayMs { get => _delay; set { if (Set(ref _delay, value) && !_syncing) Param("DELAY", value); } }
    public double LfeCrossoverHz { get => _lfeXover; set { if (Set(ref _lfeXover, value) && !_syncing) Param("LFE_XOVER", value); } }
    public double LfeLevelDb { get => _lfeLevel; set { if (Set(ref _lfeLevel, value) && !_syncing) Param("LFE_LEVEL", value); } }

    private void Param(string name, double v) => _throttle.Post("p_" + name, $"SET_UPMIX_PARAM {name} {Proto.F(v)}");

    // ----- v1.4.0: individual "reset to default" buttons (one setting each, confirmed, no reset-all)
    private void RegisterResets()
    {
        static string Db(double v) => v.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture) + " dB";
        static string Pct(double v) => $"{v * 100:0}%";
        static bool Eq(double a, double b) => Math.Abs(a - b) < 1e-9;

        _resets.Add("master", "the Master level", () => Db(FactoryDefaults.MasterDb), () => Eq(MasterDb, FactoryDefaults.MasterDb),
                    () => MasterDb = FactoryDefaults.MasterDb);
        foreach (var ch in Channels)
        {
            var c = ch;
            _resets.Add(c.ResetKey, $"the {c.LongName} trim", () => Db(FactoryDefaults.GainDb), () => Eq(c.GainDb, FactoryDefaults.GainDb),
                        () => c.GainDb = FactoryDefaults.GainDb);
        }
        _resets.Add("bass", "Bass", () => Db(FactoryDefaults.BassDb), () => Eq(BassDb, FactoryDefaults.BassDb), () => BassDb = FactoryDefaults.BassDb);
        _resets.Add("centre", "Centre steering", () => Pct(FactoryDefaults.CentreLevel), () => Eq(CentreLevel, FactoryDefaults.CentreLevel),
                    () => CentreLevel = FactoryDefaults.CentreLevel);
        _resets.Add("centreFloor", "Centre floor", () => Pct(FactoryDefaults.CentreFloor), () => Eq(CentreFloor, FactoryDefaults.CentreFloor),
                    () => CentreFloor = FactoryDefaults.CentreFloor);
        _resets.Add("dialog", "Dialogue boost", () => Pct(FactoryDefaults.DialogBoost), () => Eq(DialogBoost, FactoryDefaults.DialogBoost),
                    () => DialogBoost = FactoryDefaults.DialogBoost);
        _resets.Add("surround", "Surround level", () => Pct(FactoryDefaults.SurroundLevel), () => Eq(SurroundLevel, FactoryDefaults.SurroundLevel),
                    () => SurroundLevel = FactoryDefaults.SurroundLevel);
        _resets.Add("delay", "Surround delay", () => $"{FactoryDefaults.SurroundDelayMs:0} ms", () => Eq(SurroundDelayMs, FactoryDefaults.SurroundDelayMs),
                    () => SurroundDelayMs = FactoryDefaults.SurroundDelayMs);
        _resets.Add("lfeXover", "LFE crossover", () => $"{FactoryDefaults.LfeCrossoverHz:0} Hz", () => Eq(LfeCrossoverHz, FactoryDefaults.LfeCrossoverHz),
                    () => LfeCrossoverHz = FactoryDefaults.LfeCrossoverHz);
        _resets.Add("lfeLevel", "LFE level", () => $"{FactoryDefaults.LfeLevelDb:0.0} dB", () => Eq(LfeLevelDb, FactoryDefaults.LfeLevelDb),
                    () => LfeLevelDb = FactoryDefaults.LfeLevelDb);
        _resets.Add("lfeReset", "Resume after silence", () => $"{FactoryDefaults.LfeResetSeconds:0} s", () => Eq(LfeResetSeconds, FactoryDefaults.LfeResetSeconds),
                    () => LfeResetSeconds = FactoryDefaults.LfeResetSeconds);
        foreach (var eq in EqChannels)
        {
            foreach (var band in eq.Bands)
            {
                var b = band;
                var def = EqModel.DefaultBand(b.Channel, b.Band);
                var who = $"the {b.Name} of the {eq.Name} EQ";
                _resets.Add(b.FreqResetKey, $"the frequency of {who}", () => def.FreqHz >= 1000 ? $"{def.FreqHz / 1000:0.##} kHz" : $"{def.FreqHz:0} Hz",
                            () => Eq(b.FreqHz, def.FreqHz), () => b.FreqHz = def.FreqHz);
                _resets.Add(b.GainResetKey, $"the gain of {who}", () => Db(def.GainDb), () => Eq(b.GainDb, def.GainDb), () => b.GainDb = def.GainDb);
                _resets.Add(b.QResetKey, $"the Q of {who}", () => $"{def.Q:0.00}", () => Eq(b.Q, def.Q), () => b.Q = def.Q);
            }
        }
    }

    /// <summary>The reset registry (tests, and the EQ dialog's per-channel reset).</summary>
    internal SettingResets Resets => _resets;

    // ----- UI-only settings
    public bool StopEngineOnExit
    {
        get => _settings.Settings.StopEngineOnExit;
        set { _settings.Settings.StopEngineOnExit = value; _settings.Save(); OnPropertyChanged(); }
    }
    public bool StartWithWindows
    {
        get => _settings.Settings.StartWithWindows;
        set
        {
            _settings.Settings.StartWithWindows = value;
            _settings.Save();
            SetAutostart(value);
            OnPropertyChanged();
        }
    }

    // ----- CPU / latency display (Average / Live / Off)
    public StatsDisplayMode StatsMode
    {
        get => _settings.Settings.StatsMode;
        set
        {
            if (_settings.Settings.StatsMode == value) return;
            _settings.Settings.StatsMode = value;
            _settings.Save();
            OnPropertyChanged();
            OnPropertyChanged(nameof(ShowStats));
            OnPropertyChanged(nameof(StatsAverage));
            OnPropertyChanged(nameof(StatsLive));
            OnPropertyChanged(nameof(StatsOff));
        }
    }
    public bool ShowStats => StatsMode != StatsDisplayMode.Off;
    public bool StatsAverage { get => StatsMode == StatsDisplayMode.Average; set { if (value) StatsMode = StatsDisplayMode.Average; } }
    public bool StatsLive { get => StatsMode == StatsDisplayMode.Live; set { if (value) StatsMode = StatsDisplayMode.Live; } }
    public bool StatsOff { get => StatsMode == StatsDisplayMode.Off; set { if (value) StatsMode = StatsDisplayMode.Off; } }

    // ===================================================================== live telemetry (read-only)
    private string _engineState = "Connecting...", _inputFormat = "-", _inputDetail = "", _outputFormat = "-", _outputDetail = "";
    private string _errorText = "", _cpuText = "-", _dspText = "-", _latencyText = "-", _bufferText = "-", _driftText = "-";
    private string _countersText = "-", _analysisText = "-", _lastCommandError = "";
    private string _requestedText = "-", _effectiveStatusText = "", _effectiveReason = "";
    private Brush _statusBrush = IdleBrush, _effectiveBrush = IdleBrush;
    private bool _isPassthrough, _isBitstreamBlocked, _isConnected, _isServiceMode, _speechAvailable, _isInputActive;
    private bool _isAudioStopped, _isEngineDown;

    public string EngineStateText { get => _engineState; private set => Set(ref _engineState, value); }
    public Brush StatusBrush { get => _statusBrush; private set => Set(ref _statusBrush, value); }
    public string InputFormat { get => _inputFormat; private set { if (Set(ref _inputFormat, value)) OnPropertyChanged(nameof(TrayTooltip)); } }
    public string InputDetail { get => _inputDetail; private set => Set(ref _inputDetail, value); }
    public string OutputFormat { get => _outputFormat; private set { if (Set(ref _outputFormat, value)) OnPropertyChanged(nameof(TrayTooltip)); } }
    public string OutputDetail { get => _outputDetail; private set => Set(ref _outputDetail, value); }
    public bool IsPassthrough { get => _isPassthrough; private set => Set(ref _isPassthrough, value); }
    public bool IsBitstreamBlocked { get => _isBitstreamBlocked; private set => Set(ref _isBitstreamBlocked, value); }
    public bool IsInputActive { get => _isInputActive; private set => Set(ref _isInputActive, value); }
    public bool IsConnected { get => _isConnected; private set => Set(ref _isConnected, value); }
    public bool IsServiceMode
    {
        get => _isServiceMode;
        private set { if (Set(ref _isServiceMode, value)) OnPropertyChanged(nameof(CanShutdownEngine)); }
    }
    public bool SpeechAvailable { get => _speechAvailable; private set => Set(ref _speechAvailable, value); }
    public string ErrorText { get => _errorText; private set { if (Set(ref _errorText, value)) OnPropertyChanged(nameof(HasError)); } }
    public bool HasError => !string.IsNullOrEmpty(_errorText);
    public string LastCommandError { get => _lastCommandError; private set => Set(ref _lastCommandError, value); }
    public string CpuText { get => _cpuText; private set => Set(ref _cpuText, value); }
    public string DspText { get => _dspText; private set => Set(ref _dspText, value); }
    public string LatencyText { get => _latencyText; private set => Set(ref _latencyText, value); }
    public string BufferText { get => _bufferText; private set => Set(ref _bufferText, value); }
    public string DriftText { get => _driftText; private set => Set(ref _driftText, value); }
    public string CountersText { get => _countersText; private set => Set(ref _countersText, value); }
    public string AnalysisText { get => _analysisText; private set => Set(ref _analysisText, value); }

    // ----- effective output indicator
    public string RequestedText { get => _requestedText; private set => Set(ref _requestedText, value); }
    public string EffectiveStatusText { get => _effectiveStatusText; private set => Set(ref _effectiveStatusText, value); }
    public Brush EffectiveBrush { get => _effectiveBrush; private set => Set(ref _effectiveBrush, value); }
    public string EffectiveReason
    {
        get => _effectiveReason;
        private set { if (Set(ref _effectiveReason, value)) OnPropertyChanged(nameof(HasEffectiveReason)); }
    }
    public bool HasEffectiveReason => !string.IsNullOrEmpty(_effectiveReason);

    // ----- engine control state
    public bool IsAudioStopped
    {
        get => _isAudioStopped;
        private set { if (Set(ref _isAudioStopped, value)) OnPropertyChanged(nameof(AudioButtonText)); }
    }
    public string AudioButtonText => _isAudioStopped ? "Start audio pipeline" : "Stop audio pipeline";
    /// <summary>The engine process is not running (shut down by the user, crashed, or never started).</summary>
    public bool IsEngineDown
    {
        get => _isEngineDown;
        private set { if (Set(ref _isEngineDown, value)) OnPropertyChanged(nameof(IsEngineUp)); }
    }
    public bool IsEngineUp => !_isEngineDown;
    public bool CanShutdownEngine => !_isServiceMode;

    public string TrayTooltip => _muteAll ? "LegacyAVR - MUTED" : $"LegacyAVR\nIn:  {_inputFormat}\nOut: {_outputFormat}";

    // ----- v1.2.0 live readouts (all from engine telemetry, never inferred from settings)
    private string _processingText = "-", _pcmSwitchText = "", _freqRangeText = "", _gameStatusText = "";
    private string _latencyBreakdown = "", _marginText = "";
    public string ProcessingText { get => _processingText; private set => Set(ref _processingText, value); }
    public string PcmSwitchText
    {
        get => _pcmSwitchText;
        private set { if (Set(ref _pcmSwitchText, value)) OnPropertyChanged(nameof(HasPcmSwitchText)); }
    }
    public bool HasPcmSwitchText => !string.IsNullOrEmpty(_pcmSwitchText);
    /// <summary>Lowest - highest frequency in the input over the last ~7 s.</summary>
    public string FreqRangeText { get => _freqRangeText; private set => Set(ref _freqRangeText, value); }
    public string GameStatusText { get => _gameStatusText; private set => Set(ref _gameStatusText, value); }
    /// <summary>Per-stage latency (multi-line) for the tooltip and the Output tab.</summary>
    public string LatencyBreakdown { get => _latencyBreakdown; private set => Set(ref _latencyBreakdown, value); }
    public string MarginText { get => _marginText; private set => Set(ref _marginText, value); }
    private bool _outputIsPassthrough;
    public bool OutputIsPassthrough { get => _outputIsPassthrough; private set => Set(ref _outputIsPassthrough, value); }

    private static string Hz(float f) => f >= 1000 ? $"{f / 1000:0.0#} kHz" : $"{f:0} Hz";

    // ===================================================================== lifecycle
    public async Task InitialiseAsync()
    {
        _timer.Start();
        _gameTimer.Start();
        await ConnectAndSyncAsync();
    }

    private async Task ConnectAndSyncAsync()
    {
        if (_launching) return;
        _launching = true;
        try
        {
            EngineStateText = "Starting engine...";
            var err = await EngineLauncher.EnsureRunningAsync(_client);
            if (err != null)
            {
                ErrorText = err;
                EngineStateText = "Engine not running";
                StatusBrush = ErrorBrush;
                return;
            }
            await LoadDevicesAsync();
            await LoadStateAsync();
            await RunStartupPolicyAsync();   // v1.4.0: default output + last profile, once per session
        }
        finally
        {
            _launching = false;
        }
    }

    // ----- engine controls
    private async Task ToggleAudioAsync()
    {
        var r = await _client.SendAsync(_isAudioStopped ? "START_AUDIO" : "STOP_AUDIO");
        if (!r.Ok) LastCommandError = r.Message;
    }

    private async Task ShutdownEngineAsync()
    {
        if (_isServiceMode)
        {
            MessageBox.Show("LegacyAVR is running as a Windows service, so the dashboard can't shut it down.\n\n" +
                            "Use \"Stop audio pipeline\" to release the output, or stop the \"LegacyAVR Encoder Engine\" service " +
                            "in Services (or run scripts\\install-service.ps1 -Uninstall).",
                            "LegacyAVR", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        if (MessageBox.Show("Shut down the AVREngine process?\n\nAudio stops until you press \"Start engine\". " +
                            "(To just pause audio and keep the engine ready, use \"Stop audio pipeline\" instead.)",
                            "LegacyAVR", MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK)
            return;
        _userShutdown = true;
        await _client.SendAsync("SHUTDOWN");
    }

    private async Task StartEngineAsync()
    {
        _userShutdown = false;
        _nextReconnect = DateTime.MinValue;
        await ConnectAndSyncAsync();
    }

    public async Task LoadDevicesAsync()
    {
        var r = await _client.SendAsync("LIST_DEVICES");
        if (!r.Ok) { LastCommandError = r.Message; return; }
        var all = new List<DeviceItem>();
        foreach (var line in r.Lines)
        {
            var f = line.Split('\t');
            if (f.Length < 8 || f[0] != "DEV") continue;
            all.Add(new DeviceItem
            {
                IsRender = f[1] == "render",
                Id = f[2],
                Name = f[3],
                IsDefault = f[4] == "1",
                Channels = int.TryParse(f[5], out var ch) ? ch : 0,
                SampleRate = int.TryParse(f[6], out var sr) ? sr : 0,
                IecCaps = uint.TryParse(f[7], out var caps) ? caps : 0,
            });
        }
        _rawDevices = all;
        RebuildDeviceLists();
    }

    /// <summary>
    /// Rebuilds both device lists from the last LIST_DEVICES answer. The output list marks
    /// LegacyAVR's default with "(Default)" and keeps a "(not connected)" entry for a saved
    /// output that is missing, so the box is never silently blank.
    /// </summary>
    private void RebuildDeviceLists()
    {
        var defId = _settings.Settings.DefaultOutputDeviceId;
        var inId = _selectedInput?.Id;
        // The engine's configured output wins (it is kept up to date by user selections, the
        // startup policy and GET_STATE); the current selection only fills in before the first state.
        var outId = !string.IsNullOrEmpty(_engineOutputId) ? _engineOutputId : _selectedOutput is { IsMissing: false } so ? so.Id : null;
        _syncing = true;
        try
        {
            InputDevices.Clear();
            OutputDevices.Clear();
            foreach (var d in _rawDevices.OrderBy(d => d.IsRender ? 0 : 1).ThenBy(d => d.Name)) InputDevices.Add(d);
            foreach (var d in _rawDevices.Where(d => d.IsRender).OrderBy(d => d.Name))
                OutputDevices.Add(d.With(string.Equals(d.Id, defId, StringComparison.OrdinalIgnoreCase)));

            var avail = OutputDevices.Select(d => new OutputInfo(d.Id, d.Name)).ToList();
            var od = StartupPolicy.ResolveOutput(outId, avail, null, null,
                                                 _settings.Settings.LastOutputDeviceId, _settings.Settings.LastOutputDeviceName);
            if (od.MissingId != null)
                OutputDevices.Insert(0, new DeviceItem
                {
                    Id = od.MissingId, Name = od.MissingName ?? "Saved output device", IsRender = true, IsMissing = true,
                    IsAppDefault = string.Equals(od.MissingId, defId, StringComparison.OrdinalIgnoreCase),
                });
            SetSelected(ref _selectedInput, InputDevices.FirstOrDefault(d => d.Id == inId), nameof(SelectedInputDevice));
            SetSelected(ref _selectedOutput, OutputDevices.FirstOrDefault(d => string.Equals(d.Id, outId, StringComparison.OrdinalIgnoreCase)),
                        nameof(SelectedOutputDevice));
            OutputNotice = !string.IsNullOrEmpty(od.Message) ? od.Message : _startupOutputNotice;
        }
        finally
        {
            _syncing = false;
        }
        UpdateCodecSupport();
        OnPropertyChanged(nameof(ShowDefaultOutputButton));
        OnPropertyChanged(nameof(DefaultOutputText));
        OnPropertyChanged(nameof(ProfilesTitle));
        if (_onlyThisOutput) RefreshProfiles(_selectedProfile?.Name);
    }

    /// <summary>Sets a selection without sending anything (the engine already has it).</summary>
    private void SetSelected(ref DeviceItem? field, DeviceItem? value, string name)
    {
        field = value;
        OnPropertyChanged(name);
    }

    public async Task LoadStateAsync()
    {
        var r = await _client.SendAsync("GET_STATE");
        if (!r.Ok) return;
        var kv = r.KeyValues();
        string S(string k, string def = "") => kv.TryGetValue(k, out var v) ? v : def;
        double D(string k, double def) => double.TryParse(S(k), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : def;
        bool B(string k) => S(k) == "1";

        _syncing = true;
        try
        {
            var inId = S("input_device");
            SelectedInputDevice = inId == "default"
                ? InputDevices.FirstOrDefault(d => d.IsRender && d.IsDefault)
                : InputDevices.FirstOrDefault(d => d.Id == inId);
            _engineOutputId = S("output_device");   // shown by RebuildDeviceLists below, even if not connected

            var codec = Enum.TryParse<CodecKind>(S("codec"), true, out var c) ? c : CodecKind.AC3;
            SelectedCodec = CodecOptions.First(o => o.Value == codec);
            uint br = uint.TryParse(S("bitrate"), out var b) ? b : 0;
            SelectedBitrate = BitrateOptions.FirstOrDefault(o => o.Value == br) ?? BitrateOptions.FirstOrDefault();
            SelectedOutputMode = OutputModeOptions.First(o => o.Value == (S("output_mode") == "SHARED" ? OutputModeKind.Shared : OutputModeKind.Exclusive));
            SelectedUpmix = UpmixOptions.FirstOrDefault(o => ProfileStore.UpmixToken(o.Value) == S("upmix")) ?? UpmixOptions[2];
            SelectedBufferMs = (int)D("buffer_ms", 0);
            BassDb = D("bass_db", 0);
            DedicatedSub = S("dedicated_sub", "1") != "0";
            SelectedLfeGen = LfeGenOptions.FirstOrDefault(o => ProfileStore.LfeGenToken(o.Value) == S("lfe_gen", "AUTO")) ?? LfeGenOptions[0];
            SelectedGaming = GamingOptions.FirstOrDefault(o => ProfileStore.GamingToken(o.Value) == S("gaming", "AUTO")) ?? GamingOptions[0];
            GameLfeGen = B("game_lfe_gen");
            LfeResetSeconds = D("lfe_reset_s", 10);
            NightMode = B("night_mode");
            StereoAs20 = B("stereo_as_20");
            MuteAll = B("mute_all");
            MasterDb = D("master_db", 0);
            CentreLevel = D("centre_level", 0.85);
            CentreFloor = D("centre_floor", 0.3);
            DialogBoost = D("dialog_boost", 0.6);
            SurroundLevel = D("surround_level", 0.8);
            SurroundDelayMs = D("surround_delay_ms", 12);
            LfeCrossoverHz = D("lfe_crossover_hz", 80);
            LfeLevelDb = D("lfe_level_db", -10);
            foreach (var ch in Channels)
                ch.SetFromEngine(D($"gain{ch.Index}", 0), B($"mute{ch.Index}"));
            // v1.4.0 custom EQ
            EqEnabled = B("eq_enabled");
            foreach (var eq in EqChannels)
            {
                eq.SetEnabledFromEngine(S($"eq{eq.Index}_on", "1") != "0");
                foreach (var band in eq.Bands)
                    band.SetFromModel(EqModel.ParseBand(S($"eq{eq.Index}_b{band.Band}")) ?? EqModel.DefaultBand(eq.Index, band.Band));
            }
            IsServiceMode = B("service_mode");
            IsAudioStopped = S("audio_enabled", "1") == "0";
        }
        finally
        {
            _syncing = false;
        }
        RebuildDeviceLists();
    }

    private void Send(string command)
    {
        _ = Task.Run(async () =>
        {
            var r = await _client.SendAsync(command);
            if (!r.Ok) Application.Current?.Dispatcher.BeginInvoke(() => LastCommandError = r.Message);
        });
    }

    // ===================================================================== profiles (v1.4.0)
    // One list replaces the presets and the device profiles of 1.0-1.3. A profile stores
    // everything (output device, encoder, processing, speakers, trims, mutes, bass, LFE, EQ);
    // profiles converted from old presets only store what the preset had.
    public event EventHandler? ProfilesChanged;
    public IReadOnlyList<Profile> AllProfiles => _profiles.Profiles;

    private string _newProfileName = string.Empty;
    public string NewProfileName { get => _newProfileName; set => Set(ref _newProfileName, value); }
    private string? _activeProfile;
    public string? ActiveProfileName { get => _activeProfile; private set => Set(ref _activeProfile, value); }
    private Profile? _selectedProfile;
    public Profile? SelectedProfile
    {
        get => _selectedProfile;
        set { if (Set(ref _selectedProfile, value)) OnPropertyChanged(nameof(SelectedProfileDetails)); }
    }
    private string _profileStatus = string.Empty;
    public string ProfileStatus { get => _profileStatus; private set => Set(ref _profileStatus, value); }

    private bool _onlyThisOutput;
    /// <summary>List only profiles made for the selected output (plus the "any output" ones).</summary>
    public bool OnlyThisOutputProfiles
    {
        get => _onlyThisOutput;
        set { if (Set(ref _onlyThisOutput, value)) RefreshProfiles(_selectedProfile?.Name); }
    }
    public string ProfilesTitle => _selectedOutput is { IsMissing: false } o
        ? $"New profiles are saved for {o.Name}."
        : "Pick the output device first - new profiles remember it.";

    public bool ApplyLastProfileAtStartup
    {
        get => _settings.Settings.ApplyLastProfileAtStartup;
        set { _settings.Settings.ApplyLastProfileAtStartup = value; _settings.Save(); OnPropertyChanged(); }
    }

    /// <summary>Description, device and what the profile stores - shown under the list.</summary>
    public string SelectedProfileDetails
    {
        get
        {
            if (_selectedProfile is not { } p) return string.Empty;
            var device = p.HasDevice ? $"Made for {p.DeviceName ?? p.DeviceId}." : "Works on any output.";
            var scope = p.IsComplete
                ? "Stores every setting, including speakers, trims, mutes and EQ."
                : "Partial (converted from an older preset): settings it does not store, such as speaker levels or EQ, are left as they are.";
            return string.Join(" ", new[] { p.Description, device, scope }.Where(s => !string.IsNullOrWhiteSpace(s)));
        }
    }

    private void RefreshProfiles(string? select = null)
    {
        var outId = _selectedOutput is { IsMissing: false } o ? o.Id : null;
        var list = _profiles.Profiles.Where(p => !_onlyThisOutput || !p.HasDevice ||
                                                 string.Equals(p.DeviceId, outId, StringComparison.OrdinalIgnoreCase));
        Profiles.Clear();
        foreach (var p in list) Profiles.Add(p);
        SelectedProfile = select is null ? null : Profiles.FirstOrDefault(p => string.Equals(p.Name, select, StringComparison.OrdinalIgnoreCase));
        ProfilesChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>A complete profile of the current settings (for the selected output).</summary>
    public Profile CaptureCurrent(string name, string? description = null)
    {
        var dev = _selectedOutput is { IsMissing: false } o ? o : null;
        return new Profile
        {
            Name = name, Description = description, DeviceId = dev?.Id, DeviceName = dev?.Name,
            Codec = _codec?.Value ?? CodecKind.AC3, Bitrate = _bitrate?.Value ?? 0, OutputMode = _outputMode?.Value ?? OutputModeKind.Exclusive,
            BufferMs = _bufferMs, Upmix = _upmix?.Value ?? UpmixKind.Upmix51, StereoAs20 = _stereoAs20, NightMode = _nightMode,
            LfeGen = _lfeGen?.Value ?? LfeGenKind.Auto, Gaming = _gaming?.Value ?? GamingKind.Auto, GameLfeGen = _gameLfeGen,
            LfeResetSeconds = _lfeResetSeconds, BassDb = _bassDb, CentreLevel = _centre, CentreFloor = _centreFloor,
            DialogBoost = _dialog, SurroundLevel = _surround, SurroundDelayMs = _delay, DedicatedSub = _dedicatedSub,
            LfeCrossoverHz = _lfeXover, LfeLevelDb = _lfeLevel, MasterDb = _masterDb,
            GainsDb = Channels.Select(c => c.GainDb).ToArray(), Mutes = Channels.Select(c => c.IsMuted).ToArray(),
            Eq = CurrentEq(),
        };
    }

    public EqSettings CurrentEq() => new(_eqEnabled, EqChannels.Select(c => c.ToModel()).ToArray());

    /// <summary>
    /// Applies a profile, asking what to do when it was made for another output:
    /// switch to that output (if connected), or apply it to the output selected now.
    /// </summary>
    private async Task ApplyProfileInteractiveAsync(Profile p)
    {
        var avail = OutputDevices.Where(d => !d.IsMissing).Select(d => d.Id).ToList();
        var current = _selectedOutput is { IsMissing: false } o ? o : null;
        string? switchTo = null;
        switch (ProfileStore.TargetOf(p, current?.Id, avail))
        {
            case ProfileTarget.OtherOutput:
            {
                var r = MessageBox.Show(
                    $"\"{p.Name}\" was made for {p.DeviceName}.\n\nYes: switch the output to {p.DeviceName} and apply it.\n" +
                    $"No: apply it to the current output ({current?.Name ?? "none selected"}).",
                    "LegacyAVR", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
                if (r == MessageBoxResult.Cancel) return;
                if (r == MessageBoxResult.Yes) switchTo = p.DeviceId;
                break;
            }
            case ProfileTarget.OutputMissing:
                if (current is null)
                {
                    ProfileStatus = $"\"{p.Name}\" was made for {p.DeviceName}, which is not connected. Select an output first.";
                    return;
                }
                if (MessageBox.Show($"\"{p.Name}\" was made for {p.DeviceName}, which is not connected.\n\n" +
                                    $"Apply it to the current output ({current.Name}) instead?",
                                    "LegacyAVR", MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK) return;
                break;
        }
        await ApplyProfileAsync(p, switchTo);
    }

    /// <summary>Applies a profile without asking. Only settings that differ are sent, so the
    /// output is re-opened only when something that needs it actually changes.</summary>
    public async Task<bool> ApplyProfileAsync(Profile p, string? switchToDeviceId)
    {
        var cmds = ProfileStore.ChangedCommands(p, CaptureCurrent("current"), switchToDeviceId);
        var err = await ProfileStore.SendAsync(_client, cmds);
        if (err != null)
        {
            LastCommandError = err;
            ProfileStatus = $"\"{p.Name}\" was not fully applied: {err}";
            return false;
        }
        ActiveProfileName = p.Name;
        _settings.Settings.LastProfile = p.Name;
        _settings.Save();
        var where = switchToDeviceId != null ? $" and switched the output to {p.DeviceName}"
                  : p.HasDevice && !string.Equals(p.DeviceId, _selectedOutput?.Id, StringComparison.OrdinalIgnoreCase)
                      ? $" to {_selectedOutput?.Name ?? "the current output"}" : string.Empty;
        ProfileStatus = cmds.Count == 0 ? $"\"{p.Name}\" is already in use - nothing changed." : $"Applied \"{p.Name}\"{where}.";
        ProfilesChanged?.Invoke(this, EventArgs.Empty);
        if (switchToDeviceId != null) await LoadDevicesAsync();
        await LoadStateAsync();
        return true;
    }

    /// <summary>Tray menu: apply by name, asking only when unavoidable.</summary>
    public Task ApplyProfileFromTrayAsync(Profile p) => ApplyProfileInteractiveAsync(p);

    private void OverwriteSelectedProfile()
    {
        if (SelectedProfile is not { } sel) return;
        var dev = _selectedOutput is { IsMissing: false } o ? o.Name : "no output";
        if (!Confirm($"Overwrite \"{sel.Name}\" with the current settings (output: {dev})?")) return;
        var updated = CaptureCurrent(sel.Name, sel.Description);
        _profiles.Upsert(updated);
        RefreshProfiles(updated.Name);
        ProfileStatus = $"Saved \"{updated.Name}\"";
    }

    private void SaveAsNewProfile()
    {
        var name = NewProfileName.Trim();
        if (name.Length == 0) return;
        if (_profiles.Find(name) is not null && !Confirm($"A profile called \"{name}\" already exists. Overwrite it?")) return;
        var p = CaptureCurrent(name);
        _profiles.Upsert(p);
        RefreshProfiles(p.Name);
        NewProfileName = string.Empty;
        ProfileStatus = p.HasDevice ? $"Saved \"{p.Name}\" for {p.DeviceName}" : $"Saved \"{p.Name}\"";
    }

    private void DeleteSelectedProfile()
    {
        if (SelectedProfile is not { } sel) return;
        if (!Confirm($"Delete profile \"{sel.Name}\"?\n\n(The five starting profiles can be brought back with \"Restore defaults\".)")) return;
        _profiles.Delete(sel.Name);
        if (string.Equals(_settings.Settings.LastProfile, sel.Name, StringComparison.OrdinalIgnoreCase))
        {
            _settings.Settings.LastProfile = null;
            _settings.Save();
            ActiveProfileName = null;
        }
        RefreshProfiles(null);
        ProfileStatus = $"Deleted \"{sel.Name}\"";
    }

    private void RestoreDefaultProfiles()
    {
        _profiles.RestoreDefaults();
        RefreshProfiles(SelectedProfile?.Name);
        ProfileStatus = "Starting profiles restored (your own profiles are kept).";
    }

    private void ImportProfiles()
    {
        var dlg = new OpenFileDialog
        {
            Filter = "LegacyAVR profiles and presets (*.json)|*.json|All files (*.*)|*.*",
            Title = "Import profiles (also reads presets and device profiles from older versions)",
        };
        if (dlg.ShowDialog() != true) return;
        try
        {
            var r = _profiles.Import(dlg.FileName);
            RefreshProfiles(r.Names.FirstOrDefault());
            ProfileStatus = (r.Added == 1 ? "Imported 1 profile" : $"Imported {r.Added} profiles") +
                            (r.Renamed > 0 ? $" ({r.Renamed} renamed because the name was already used)." : ".") +
                            " A profile made on another PC can be applied to your current output.";
        }
        catch (Exception ex)
        {
            MessageBox.Show("Could not import profiles:\n" + ex.Message, "LegacyAVR", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void ExportProfiles()
    {
        // The selected profile, or all of them when none is selected.
        var items = SelectedProfile is { } sel ? new List<Profile> { sel } : _profiles.Profiles.ToList();
        var file = SelectedProfile is { } s1 ? $"LegacyAVR-{string.Concat(s1.Name.Split(System.IO.Path.GetInvalidFileNameChars()))}.json" : "LegacyAVR-profiles.json";
        var dlg = new SaveFileDialog { Filter = "LegacyAVR profiles (*.json)|*.json", FileName = file, Title = "Export profiles" };
        if (dlg.ShowDialog() != true) return;
        try
        {
            ProfileStore.Export(dlg.FileName, items);
            ProfileStatus = items.Count == 1 ? $"Exported \"{items[0].Name}\"" : $"Exported {items.Count} profiles";
        }
        catch (Exception ex)
        {
            MessageBox.Show("Could not export profiles:\n" + ex.Message, "LegacyAVR", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    // ===================================================================== startup (v1.4.0)
    /// <summary>
    /// Once per dashboard session, after the first successful connection: select LegacyAVR's
    /// default output, then re-apply the last profile when that is enabled and possible.
    /// Problems (device not connected...) are reported, never solved by picking another device.
    /// </summary>
    private async Task RunStartupPolicyAsync()
    {
        if (_startupDone) return;
        _startupDone = true;
        var notes = new List<string>();
        if (!string.IsNullOrEmpty(ProfileStatus)) notes.Add(ProfileStatus);   // migration note

        var avail = _rawDevices.Where(d => d.IsRender).Select(d => new OutputInfo(d.Id, d.Name)).ToList();
        var od = StartupPolicy.ResolveOutput(_engineOutputId, avail, _settings.Settings.DefaultOutputDeviceId,
                                             _settings.Settings.DefaultOutputDeviceName,
                                             _settings.Settings.LastOutputDeviceId, _settings.Settings.LastOutputDeviceName);
        _startupOutputNotice = od.Message;
        if (!string.IsNullOrEmpty(od.Message)) OutputNotice = od.Message;
        if (od.SwitchToId != null)
        {
            var r = await _client.SendAsync($"SET_OUTPUT_DEVICE {Proto.Quote(od.SwitchToId)}");
            if (r.Ok) { _engineOutputId = od.SwitchToId; RebuildDeviceLists(); }
            else notes.Add("Could not select the default output: " + r.Message);
        }

        var pd = StartupPolicy.ResolveLastProfile(_settings.Settings.ApplyLastProfileAtStartup, _settings.Settings.LastProfile,
                                                  _profiles, _engineOutputId, avail.Select(a => a.Id));
        if (pd.Action == StartupPolicy.ProfileAction.Apply && pd.Profile != null)
        {
            if (await ApplyProfileAsync(pd.Profile, pd.SwitchToId)) notes.Clear();   // ApplyProfileAsync wrote its own status
        }
        else if (pd.Action == StartupPolicy.ProfileAction.Report)
        {
            notes.Add(pd.Message);
        }
        if (notes.Count > 0) ProfileStatus = string.Join(" ", notes);
    }

    // ===================================================================== telemetry tick (UI thread, 30 Hz)
    private void OnTick()
    {
        if (!_reader.IsOpen && !_reader.TryOpen())
        {
            ShowDisconnected();
            return;
        }
        if (!_reader.TryRead(out var t) || _reader.IsStale)
        {
            if (_reader.IsStale) _reader.Close();
            ShowDisconnected();
            return;
        }

        IsConnected = true;
        IsEngineDown = false;
        var flags = (TelemetryFlags)t.Flags;
        var state = (EngineState)t.EngineState;
        IsAudioStopped = flags.HasFlag(TelemetryFlags.AudioStopped);
        EngineStateText = IsAudioStopped ? "Stopped (audio off)" : state switch
        {
            EngineState.Running => "Running",
            EngineState.Idle => "Idle - waiting for audio",
            EngineState.Starting => "Starting...",
            EngineState.Reconfiguring => "Reconfiguring...",
            EngineState.Error => "Error",
            _ => "Stopped",
        };
        StatusBrush = IsAudioStopped ? WarnBrush : state switch
        {
            EngineState.Running => GoodBrush,
            EngineState.Idle => IdleBrush,
            EngineState.Error => ErrorBrush,
            _ => WarnBrush,
        };

        InputFormat = t.InputDescriptionText;
        InputDetail = t.InSampleRate == 0 ? "" :
            $"{t.InputDeviceText}  |  endpoint {t.InChannels}ch {t.InBitsPerSample}-bit {t.InSampleRate / 1000.0:0.#} kHz";
        OutputFormat = t.OutputDescriptionText;
        OutputDetail = IsAudioStopped ? "Output device released" :
            t.OutputDeviceText + ((OutputModeKind)t.OutMode == OutputModeKind.Exclusive ? "  |  WASAPI Exclusive" : "  |  WASAPI Shared");
        IsPassthrough = flags.HasFlag(TelemetryFlags.Passthrough);
        IsBitstreamBlocked = flags.HasFlag(TelemetryFlags.BitstreamBlocked);
        IsInputActive = flags.HasFlag(TelemetryFlags.InputActive);
        IsServiceMode = flags.HasFlag(TelemetryFlags.ServiceMode);
        SpeechAvailable = flags.HasFlag(TelemetryFlags.SpeechAvailable);
        ErrorText = t.LastErrorText;

        UpdateEffective(t);
        UpdateStats(t.CpuPercent, t.LatencyMs, state is EngineState.Running or EngineState.Idle && !IsAudioStopped);

        DspText = $"{t.DspLoadPercent:0.0} % of block";
        BufferText = $"in {t.CaptureBufferMs:0} ms / out {t.OutputBufferMs:0} ms";
        DriftText = $"{t.DriftPpm:+0;-0;0} ppm";
        CountersText = $"{t.Underruns} underruns  |  {t.Overruns} overruns  |  {t.InputDrops} drops";
        AnalysisText = $"L/R corr {t.Correlation:+0.00;-0.00}  |  dialogue {t.DialogActivity * 100:0}%  |  " +
                       $"night GR {t.NightGainReductionDb:0.0} dB  |  limiter {t.LimiterReductionDb:0.0} dB";

        if (_muteAll != flags.HasFlag(TelemetryFlags.MuteAll))
        {
            _syncing = true;
            MuteAll = flags.HasFlag(TelemetryFlags.MuteAll);
            _syncing = false;
        }

        UpdateV12(t, flags);

        unsafe
        {
            UpdateInputMeterNames(t.InChannelMask, t.InChannels);
            for (int i = 0; i < 8; i++)
            {
                InputMeters[i].PeakDb = t.InPeakDb[i];
                InputMeters[i].RmsDb = t.InRmsDb[i];
            }
            UpdateOutputMeterNames(t.EmitMask, t.EmitChannels);
            for (int i = 0; i < 8; i++)
            {
                OutputMeters[i].PeakDb = t.EmitPeakDb[i];
                OutputMeters[i].RmsDb = t.EmitRmsDb[i];
            }
            bool noSub = !t.HasFlag2(TelemetryFlags2.DedicatedSub);
            for (int i = 0; i < 6; i++)
            {
                var ch = Channels[i];
                bool foldedLfe = noSub && i == 3;   // no subwoofer: show what is folded into the fronts
                ch.PeakDb = foldedLfe ? t.LfeFoldPeakDb : t.OutPeakDb[i];
                ch.RmsDb = foldedLfe ? t.LfeFoldRmsDb : t.OutRmsDb[i];
                ch.IsActive = t.OutChannels != 2 || i < 2 || (TestSignalKind)t.TestSignal != TestSignalKind.Off;
                ch.IsTestTarget = t.TestChannel == i;
            }
        }
        Diagnostics.UpdateFromTelemetry(t);

        if (t.ConfigRevision != _lastConfigRevision)
        {
            _lastConfigRevision = t.ConfigRevision;
            if ((DateTime.UtcNow - _throttle.LastLocalChangeUtc).TotalSeconds > 1.5) _ = LoadStateAsync();
        }
    }

    private void UpdateEffective(in TelemetryBlock t)
    {
        var reqCodec = (CodecKind)t.RequestedCodec;
        var reqMode = (OutputModeKind)t.RequestedMode;
        string codecTxt = reqCodec switch
        {
            CodecKind.AC3 => "Dolby Digital",
            CodecKind.DTS => "DTS",
            CodecKind.EAC3 => "Dolby Digital Plus",
            _ => "PCM",
        };
        string rate = t.RequestedBitrate == 0 ? "" :
            t.RequestedBitrate % 1000 == 0 ? $" {t.RequestedBitrate / 1000} kbps" : $" {t.RequestedBitrate / 1000.0:0.##} kbps";
        RequestedText = $"{codecTxt}{rate}  |  {(reqMode == OutputModeKind.Exclusive ? "Exclusive" : "Shared")}";

        var status = (OutputStatusKind)t.OutputStatus;
        (EffectiveStatusText, EffectiveBrush) = status switch
        {
            OutputStatusKind.Match => ("AS REQUESTED", GoodBrush),
            OutputStatusKind.FallbackShared => ("FALLBACK", WarnBrush),
            OutputStatusKind.BitstreamPassthrough => ("PASSTHROUGH", InfoBrush),
            OutputStatusKind.StereoPcmDirect => ("LOSSLESS STEREO", InfoBrush),
            OutputStatusKind.StoppedByUser => ("STOPPED", IdleBrush),
            _ => ("ERROR", ErrorBrush),
        };
        EffectiveReason = t.EffectiveReasonText;
    }

    private void UpdateStats(float cpu, float latency, bool running)
    {
        if (running)
        {
            _statHistory.Enqueue((cpu, latency));
            _cpuSum += cpu;
            _latSum += latency;
            while (_statHistory.Count > StatWindow)
            {
                var (c, l) = _statHistory.Dequeue();
                _cpuSum -= c;
                _latSum -= l;
            }
        }
        if (_statHistory.Count == 0 || StatsMode == StatsDisplayMode.Off)
        {
            CpuText = "-";
            LatencyText = "-";
            return;
        }
        if (StatsMode == StatsDisplayMode.Live)
        {
            CpuText = $"{cpu:0.00} %";
            LatencyText = $"{latency:0} ms";
        }
        else
        {
            CpuText = $"{_cpuSum / _statHistory.Count:0.0} %";
            LatencyText = $"{_latSum / _statHistory.Count:0} ms";
        }
    }

    // ===================================================================== v1.2.0 telemetry
    private void UpdateV12(in TelemetryBlock t, TelemetryFlags flags)
    {
        bool running = !IsAudioStopped && (EngineState)t.EngineState is EngineState.Running or EngineState.Idle;
        ProcessingText = !running ? (IsAudioStopped ? "Audio pipeline stopped" : "-") : t.ProcessingTextValue;
        PcmSwitchText = running ? t.PcmSwitchTextValue : string.Empty;
        OutputIsPassthrough = running && t.EmitChannels == 0 && flags.HasFlag(TelemetryFlags.Passthrough);

        FreqRangeText = !running || !flags.HasFlag(TelemetryFlags.InputActive) || t.FreqLowHz <= 0 || t.FreqHighHz <= 0
            ? "Frequency range: -"
            : $"Frequency range (last ~7 s): {Hz(t.FreqLowHz)} - {Hz(t.FreqHighHz)}";

        bool gaming = t.HasFlag2(TelemetryFlags2.GamingActive);
        if (_gamingActive != gaming)
        {
            _gamingActive = gaming;
            OnPropertyChanged(nameof(IsGeneralLfeGenEnabled));
            OnPropertyChanged(nameof(LfeOverrideText));
        }
        var game = t.GameNameText;

        // LFE status indicator + output layout in the subwoofer switch label
        var lfe = (LfeStatusKind)t.LfeStatus;
        LfeStatusText = running ? t.LfeStatusTextValue : "-";
        LfeStatusBrush = !running ? IdleBrush : lfe switch
        {
            LfeStatusKind.Source => GoodBrush,
            LfeStatusKind.Generated or LfeStatusKind.Upmixed => InfoBrush,
            LfeStatusKind.Inhibited => WarnBrush,
            _ => IdleBrush,
        };
        var layout = running ? OutputLayout(t.EmitMask, t.EmitChannels, t.HasFlag2(TelemetryFlags2.DedicatedSub)) : string.Empty;
        if (running && t.EmitChannels == 0 && flags.HasFlag(TelemetryFlags.Passthrough)) layout = "passthrough";
        if (layout != _outputLayout)
        {
            _outputLayout = layout;
            OnPropertyChanged(nameof(SubToggleText));
        }
        GameStatusText = (GamingKind)t.GamingMode switch
        {
            GamingKind.Off => "Game mode off: stereo games are upmixed / switched like any other source.",
            GamingKind.On => "Game mode always on: stereo is never upmixed and the output format is kept.",
            _ => gaming
                ? $"Game detected{(string.IsNullOrEmpty(game) ? "" : ": " + game)} - stereo is kept as is, no format switching."
                : "No full-screen game in the foreground.",
        };

        if (running)
        {
            LatencyBreakdown =
                $"Windows mixer + loopback   {t.LatCaptureMs,5:0.0} ms\n" +
                $"Capture queue              {t.LatCaptureQueueMs,5:0.0} ms\n" +
                $"Resampler                  {t.LatResamplerMs,5:0.0} ms\n" +
                $"1536-sample frame (avg)    {t.LatAssemblyMs,5:0.0} ms\n" +
                $"Output queue (avg)         {t.LatOutputQueueMs,5:0.0} ms\n" +
                $"Encoder look-ahead         {t.LatEncoderMs,5:0.0} ms\n" +
                $"WASAPI buffer              {t.LatDeviceBufferMs,5:0.0} ms\n" +
                $"Driver / stream            {t.LatDeviceStreamMs,5:0.0} ms\n" +
                $"Total (before the AVR)     {t.LatencyMs,5:0.0} ms";
            bool auto = t.HasFlag2(TelemetryFlags2.BufferAuto);
            MarginText = $"In use: {t.LatMarginMs:0.0} ms margin ({(auto ? "automatic" : "your setting")}; lowest safe here " +
                         $"{t.LatFloorMs:0.0} ms = {t.LatCapturePacketMs:0} ms capture packet + half the {t.LatRenderPeriodMs:0.#} ms " +
                         $"output period + 2 ms).  Expected total now: about {t.LatencyMs:0} ms before the AVR.";
        }
        else
        {
            LatencyBreakdown = "Latency details appear while audio is running.";
            MarginText = string.Empty;
        }

        // v1.4.0 custom EQ headroom (shown in the EQ dialog and the Bass card)
        float headroom = running ? t.EqHeadroomDb : 0f;
        if (Math.Abs(headroom - _eqHeadroomDb) > 0.05f)
        {
            _eqHeadroomDb = headroom;
            OnPropertyChanged(nameof(EqStatusText));
            OnPropertyChanged(nameof(EqHeadroomText));
        }

        if (_dedicatedSub != t.HasFlag2(TelemetryFlags2.DedicatedSub) && (DateTime.UtcNow - _subChangedUtc).TotalSeconds > 1.5)
        {
            _syncing = true;
            DedicatedSub = t.HasFlag2(TelemetryFlags2.DedicatedSub);
            _syncing = false;
        }
    }

    private static readonly string[] OutNames71 = { "FL", "FR", "C", "LFE", "BL", "BR", "SL", "SR" };

    private void UpdateOutputMeterNames(uint mask, uint channels)
    {
        if (mask == _lastEmitMask && channels == _lastEmitChannels) return;
        _lastEmitMask = mask;
        _lastEmitChannels = channels;
        var names = new List<string>();
        for (int bit = 0; bit < SpeakerNames.Length && names.Count < (int)channels; bit++)
            if ((mask & (1u << bit)) != 0) names.Add(SpeakerNames[bit]);
        OutputMeters.Clear();
        for (int i = 0; i < 8; i++)
            OutputMeters.Add(new InputMeterViewModel(i < names.Count ? names[i] : i < channels ? OutNames71[i] : "-")
            {
                IsPresent = i < channels && !(IsNoSub && i < names.Count && names[i] == "LFE"),
            });
    }

    // ===================================================================== game detection (Auto)
    private void OnGameTick()
    {
        if (!IsConnected || _gaming?.Value != GamingKind.Auto)
        {
            if (_gameReported && IsConnected) ReportGame(false, string.Empty);
            return;
        }
        var (isGame, name) = GameDetector.Probe();
        // Heartbeat while a game is in front (the engine forgets it after 6 s), one "0" when it leaves.
        if (isGame && (!_gameReported || (DateTime.UtcNow - _lastGameBeat).TotalSeconds >= 2)) ReportGame(true, name);
        else if (!isGame && _gameReported) ReportGame(false, string.Empty);
    }

    private void ReportGame(bool on, string name)
    {
        _gameReported = on;
        _lastGameBeat = DateTime.UtcNow;
        Send(on ? $"GAME_DETECTED 1 {Proto.Quote(name)}" : "GAME_DETECTED 0");
    }

    private void UpdateInputMeterNames(uint mask, int channels)
    {
        if (mask == _lastInMask) return;
        _lastInMask = mask;
        var names = new List<string>();
        for (int bit = 0; bit < SpeakerNames.Length && names.Count < channels; bit++)
            if ((mask & (1u << bit)) != 0) names.Add(SpeakerNames[bit]);
        InputMeters.Clear();
        for (int i = 0; i < 8; i++)
            InputMeters.Add(new InputMeterViewModel(i < names.Count ? names[i] : i < channels ? $"Ch{i + 1}" : "-")
            { IsPresent = i < channels });
    }

    private void ShowDisconnected()
    {
        IsConnected = false;
        IsEngineDown = true;
        EngineStateText = _userShutdown ? "Engine shut down" : "Engine not running";
        StatusBrush = _userShutdown ? IdleBrush : ErrorBrush;
        InputFormat = "-";
        OutputFormat = _userShutdown ? "Engine shut down - press Start engine" : "-";
        RequestedText = "-";
        EffectiveStatusText = "OFF";
        EffectiveBrush = IdleBrush;
        EffectiveReason = string.Empty;
        CpuText = "-";
        LatencyText = "-";
        _statHistory.Clear();
        _cpuSum = _latSum = 0;
        foreach (var c in Channels) { c.PeakDb = -96; c.RmsDb = -96; }
        if (!_userShutdown && DateTime.UtcNow > _nextReconnect)
        {
            _nextReconnect = DateTime.UtcNow.AddSeconds(5);
            _ = ConnectAndSyncAsync();
        }
    }

    // ===================================================================== helpers
    private static Brush Frozen(byte r, byte g, byte b)
    {
        var br = new SolidColorBrush(Color.FromRgb(r, g, b));
        br.Freeze();
        return br;
    }

    private static void Shell(string target)
    {
        try { Process.Start(new ProcessStartInfo(target) { UseShellExecute = true }); }
        catch (Exception) { /* nothing useful to do */ }
    }

    private static void SetAutostart(bool enable)
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run", writable: true);
        if (key is null) return;
        if (enable) key.SetValue("LegacyAVR", $"\"{Environment.ProcessPath}\" --minimized");
        else key.DeleteValue("LegacyAVR", throwOnMissingValue: false);
    }

    public void Dispose()
    {
        _timer.Stop();
        _gameTimer.Stop();
    }
}
