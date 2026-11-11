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
        CopyToChannelsCommand = new RelayCommand(() => _owner.RequestCopy(this));
    }

    /// <summary>v1.6.0: opens the "apply this band to other channels" dialog.</summary>
    public RelayCommand CopyToChannelsCommand { get; }

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
            var v = EqEntry.RoundFreq(Math.Clamp(value, EqModel.FreqMinHz, EqModel.FreqMax(Channel)));
            if (!Set(ref _freq, v)) return;
            OnPropertyChanged(nameof(FreqLog));
            OnPropertyChanged(nameof(FreqText));
            OnPropertyChanged(nameof(FreqEntry));
            ClearEdit(EditField.Freq);
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
            if (!Set(ref _gain, EqEntry.RoundGain(Math.Clamp(value, -EqModel.GainMaxDb, EqModel.GainMaxDb)))) return;
            OnPropertyChanged(nameof(GainText));
            OnPropertyChanged(nameof(GainEntry));
            ClearEdit(EditField.Gain);
            Changed();
        }
    }

    public double Q
    {
        get => _q;
        set
        {
            if (!Set(ref _q, EqEntry.RoundQ(Math.Clamp(value, EqModel.QMin, EqModel.QMax)))) return;
            OnPropertyChanged(nameof(QText));
            OnPropertyChanged(nameof(QEntry));
            ClearEdit(EditField.Q);
            Changed();
        }
    }

    public string FreqText => EqEntry.FormatFreq(_freq);
    public string GainText => EqEntry.FormatGain(_gain);
    public string QText => EqEntry.FormatQ(_q);

    // ---- v1.6.0: the readouts are editable. A valid entry (Enter or leaving the box) moves the
    // slider and the setting; an invalid one is refused - the box goes back to the last valid
    // value and the row says why. Values outside the range are never clamped silently.
    private enum EditField { None, Freq, Gain, Q }
    private EditField _errorField = EditField.None;
    private string _editMessage = string.Empty;
    private bool _editIsError;

    public string FreqEntry
    {
        get => FreqText;
        set => Commit(EditField.Freq, EqEntry.ParseFreq(value, Channel), v => FreqHz = v, nameof(FreqEntry));
    }
    public string GainEntry
    {
        get => GainText;
        set => Commit(EditField.Gain, EqEntry.ParseGain(value), v => GainDb = v, nameof(GainEntry));
    }
    public string QEntry
    {
        get => QText;
        set => Commit(EditField.Q, EqEntry.ParseQ(value), v => Q = v, nameof(QEntry));
    }

    public bool FreqInvalid => _editIsError && _errorField == EditField.Freq;
    public bool GainInvalid => _editIsError && _errorField == EditField.Gain;
    public bool QInvalid => _editIsError && _errorField == EditField.Q;
    /// <summary>Why the last typed value was refused, or how it was rounded ("" = nothing to say).</summary>
    public string EditMessage => _editMessage;
    public bool HasEditMessage => _editMessage.Length > 0;
    public bool EditIsError => _editIsError;

    private void Commit(EditField field, EqEntryResult r, Action<double> apply, string entryProperty)
    {
        if (r.Ok)
        {
            apply(r.Value);
            SetEdit(EditField.None, r.Note ?? string.Empty, false);
        }
        else
        {
            SetEdit(field, r.Error ?? "Invalid value.", true);
        }
        OnPropertyChanged(entryProperty);   // the box shows the formatted (last valid) value again
    }

    private void SetEdit(EditField field, string message, bool error)
    {
        _errorField = field;
        _editMessage = message;
        _editIsError = error;
        OnPropertyChanged(nameof(FreqInvalid));
        OnPropertyChanged(nameof(GainInvalid));
        OnPropertyChanged(nameof(QInvalid));
        OnPropertyChanged(nameof(EditMessage));
        OnPropertyChanged(nameof(HasEditMessage));
        OnPropertyChanged(nameof(EditIsError));
    }

    /// <summary>Moving a slider or loading values replaces an old error about that field.</summary>
    private void ClearEdit(EditField field)
    {
        if (_editMessage.Length == 0) return;
        if (_editIsError && _errorField != field) return;   // an error about another field stays
        SetEdit(EditField.None, string.Empty, false);
    }

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

    /// <summary>v1.6.0: raised when a band's "apply to other channels" button is pressed.</summary>
    public event EventHandler<EqBandViewModel>? CopyRequested;
    internal void RequestCopy(EqBandViewModel band) => CopyRequested?.Invoke(this, band);
}

/// <summary>v1.6.0: one destination channel in the "apply band to other channels" dialog.</summary>
public sealed class EqCopyTarget : ObservableObject
{
    private bool _isChecked;
    public EqCopyTarget(ChannelEqViewModel channel, string? blocker)
    {
        Channel = channel;
        Blocker = blocker;
        _isChecked = blocker is null;   // every applicable channel is checked by default
    }
    public ChannelEqViewModel Channel { get; }
    public string Name => Channel.Name;
    /// <summary>Why this channel cannot take the band (null = it can).</summary>
    public string? Blocker { get; }
    public bool IsApplicable => Blocker is null;
    public string Note => Blocker is null ? string.Empty : $"Not available: {Blocker}.";
    public event EventHandler? CheckedChanged;
    public bool IsChecked
    {
        get => _isChecked;
        set
        {
            if (!IsApplicable) value = false;
            if (Set(ref _isChecked, value)) CheckedChanged?.Invoke(this, EventArgs.Empty);
        }
    }
}

/// <summary>
/// v1.6.0: copies one band (frequency and gain, plus Q for bells) of one channel to the checked
/// channels, after a confirmation. Every other band, and the source, stay as they are. The copy
/// goes through each band's normal setters, so the display, the engine and the saved EQ follow.
/// </summary>
public sealed class EqBandCopyViewModel : ObservableObject
{
    private readonly Func<string, bool> _confirm;

    public EqBandCopyViewModel(ChannelEqViewModel source, EqBandViewModel band, System.Collections.Generic.IEnumerable<ChannelEqViewModel> channels,
                               Func<string, bool> confirm)
    {
        Source = source;
        Band = band;
        _confirm = confirm;
        SourceModel = band.ToModel();
        Targets = channels.Where(c => c.Index != source.Index)
                          .Select(c => new EqCopyTarget(c, EqEntry.CopyBlocker(SourceModel, c.Index))).ToArray();
        foreach (var t in Targets) t.CheckedChanged += (_, _) => { OnPropertyChanged(nameof(CanApply)); OnPropertyChanged(nameof(SelectionText)); };
        ApplyCommand = new RelayCommand(() => { if (Apply()) Applied?.Invoke(this, EventArgs.Empty); }, () => CanApply);
    }

    public ChannelEqViewModel Source { get; }
    public EqBandViewModel Band { get; }
    public EqBand SourceModel { get; }
    public EqCopyTarget[] Targets { get; }
    public RelayCommand ApplyCommand { get; }
    public event EventHandler? Applied;

    public string Title => $"Apply {Band.Name} to other channels";
    public string SourceText => $"Copy {Band.Name} of {Source.Name}: {EqEntry.Describe(SourceModel, Band.Band)}";
    public string ScopeText => EqModel.IsShelf(Band.Band)
        ? "Copies the frequency and gain of this shelf. The other bands of each channel are not changed."
        : "Copies the frequency, gain and Q of this bell. The other bands of each channel are not changed.";
    public bool CanApply => Targets.Any(t => t.IsChecked);
    public string SelectionText => CanApply ? string.Empty : "Select at least one channel to apply the band to.";

    /// <summary>Asks before overwriting, then copies. Returns false when nothing was changed.</summary>
    public bool Apply()
    {
        var chosen = Targets.Where(t => t.IsChecked && t.IsApplicable).Select(t => t.Channel).ToList();
        if (chosen.Count == 0) return false;
        var names = EqEntry.JoinNames(chosen.Select(c => c.Name).ToList());
        if (!_confirm($"Overwrite {Band.Name} on {names} with {EqEntry.Describe(SourceModel, Band.Band)}?\n\n" +
                      "The current settings of that band on those channels are replaced. Their other bands are not changed."))
            return false;
        foreach (var ch in chosen)
        {
            var dest = ch.Bands[Band.Band];
            var merged = EqModel.Clamp(ch.Index, Band.Band, EqEntry.CopyInto(SourceModel, dest.ToModel(), Band.Band));
            dest.FreqHz = merged.FreqHz;
            dest.GainDb = merged.GainDb;
            dest.Q = merged.Q;
        }
        return true;
    }
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
