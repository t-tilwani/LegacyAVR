using System;
using System.Globalization;

namespace LegacyAVR.UI.ViewModels;

/// <summary>One output speaker (FL FR C LFE SL SR): meter + gain + mute.</summary>
public sealed class ChannelStripViewModel : ObservableObject
{
    public const double MuteDb = -48.5;   // slider minimum == muted (engine kGainMuteDb)
    public const double MinDb = -48.0;
    public const double MaxDb = 6.0;

    private readonly Action<ChannelStripViewModel> _gainChanged;
    private readonly Action<ChannelStripViewModel> _muteChanged;
    private double _peakDb = -96, _rmsDb = -96, _gainDb;
    private bool _isMuted, _isActive, _isTestTarget;

    public ChannelStripViewModel(int index, string shortName, string longName,
                                 Action<ChannelStripViewModel> gainChanged, Action<ChannelStripViewModel> muteChanged)
    {
        Index = index;
        _shortName = shortName;
        _longName = longName;
        _gainChanged = gainChanged;
        _muteChanged = muteChanged;
    }

    public int Index { get; }
    /// <summary>v1.4.0: reset-button key of this strip's trim (MainViewModel.ResetCommand).</summary>
    public string ResetKey => $"gain{Index}";
    /// <summary>v1.4.0: this channel's custom EQ (set by MainViewModel).</summary>
    public ChannelEqViewModel? Eq { get; internal set; }
    private string _shortName = string.Empty, _longName = string.Empty;
    public string ShortName { get => _shortName; private set => Set(ref _shortName, value); }
    public string LongName { get => _longName; private set => Set(ref _longName, value); }

    /// <summary>v1.2.0: the LFE strip is relabelled when there is no subwoofer.</summary>
    public void Relabel(string shortName, string longName)
    {
        ShortName = shortName;
        LongName = longName;
    }

    public double PeakDb { get => _peakDb; set => Set(ref _peakDb, value); }
    public double RmsDb { get => _rmsDb; set => Set(ref _rmsDb, value); }

    public double GainDb
    {
        get => _gainDb;
        set
        {
            var v = Math.Clamp(value, MuteDb, MaxDb);
            v = v <= MinDb - 0.25 ? MuteDb : Math.Round(v * 2) / 2;   // 0.5 dB steps, bottom = mute
            if (Set(ref _gainDb, v))
            {
                OnPropertyChanged(nameof(GainText));
                if (!Suppress) _gainChanged(this);
            }
        }
    }

    public string GainText => _gainDb <= MuteDb ? "-inf" : _gainDb.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture) + " dB";

    public bool IsMuted
    {
        get => _isMuted;
        set { if (Set(ref _isMuted, value) && !Suppress) _muteChanged(this); }
    }

    /// <summary>Speaker carries signal in the current layout (dims inactive strips).</summary>
    public bool IsActive { get => _isActive; set => Set(ref _isActive, value); }
    /// <summary>Currently addressed by a diagnostics test signal.</summary>
    public bool IsTestTarget { get => _isTestTarget; set => Set(ref _isTestTarget, value); }

    /// <summary>True while values are being synchronised from the engine (no echo commands).</summary>
    internal bool Suppress { get; set; }

    public void SetFromEngine(double gainDb, bool muted)
    {
        Suppress = true;
        GainDb = gainDb;
        IsMuted = muted;
        Suppress = false;
    }
}

/// <summary>One channel meter (input endpoint channels, or the channels leaving the engine).</summary>
public sealed class InputMeterViewModel : ObservableObject
{
    private double _peakDb = -96, _rmsDb = -96;
    private bool _present;

    public InputMeterViewModel(string name) => Name = name;
    public string Name { get; }
    public double PeakDb { get => _peakDb; set => Set(ref _peakDb, value); }
    public double RmsDb { get => _rmsDb; set => Set(ref _rmsDb, value); }
    public bool IsPresent { get => _present; set => Set(ref _present, value); }
}

public sealed class DeviceItem
{
    public required string Id { get; init; }
    /// <summary>v1.4.0: LegacyAVR's own default output (the "Default" button) - not Windows' default.</summary>
    public bool IsAppDefault { get; init; }
    /// <summary>v1.4.0: placeholder for a saved output that is not connected right now.</summary>
    public bool IsMissing { get; init; }
    public required string Name { get; init; }
    public required bool IsRender { get; init; }
    public bool IsDefault { get; init; }
    public int Channels { get; init; }
    public int SampleRate { get; init; }
    public uint IecCaps { get; init; }

    public string Display
    {
        get
        {
            if (IsMissing) return $"{Name} (not connected){(IsAppDefault ? " (Default)" : "")}";
            // bit 7 = the device answered the exclusive-mode probe (v1.2.0)
            var formats = string.Join(" ", new[] { (IecCaps & 1) != 0 ? "DD" : null, (IecCaps & 2) != 0 ? "DTS" : null,
                                                    (IecCaps & 4) != 0 ? "DD+" : null }.WhereNotNull());
            var caps = !IsRender ? "" : formats.Length > 0 ? $" [{formats}]" : (IecCaps & 0x80) != 0 ? " [PCM only]" : "";
            var flow = IsRender ? "" : " (recording)";
            var def = IsDefault ? " - Windows default" : "";
            var app = IsAppDefault ? " (Default)" : "";
            return $"{Name}{app}{flow}  {Channels}ch {SampleRate / 1000.0:0.#} kHz{caps}{def}";
        }
    }

    public override string ToString() => Display;

    public DeviceItem With(bool appDefault) => new()
    {
        Id = Id, Name = Name, IsRender = IsRender, IsDefault = IsDefault, Channels = Channels, SampleRate = SampleRate,
        IecCaps = IecCaps, IsMissing = IsMissing, IsAppDefault = appDefault,
    };
}

internal static class EnumerableExtensions
{
    public static System.Collections.Generic.IEnumerable<string> WhereNotNull(this string?[] items)
    {
        foreach (var s in items) if (s is not null) yield return s;
    }
}
