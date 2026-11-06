using System;
using System.Linq;

namespace LegacyAVR.UI.Services;

/// <summary>v1.4.0: one parametric band of the custom EQ.</summary>
public sealed record EqBand(double FreqHz, double GainDb, double Q);

/// <summary>v1.4.0: one output channel's EQ (five bands: low shelf, three bells, high shelf).</summary>
public sealed record EqChannel(bool Enabled, EqBand[] Bands);

/// <summary>v1.4.0: the whole custom EQ - master switch plus the six output channels.</summary>
public sealed record EqSettings(bool Enabled, EqChannel[] Channels);

/// <summary>
/// Limits, factory defaults and the frequency response of the custom EQ. Mirrors
/// engine/core/ChannelEq.h - the engine clamps again, so the two can never disagree about
/// what is actually applied.
/// </summary>
public static class EqModel
{
    public const int Bands = 5;
    public const int Channels = 6;
    public const int LfeChannel = 3;
    public const double GainMaxDb = 6.0;
    public const double QMin = 0.5;
    public const double QMax = 4.0;
    public const double QDefault = 1.0;
    public const double FreqMinHz = 20.0;
    public const double FreqMaxHz = 20000.0;
    public const double LfeFreqMaxHz = 200.0;
    private const double Fs = 48000.0;

    private static readonly double[] DefaultHz = { 100, 300, 1000, 3000, 8000 };
    private static readonly double[] DefaultLfeHz = { 30, 45, 70, 110, 160 };
    public static readonly string[] BandNames = { "Low shelf", "Bell 1", "Bell 2", "Bell 3", "High shelf" };

    public static bool IsShelf(int band) => band == 0 || band == Bands - 1;
    public static double FreqMax(int channel) => channel == LfeChannel ? LfeFreqMaxHz : FreqMaxHz;

    public static EqBand DefaultBand(int channel, int band) =>
        new(channel == LfeChannel ? DefaultLfeHz[band] : DefaultHz[band], 0.0, QDefault);

    public static EqChannel DefaultChannel(int channel) =>
        new(true, Enumerable.Range(0, Bands).Select(b => DefaultBand(channel, b)).ToArray());

    /// <summary>Master off, every channel enabled, every band flat.</summary>
    public static EqSettings Default() =>
        new(false, Enumerable.Range(0, Channels).Select(DefaultChannel).ToArray());

    public static EqBand Clamp(int channel, int band, EqBand b)
    {
        var def = DefaultBand(channel, band);
        double f = double.IsFinite(b.FreqHz) ? b.FreqHz : def.FreqHz;
        double g = double.IsFinite(b.GainDb) ? b.GainDb : 0;
        double q = double.IsFinite(b.Q) ? b.Q : QDefault;
        return new(Math.Clamp(f, FreqMinHz, FreqMax(channel)), Math.Clamp(g, -GainMaxDb, GainMaxDb), Math.Clamp(q, QMin, QMax));
    }

    /// <summary>Repairs settings read from a file: right shape, every value in range.</summary>
    public static EqSettings Normalise(EqSettings? s)
    {
        if (s is null) return Default();
        var chans = new EqChannel[Channels];
        for (int c = 0; c < Channels; c++)
        {
            var src = s.Channels is { } arr && c < arr.Length ? arr[c] : null;
            var bands = new EqBand[Bands];
            for (int b = 0; b < Bands; b++)
                bands[b] = src?.Bands is { } sb && b < sb.Length && sb[b] is { } band ? Clamp(c, b, band) : DefaultBand(c, b);
            chans[c] = new EqChannel(src?.Enabled ?? true, bands);
        }
        return new EqSettings(s.Enabled, chans);
    }

    public static bool IsFlat(EqChannel ch) => ch.Bands.All(b => Math.Abs(b.GainDb) < 1e-6);

    /// <summary>Combined response of one channel in dB at <paramref name="hz"/> (RBJ cookbook, as the engine).</summary>
    public static double ResponseDb(EqChannel ch, double hz)
    {
        double m = 1.0;
        for (int b = 0; b < Bands && b < ch.Bands.Length; b++) m *= Magnitude(b, ch.Bands[b], hz);
        return 20 * Math.Log10(Math.Max(m, 1e-9));
    }

    private static double Magnitude(int band, EqBand p, double hz)
    {
        double A = Math.Pow(10, p.GainDb / 40);
        double w0 = 2 * Math.PI * p.FreqHz / Fs, cw = Math.Cos(w0), sw = Math.Sin(w0);
        double b0, b1, b2, a0, a1, a2;
        if (band == 0 || band == Bands - 1)
        {
            double alpha = sw / 2 * Math.Sqrt(2), sa = 2 * Math.Sqrt(A) * alpha;
            if (band == 0)
            {
                b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
                a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa;
            }
            else
            {
                b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
                a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa;
            }
        }
        else
        {
            double alpha = sw / (2 * p.Q);
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        }
        double w = 2 * Math.PI * hz / Fs;
        double c1 = Math.Cos(w), s1 = Math.Sin(w), c2 = Math.Cos(2 * w), s2 = Math.Sin(2 * w);
        double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return Math.Sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }

    /// <summary>Engine command for one band.</summary>
    public static string BandCommand(int channel, int band, EqBand b) =>
        $"SET_EQ_BAND {channel} {band} {Proto.F(b.FreqHz)} {Proto.F(b.GainDb)} {Proto.F(b.Q)}";

    /// <summary>"freq,gain,q" as GET_STATE reports a band; null when it does not parse.</summary>
    public static EqBand? ParseBand(string? s)
    {
        if (string.IsNullOrEmpty(s)) return null;
        var p = s.Split(',');
        if (p.Length != 3) return null;
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        var st = System.Globalization.NumberStyles.Float;
        return double.TryParse(p[0], st, ci, out var f) && double.TryParse(p[1], st, ci, out var g) && double.TryParse(p[2], st, ci, out var q)
            ? new EqBand(f, g, q) : null;
    }
}
