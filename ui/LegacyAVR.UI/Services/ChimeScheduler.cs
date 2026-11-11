using System;

namespace LegacyAVR.UI.Services;

/// <summary>
/// v1.6.0: decides when the dashboard asks the engine for the activation chime (PLAY_CHIME):
/// once when the audio pipeline is first running after the app starts, and once after the user
/// changes the input and the engine has re-opened the pipeline with it. A failed selection, an
/// error or a timeout cancels the input cue; the startup cue also covers an input change that
/// happens while starting, so the two never play back to back. Pure logic - unit-tested.
/// </summary>
public sealed class ChimeScheduler
{
    /// <summary>The pipeline must have been running this long before the startup cue.</summary>
    public static readonly TimeSpan StartupSettle = TimeSpan.FromMilliseconds(600);
    /// <summary>...and this long after the input change re-opened it.</summary>
    public static readonly TimeSpan InputSettle = TimeSpan.FromMilliseconds(400);
    /// <summary>If no restart is seen (it can be quicker than one dashboard tick), wait this long.</summary>
    public static readonly TimeSpan NoRestartWait = TimeSpan.FromMilliseconds(1500);
    /// <summary>An input change that has not come up running by then is treated as failed.</summary>
    public static readonly TimeSpan InputTimeout = TimeSpan.FromSeconds(15);
    /// <summary>The startup cue belongs to startup: if audio is not running by then (stopped, error,
    /// no device), it is skipped rather than played much later when the user starts audio.</summary>
    public static readonly TimeSpan StartupWindow = TimeSpan.FromSeconds(30);

    private bool _startupHandled;
    private bool _inputPending;
    private bool _sawRestart;
    private DateTime _inputSince;
    private DateTime? _healthySince;
    private DateTime? _firstTick;

    /// <summary>The user's setting (default on). Cues that come due while it is off are dropped, not saved up.</summary>
    public bool Enabled { get; set; } = true;
    public bool StartupHandled => _startupHandled;
    public bool InputPending => _inputPending;

    /// <summary>The engine accepted SET_INPUT_DEVICE; the cue waits for the pipeline to come back up.</summary>
    public void InputChangeAccepted(DateTime now)
    {
        _inputPending = true;
        _sawRestart = false;
        _inputSince = now;
    }

    /// <summary>The engine refused the selection: no cue.</summary>
    public void InputChangeFailed() => _inputPending = false;

    /// <summary>
    /// Called on every telemetry tick. <paramref name="healthy"/>: pipeline running or idle,
    /// audio not stopped. <paramref name="ready"/>: the dashboard has finished its startup work
    /// and is not applying a profile. <paramref name="error"/>: the engine reports an error state.
    /// Returns true exactly when one cue should be requested now.
    /// </summary>
    public bool Tick(bool healthy, bool ready, bool error, DateTime now)
    {
        _firstTick ??= now;
        if (healthy && !error) _healthySince ??= now;
        else _healthySince = null;
        if (!_startupHandled && now - _firstTick.Value > StartupWindow) _startupHandled = true;

        if (_inputPending)
        {
            if (!healthy) _sawRestart = true;
            if (error || now - _inputSince > InputTimeout)
            {
                _inputPending = false;   // the new input did not come up: no "success" cue
                return false;
            }
        }
        if (!ready || _healthySince is not DateTime since) return false;
        var stable = now - since;

        if (!_startupHandled)
        {
            if (stable < StartupSettle) return false;
            _startupHandled = true;
            _inputPending = false;       // one cue covers an input change made while starting
            return Enabled;
        }
        if (_inputPending && stable >= InputSettle && (_sawRestart || now - _inputSince >= NoRestartWait))
        {
            _inputPending = false;
            return Enabled;
        }
        return false;
    }
}
