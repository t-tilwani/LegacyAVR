using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace LegacyAVR.UI.Controls;

/// <summary>
/// Vertical/horizontal level meter: RMS bar (green / amber / red zones), a thin peak
/// line, and a peak-hold tick (1.5 s hold, then 15 dB/s fall). Values arrive at ~30 Hz
/// from telemetry; the bar itself falls smoothly at 24 dB/s between updates.
/// </summary>
public sealed class VuMeter : FrameworkElement
{
    public static readonly DependencyProperty RmsDbProperty = DependencyProperty.Register(
        nameof(RmsDb), typeof(double), typeof(VuMeter), new FrameworkPropertyMetadata(-96.0, FrameworkPropertyMetadataOptions.AffectsRender, OnLevel));
    public static readonly DependencyProperty PeakDbProperty = DependencyProperty.Register(
        nameof(PeakDb), typeof(double), typeof(VuMeter), new FrameworkPropertyMetadata(-96.0, FrameworkPropertyMetadataOptions.AffectsRender, OnLevel));
    public static readonly DependencyProperty MinDbProperty = DependencyProperty.Register(
        nameof(MinDb), typeof(double), typeof(VuMeter), new FrameworkPropertyMetadata(-60.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty OrientationProperty = DependencyProperty.Register(
        nameof(Orientation), typeof(Orientation), typeof(VuMeter), new FrameworkPropertyMetadata(Orientation.Vertical, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty IsDimmedProperty = DependencyProperty.Register(
        nameof(IsDimmed), typeof(bool), typeof(VuMeter), new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender));

    public double RmsDb { get => (double)GetValue(RmsDbProperty); set => SetValue(RmsDbProperty, value); }
    public double PeakDb { get => (double)GetValue(PeakDbProperty); set => SetValue(PeakDbProperty, value); }
    public double MinDb { get => (double)GetValue(MinDbProperty); set => SetValue(MinDbProperty, value); }
    public Orientation Orientation { get => (Orientation)GetValue(OrientationProperty); set => SetValue(OrientationProperty, value); }
    public bool IsDimmed { get => (bool)GetValue(IsDimmedProperty); set => SetValue(IsDimmedProperty, value); }

    private static readonly Brush Track = Make(0x12, 0x15, 0x1B);
    private static readonly Brush Green = Make(0x22, 0xC5, 0x5E);
    private static readonly Brush Amber = Make(0xF5, 0x9E, 0x0B);
    private static readonly Brush Red = Make(0xEF, 0x44, 0x44);
    private static readonly Brush PeakLine = Make(0xE6, 0xE8, 0xEC);
    private static readonly Pen TickPen = MakePen(0x2A, 0x2F, 0x3A);

    private double _shownRms = -96, _hold = -96;
    private DateTime _holdTime = DateTime.MinValue, _last = DateTime.UtcNow;

    private static void OnLevel(DependencyObject d, DependencyPropertyChangedEventArgs e) => ((VuMeter)d).Advance();

    private void Advance()
    {
        var now = DateTime.UtcNow;
        double dt = Math.Min((now - _last).TotalSeconds, 0.25);
        _last = now;
        _shownRms = Math.Max(RmsDb, _shownRms - 24.0 * dt);   // smooth decay
        if (PeakDb >= _hold) { _hold = PeakDb; _holdTime = now; }
        else if ((now - _holdTime).TotalSeconds > 1.5) _hold = Math.Max(PeakDb, _hold - 15.0 * dt);
    }

    private double Frac(double db) => Math.Clamp((db - MinDb) / (0.0 - MinDb), 0.0, 1.0);

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        bool vertical = Orientation == Orientation.Vertical;
        double len = vertical ? h : w;
        dc.PushOpacity(IsDimmed ? 0.35 : 1.0);
        dc.DrawRoundedRectangle(Track, null, new Rect(0, 0, w, h), 3, 3);

        // Coloured segments: < -18 dB green, -18..-6 amber, > -6 red.
        double f = Frac(_shownRms);
        double fAmber = Frac(-18), fRed = Frac(-6);
        void Seg(double a, double b, Brush br)
        {
            if (b <= a) return;
            dc.DrawRectangle(br, null, vertical
                ? new Rect(1, h - b * len, w - 2, (b - a) * len)
                : new Rect(a * len, 1, (b - a) * len, h - 2));
        }
        Seg(0, Math.Min(f, fAmber), Green);
        Seg(fAmber, Math.Min(f, fRed), Amber);
        Seg(fRed, f, Red);

        // dB scale ticks every 6 dB
        for (double db = 0; db > MinDb; db -= 6)
        {
            double p = Frac(db) * len;
            if (vertical) dc.DrawLine(TickPen, new Point(0, h - p), new Point(3, h - p));
            else dc.DrawLine(TickPen, new Point(p, h), new Point(p, h - 3));
        }

        // instantaneous peak + peak hold
        void Mark(double db, double thickness, Brush br)
        {
            if (db <= MinDb) return;
            double p = Frac(db) * len;
            dc.DrawRectangle(br, null, vertical
                ? new Rect(0, Math.Max(0, h - p - thickness / 2), w, thickness)
                : new Rect(Math.Max(0, p - thickness / 2), 0, thickness, h));
        }
        Mark(PeakDb, 1, PeakLine);
        Mark(_hold, 2, _hold > -1 ? Red : PeakLine);
        dc.Pop();
    }

    private static Brush Make(byte r, byte g, byte b)
    {
        var br = new SolidColorBrush(Color.FromRgb(r, g, b));
        br.Freeze();
        return br;
    }

    private static Pen MakePen(byte r, byte g, byte b)
    {
        var p = new Pen(Make(r, g, b), 1);
        p.Freeze();
        return p;
    }
}
