namespace LegacyAVR.UI.Services;

/// <summary>
/// v1.4.0: factory defaults of every adjustable value, in one place. They match the
/// engine's own defaults (engine/core/LiveParams.h, engine/win/ConfigStore.h); the slider
/// reset buttons and new profiles use them.
/// </summary>
public static class FactoryDefaults
{
    public const double MasterDb = 0.0;
    public const double GainDb = 0.0;
    public const double BassDb = 0.0;
    public const double CentreLevel = 0.85;
    public const double CentreFloor = 0.30;
    public const double DialogBoost = 0.6;
    public const double SurroundLevel = 0.8;
    public const double SurroundDelayMs = 12.0;
    public const double LfeCrossoverHz = 80.0;
    public const double LfeLevelDb = -10.0;

    // Native-LFE "resume after silence": whole seconds from 1 to 300, 10 recommended.
    public const double LfeResetSeconds = 10.0;
    public const double LfeResetMinSeconds = 1.0;
    public const double LfeResetMaxSeconds = 300.0;

    /// <summary>Whole seconds in 1..300 (non-finite values give the default).</summary>
    public static double ClampLfeReset(double s) =>
        double.IsFinite(s) ? System.Math.Clamp(System.Math.Round(s), LfeResetMinSeconds, LfeResetMaxSeconds) : LfeResetSeconds;
}
