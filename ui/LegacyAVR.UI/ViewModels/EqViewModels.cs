using System;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Media;
using LegacyAVR.UI.Services;

namespace LegacyAVR.UI.ViewModels;

/// <summary>v1.4.0: one band of a channel's custom EQ (frequency on a log scale, gain, Q).</summary>
public sealed class EqBandViewModel : ObservableObject
{
    private readonly ChannelEqViewModel _owner;
    private double _freq, _gain, _q;

    public EqBandViewModel(ChannelEqViewModel owner, int band)
    {
        _owner = owner;
        Band = band;
        var d = EqModel.DefaultBand(owner.Index, band);
        _freq = d.FreqHz;
        _gain = d.GainDb;
        _q = d.Q;
    }

    public int Band { get; }
    public int Channel => _owner.Index;
    public string Name => EqModel.BandNames[Band];
    public bool HasQ => !EqModel.IsShelf(Band);
    public double FreqLogMin => Math.Log10(EqModel.FreqMinHz);
    public double FreqLogMax => Math.Log10(EqModel.FreqMax(Channel));
    public double GainMax => EqModel.GainMaxDb;
    public double QMin => EqModel.QMin;
    public double QMax => EqModel.QMax;

    // Reset-button keys (MainViewModel.ResetCommand)
    public string FreqResetKey => $"eq{Channel}b{Band}f";
    public string GainResetKey => $"eq{Channel}b{Band}g";
    public string QResetKey => $"eq{Channel}b{Band}q";

    public double FreqHz
    {
        get => _freq;
        set
        {
            var v = Math.Clamp(value, EqModel.FreqMinHz, EqModel.FreqMax(Channel));
            v = v < 100 ? Math.Round(v) : v < 1000 ? Math.Round(v / 5) * 5 : Math.Round(v / 50) * 50;
            if (!Set(ref _freq, v)) return;
            OnPropertyChanged(nameof(FreqLog));
            OnPropertyChanged(nameof(FreqText));
            Changed();
        }
    }

    /// <summary>Slider position: log10 of the frequency, so every octave gets the same travel.</summary>
    public double FreqLog
    {
        get => Math.Log10(_freq);
        set => FreqHz = Math.Pow(10, value);
    }

    public double GainDb
    {
        get => _gain;
        set
        {
            if (!Set(ref _gain, Math.Round(Math.Clamp(value, -EqModel.GainMaxDb, EqModel.GainMaxDb) * 2) / 2)) return;
            OnPropertyChanged(nameof(GainText));
            Changed();
        }
    }

    public double Q
    {
        get => _q;
        set
        {
            if (!Set(ref _q, Math.Round(Math.Clamp(value, EqModel.QMin, EqModel.QMax) * 20) / 20)) return;
            OnPropertyChanged(nameof(QText));
            Changed();
        }
    }

    public string FreqText => _freq >= 1000 ? $"{_freq / 1000:0.##} kHz" : $"{_freq:0} Hz";
    public string GainText => _gain.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture) + " dB";
    public string QText => $"Q {_q:0.00}";

    public EqBand ToModel() => new(_freq, _gain, _q);
    public bool IsDefault => ToModel() == EqModel.DefaultBand(Channel, Band);

    /// <summary>True while values come from the engine / a profile (no echo commands).</summary>
    internal bool Suppress { get; set; }

    public void SetFromModel(EqBand b)
    {
        b = EqModel.Clamp(Channel, Band, b);
        Suppress = true;
        FreqHz = b.FreqHz;
        GainDb = b.GainDb;
        Q = b.Q;
        Suppress = false;
        _owner.RefreshCurve();
    }

    private void Changed()
    {
        _owner.RefreshCurve();
        if (!Suppress) _owner.BandChanged(this);
    }
}

/// <summary>v1.4.0: the custom EQ of one output channel.</summary>
public sealed class ChannelEqViewModel : ObservableObject
{
    private readonly Action<ChannelEqViewModel, EqBandViewModel> _bandChanged;
    private readonly Action<ChannelEqViewModel> _enabledChanged;
    private bool _enabled = true;
    private string _name;

    public ChannelEqViewModel(int index, string name, Action<ChannelEqViewModel, EqBandViewModel> bandChanged,
                              Action<ChannelEqViewModel> enabledChanged)
    {
        Index = index;
        _name = name;
        _bandChanged = bandChanged;
        _enabledChanged = enabledChanged;
        Bands = Enumerable.Range(0, EqModel.Bands).Select(b => new EqBandViewModel(this, b)).ToArray();
        RefreshCurve();
    }

    public int Index { get; }
    public string Name { get => _name; set => Set(ref _name, value); }
    public EqBandViewModel[] Bands { get; }
    /// <summary>v1.5.0: the channel name (what any list or selection box falls back to).</summary>
    public override string ToString() => _name;
    public string ChannelResetKey => $"eqch{Index}";

    /// <summary>Per-channel enable / bypass (settings are kept while bypassed).</summary>
    public bool Enabled
    {
        get => _enabled;
        set
        {
            if (!Set(ref _enabled, value)) return;
            OnPropertyChanged(nameof(Summary));
            if (!Suppress) _enabledChanged(this);
        }
    }

    internal bool Suppress { get; set; }

    public void SetEnabledFromEngine(bool on)
    {
        Suppress = true;
        Enabled = on;
        Suppress = false;
    }

    public EqChannel ToModel() => new(_enabled, Bands.Select(b => b.ToModel()).ToArray());
    public bool IsFlat => Bands.All(b => Math.Abs(b.GainDb) < 1e-9);

    /// <summary>"flat", "bypassed", "+3.0 dB at 80 Hz, ..." - shown on the mixer strip button tooltip.</summary>
    public string Summary
    {
        get
        {
            if (!_enabled) return "EQ bypassed for this channel";
            var changed = Bands.Where(b => Math.Abs(b.GainDb) > 1e-9).Select(b => $"{b.GainText} at {b.FreqText}").ToList();
            return changed.Count == 0 ? "EQ flat" : "EQ: " + string.Join(", ", changed);
        }
    }

    // ---- response curve for the dialog (dB, -12..+12 mapped onto a 600 x 160 canvas)
    public const double PlotWidth = 600, PlotHeight = 160, PlotRangeDb = 12;
    private PointCollection _curve = new();
    public PointCollection Curve { get => _curve; private set => Set(ref _curve, value); }

    internal void RefreshCurve()
    {
        var model = new EqChannel(true, Bands.Select(b => b.ToModel()).ToArray());
        var pts = new PointCollection();
        double lo = Math.Log10(EqModel.FreqMinHz), hi = Math.Log10(EqModel.FreqMaxHz);
        for (int i = 0; i <= 150; i++)
        {
            double lf = lo + (hi - lo) * i / 150.0;
            double db = Math.Clamp(EqModel.ResponseDb(model, Math.Pow(10, lf)), -PlotRangeDb, PlotRangeDb);
            pts.Add(new Point(PlotWidth * i / 150.0, PlotHeight / 2 - db / PlotRangeDb * (PlotHeight / 2)));
        }
        pts.Freeze();
        Curve = pts;
        OnPropertyChanged(nameof(Summary));
        OnPropertyChanged(nameof(IsFlat));
    }

    internal void BandChanged(EqBandViewModel band) => _bandChanged(this, band);
}

/// <summary>
/// v1.4.0: the EQ dialog. Auditioning solos the channel being tuned; it is temporary - the
/// dialog refreshes it every 2 s, the engine drops it 6 s after the last refresh, and
/// closing the dialog (or switching it off) ends it. Saved mixer mutes are never changed.
/// </summary>
public sealed class EqDialogViewModel : ObservableObject, IDisposable
{
    private readonly System.Windows.Threading.DispatcherTimer _heartbeat;
    private ChannelEqViewModel _channel;
    private bool _audition;

    public EqDialogViewModel(MainViewModel main, int channel)
    {
        Main = main;
        _channel = main.EqChannels[Math.Clamp(channel, 0, main.EqChannels.Count - 1)];
        ResetChannelCommand = new RelayCommand(() => Main.ConfirmAndResetEqChannel(_channel.Index), () => !_channel.IsFlat);
        _heartbeat = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _heartbeat.Tick += (_, _) => { if (_audition) Main.SetAudition(_channel.Index); };
    }

    public MainViewModel Main { get; }
    public RelayCommand ResetChannelCommand { get; }

    public ChannelEqViewModel Channel
    {
        get => _channel;
        set
        {
            if (value is null || !Set(ref _channel, value)) return;
            OnPropertyChanged(nameof(Title));
            OnPropertyChanged(nameof(FreqRangeText));
            if (_audition) Main.SetAudition(_channel.Index);   // audition follows the channel
        }
    }

    public string Title => $"Custom EQ - {_channel.Name}";
    public string FreqRangeText => _channel.Index == EqModel.LfeChannel
        ? "The LFE channel only carries bass, so its bands are limited to 20 - 200 Hz."
        : "Bands: low shelf, three bells, high shelf. Gain is limited to +/-6 dB and Q to 0.5 - 4 so changes stay safe.";

    /// <summary>Solo this channel while tuning (other channels muted temporarily).</summary>
    public bool Audition
    {
        get => _audition;
        set
        {
            if (!Set(ref _audition, value)) return;
            Main.SetAudition(value ? _channel.Index : -1);
            if (value) _heartbeat.Start(); else _heartbeat.Stop();
        }
    }

    public void Dispose()
    {
        _heartbeat.Stop();
        if (_audition) Main.SetAudition(-1);
        _audition = false;
    }
}
