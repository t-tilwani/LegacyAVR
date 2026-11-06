using System;
using System.Collections.Generic;
using System.Linq;

namespace LegacyAVR.UI.Services;

/// <summary>An output device as the dashboard lists it (id + friendly name).</summary>
public sealed record OutputInfo(string Id, string Name);

/// <summary>
/// v1.4.0: decisions the dashboard takes when it connects, kept free of UI code so they are
/// unit-tested. Nothing here ever changes Windows' own default device.
/// </summary>
public static class StartupPolicy
{
    /// <summary>What to do about the output device after connecting.</summary>
    /// <param name="SwitchToId">Send SET_OUTPUT_DEVICE with this id (null = keep the engine's).</param>
    /// <param name="MissingId">The id the selection refers to but which is not connected (shown, not replaced).</param>
    /// <param name="Message">Shown to the user ("" = nothing to report).</param>
    public sealed record OutputDecision(string? SwitchToId, string? MissingId, string? MissingName, string Message);

    /// <summary>
    /// LegacyAVR's own default output (the "Default" button) wins whenever it is connected.
    /// If it is not connected, the engine's current output is kept and the problem reported -
    /// another device is never picked silently. If nothing is selected at all, that is said
    /// explicitly instead of showing an empty box.
    /// </summary>
    public static OutputDecision ResolveOutput(string? engineOutputId, IReadOnlyList<OutputInfo> available,
                                               string? defaultId, string? defaultName,
                                               string? lastId = null, string? lastName = null)
    {
        bool Has(string? id) => !string.IsNullOrEmpty(id) && available.Any(d => Same(d.Id, id));
        string NameOf(string? id, string fallback) =>
            Same(id, defaultId) && !string.IsNullOrEmpty(defaultName) ? defaultName! :
            Same(id, lastId) && !string.IsNullOrEmpty(lastName) ? lastName! : fallback;

        if (!string.IsNullOrEmpty(defaultId))
        {
            if (Has(defaultId))
                return new(Same(defaultId, engineOutputId) ? null : defaultId, null, null, string.Empty);
            var name = NameOf(defaultId, "The default output");
            if (Has(engineOutputId))
            {
                var using_ = available.First(d => Same(d.Id, engineOutputId)).Name;
                return new(null, null, null, $"Default output \"{name}\" is not connected. Using {using_} for now.");
            }
            // Neither is connected: show the missing device rather than an empty box.
            var missingId = string.IsNullOrEmpty(engineOutputId) ? defaultId : engineOutputId;
            return new(null, missingId, NameOf(missingId, name),
                       $"Default output \"{name}\" is not connected. Reconnect it or pick another output.");
        }
        if (Has(engineOutputId)) return new(null, null, null, string.Empty);
        if (string.IsNullOrEmpty(engineOutputId))
            return new(null, null, null, "No output device is selected - pick your HDMI / S/PDIF device.");
        var missing = NameOf(engineOutputId, "The saved output device");
        return new(null, engineOutputId, missing, $"Output \"{missing}\" is not connected. Reconnect it or pick another output.");
    }

    /// <summary>What to do with the last applied profile at startup.</summary>
    public enum ProfileAction { None, Apply, Report }

    /// <param name="SwitchToId">Output to switch to when applying (null = apply to the current output).</param>
    public sealed record ProfileDecision(ProfileAction Action, Profile? Profile, string? SwitchToId, string Message);

    public static ProfileDecision ResolveLastProfile(bool enabled, string? lastProfile, ProfileStore store,
                                                     string? currentOutputId, IEnumerable<string> availableIds)
    {
        if (!enabled || string.IsNullOrWhiteSpace(lastProfile)) return new(ProfileAction.None, null, null, string.Empty);
        var p = store.Find(lastProfile);
        if (p is null)
            return new(ProfileAction.Report, null, null, $"Last profile \"{lastProfile}\" no longer exists, so nothing was applied at startup.");
        return ProfileStore.TargetOf(p, currentOutputId, availableIds) switch
        {
            ProfileTarget.AnyOutput or ProfileTarget.SameOutput => new(ProfileAction.Apply, p, null, string.Empty),
            ProfileTarget.OtherOutput => new(ProfileAction.Apply, p, p.DeviceId, string.Empty),
            _ => new(ProfileAction.Report, p, null,
                     $"Profile \"{p.Name}\" was not applied at startup: its output \"{p.DeviceName ?? p.DeviceId}\" is not connected. " +
                     "Reconnect it, or select the profile and choose \"apply to the current output\"."),
        };
    }

    /// <summary>The "Default" button is offered only for a connected output that is not already the default.</summary>
    public static bool ShowDefaultButton(string? selectedId, bool selectedIsConnected, string? defaultId) =>
        selectedIsConnected && !string.IsNullOrEmpty(selectedId) && !Same(selectedId, defaultId);

    private static bool Same(string? a, string? b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
}
