using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace LegacyAVR.UI.Services;

/// <summary>Result of parsing a typed EQ value: the value to apply, or why it was refused.</summary>
/// <param name="Value">The value after rounding to the control's step (valid only when <see cref="Ok"/>).</param>
/// <param name="Error">Why the text was refused (null when it was accepted).</param>
/// <param name="Note">Set when a valid value was rounded to the control's step, so the change is never silent.</param>
public readonly record struct EqEntryResult(bool Ok, double Value, string? Error, string? Note);

/// <summary>
/// v1.6.0: typed values for the EQ dialog's frequency, gain and Q boxes, and the per-band copy to
/// other channels. The slider steps and ranges are the same as the sliders' (and the engine's), so a
/// typed value and a dragged value always end up identical. Values outside the range are refused,
/// never clamped; values between steps are rounded and the rounding is reported.
/// </summary>
public static class EqEntry
{
    private static readonly CultureInfo Ci = CultureInfo.InvariantCulture;
    private static readonly Regex Number = new(@"^[+-]?(\d+([.,]\d*)?|[.,]\d+)$", RegexOptions.Compiled);

    // ---- steps shared with the sliders
    public static double RoundFreq(double hz) =>
        hz < 100 ? Math.Round(hz) : hz < 1000 ? Math.Round(hz / 5) * 5 : Math.Round(hz / 50) * 50;
    public static double RoundGain(double db) => Math.Round(db * 2) / 2;
    public static double RoundQ(double q) => Math.Round(q * 20) / 20;

    // ---- display (also what the boxes show)
    public static string FormatFreq(double hz) =>
        hz >= 1000 ? (hz / 1000).ToString("0.##", Ci) + " kHz" : hz.ToString("0", Ci) + " Hz";
    public static string FormatGain(double db) => db.ToString("+0.0;-0.0;0.0", Ci) + " dB";
    public static string FormatQ(double q) => "Q " + q.ToString("0.00", Ci);

    private static string FreqRangeText(int channel) =>
        $"{FormatFreq(EqModel.FreqMinHz)} - {FormatFreq(EqModel.FreqMax(channel))}";

    /// <summary>"120", "120 Hz", "1.2k", "1.2 kHz", "1200hz".</summary>
    public static EqEntryResult ParseFreq(string? text, int channel)
    {
        var t = Clean(text, "hz");
        double scale = 1;
        if (t.EndsWith("k", StringComparison.Ordinal)) { scale = 1000; t = t[..^1].Trim(); }
        if (!TryNumber(t, out var v))
            return Fail($"Type a frequency such as 120 Hz or 1.2 kHz ({FreqRangeText(channel)}).");
        v *= scale;
        if (v < EqModel.FreqMinHz || v > EqModel.FreqMax(channel))
            return Fail(channel == EqModel.LfeChannel
                ? $"The LFE channel's bands go from {FreqRangeText(channel)}."
                : $"Frequency must be {FreqRangeText(channel)}.");
        return Rounded(v, RoundFreq(v), FormatFreq);
    }

    /// <summary>"3", "+3", "-2.5 dB".</summary>
    public static EqEntryResult ParseGain(string? text)
    {
        if (!TryNumber(Clean(text, "db"), out var v))
            return Fail("Type a gain in dB, for example -2.5 or +3 (-6 to +6 dB).");
        if (v < -EqModel.GainMaxDb || v > EqModel.GainMaxDb)
            return Fail($"Gain must be between -{EqModel.GainMaxDb:0} and +{EqModel.GainMaxDb:0} dB.");
        return Rounded(v, RoundGain(v), FormatGain);
    }

    /// <summary>"1.3", "Q 1.3", "q=1.3".</summary>
    public static EqEntryResult ParseQ(string? text)
    {
        var t = (text ?? string.Empty).Trim().ToLowerInvariant();
        if (t.StartsWith('q')) t = t[1..].TrimStart(' ', '=', ':');
        if (!TryNumber(t, out var v))
            return Fail("Type a Q value such as 1.3 (0.50 - 4.00).");
        if (v < EqModel.QMin || v > EqModel.QMax)
            return Fail($"Q must be between {EqModel.QMin.ToString("0.00", Ci)} and {EqModel.QMax.ToString("0.00", Ci)}.");
        return Rounded(v, RoundQ(v), FormatQ);
    }

    private static string Clean(string? text, string unit)
    {
        var t = (text ?? string.Empty).Trim().ToLowerInvariant();
        if (t.EndsWith(unit, StringComparison.Ordinal)) t = t[..^unit.Length];
        return t.Trim();
    }

    private static bool TryNumber(string t, out double v)
    {
        v = 0;
        if (!Number.IsMatch(t)) return false;
        return double.TryParse(t.Replace(',', '.'), NumberStyles.Float, Ci, out v) && double.IsFinite(v);
    }

    private static EqEntryResult Fail(string why) => new(false, 0, why, null);

    private static EqEntryResult Rounded(double typed, double rounded, Func<double, string> fmt) =>
        new(true, rounded, null, Math.Abs(typed - rounded) > 1e-9 ? $"Rounded to {fmt(rounded)} (the control's step)." : null);

    // ---- per-band copy to other channels
    /// <summary>Why <paramref name="band"/> cannot be copied to <paramref name="targetChannel"/>, or null when it can.</summary>
    public static string? CopyBlocker(EqBand band, int targetChannel) =>
        band.FreqHz > EqModel.FreqMax(targetChannel)
            ? $"{FormatFreq(band.FreqHz)} is above this channel's {FormatFreq(EqModel.FreqMax(targetChannel))} limit"
            : null;

    /// <summary>The destination band with the source band's applicable settings: frequency and gain,
    /// plus Q for bells (a shelf has no Q, so the destination keeps its own).</summary>
    public static EqBand CopyInto(EqBand source, EqBand destination, int band) =>
        EqModel.IsShelf(band) ? destination with { FreqHz = source.FreqHz, GainDb = source.GainDb }
                              : new EqBand(source.FreqHz, source.GainDb, source.Q);

    /// <summary>"1 kHz, +3.0 dB, Q 1.40" (no Q for shelves).</summary>
    public static string Describe(EqBand b, int band) =>
        $"{FormatFreq(b.FreqHz)}, {FormatGain(b.GainDb)}" + (EqModel.IsShelf(band) ? string.Empty : $", {FormatQ(b.Q)}");

    /// <summary>Names joined for a sentence: "A", "A and B", "A, B and C".</summary>
    public static string JoinNames(IReadOnlyList<string> names) => names.Count switch
    {
        0 => string.Empty,
        1 => names[0],
        _ => string.Join(", ", names.Take(names.Count - 1)) + " and " + names[^1],
    };
}
