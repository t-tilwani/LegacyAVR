using System;
using System.Collections.Generic;

namespace LegacyAVR.UI.Services;

/// <summary>
/// v1.4.0: the individual "reset to default" buttons next to sliders. Each key resets exactly
/// one setting to its factory default, after a confirmation. There is deliberately no
/// "reset everything".
/// </summary>
public sealed class SettingResets
{
    private sealed record Entry(string Label, Func<string> DefaultText, Func<bool> IsDefault, Action Reset);
    private readonly Dictionary<string, Entry> _entries = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Registers one resettable setting.</summary>
    /// <param name="key">Command parameter used by the button, e.g. "centre" or "gain3".</param>
    /// <param name="label">"Centre steering" - used in the confirmation question.</param>
    /// <param name="defaultText">"85%" - the factory default as the user sees it.</param>
    /// <param name="isDefault">True when the setting already has its default value.</param>
    /// <param name="reset">Sets the default (through the normal property, so it reaches the engine).</param>
    public void Add(string key, string label, Func<string> defaultText, Func<bool> isDefault, Action reset) =>
        _entries[key] = new Entry(label, defaultText, isDefault, reset);

    public bool Contains(string key) => _entries.ContainsKey(key);

    public string Question(string key) =>
        _entries.TryGetValue(key, out var e) ? $"Reset {e.Label} to its default ({e.DefaultText()})?" : string.Empty;

    /// <summary>Asks, then resets only that setting. Returns true when it was reset.</summary>
    public bool Reset(string key, Func<string, bool> confirm)
    {
        if (!_entries.TryGetValue(key, out var e)) return false;
        if (e.IsDefault()) return false;                 // nothing to do, don't ask
        if (!confirm(Question(key))) return false;
        e.Reset();
        return true;
    }
}
