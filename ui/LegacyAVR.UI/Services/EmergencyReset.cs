using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;

namespace LegacyAVR.UI.Services;

/// <summary>
/// v1.5.0 emergency reset: force-stops the LegacyAVR engine processes and (optionally) starts
/// the dashboard again. Used when the engine or the dashboard stops responding, so nothing here
/// relies on the engine's pipe.
/// </summary>
public static class EmergencyReset
{
    /// <summary>Command-line switch of the relaunched dashboard: wait for this process id to exit
    /// before taking the single-instance lock.</summary>
    public const string WaitForExitArg = "--wait-for-exit";

    /// <summary>Process names of the engine (per-user exe and the Windows service).</summary>
    public static readonly string[] EngineProcessNames = { "AVREngine", "AVREngineService" };

    /// <summary>Kills every engine process and waits for it to exit. Returns one line per process
    /// that could not be stopped (empty = all stopped).</summary>
    public static IReadOnlyList<string> StopEngines(int waitMs = 4000)
    {
        var problems = new List<string>();
        foreach (var name in EngineProcessNames)
        {
            Process[] procs;
            try { procs = Process.GetProcessesByName(name); }
            catch (Exception ex) { problems.Add($"{name}: {ex.Message}"); continue; }
            foreach (var p in procs)
            {
                using (p)
                {
                    try
                    {
                        if (p.HasExited) continue;
                        p.Kill(entireProcessTree: true);
                        if (!p.WaitForExit(waitMs)) problems.Add($"{name} (process {p.Id}) did not exit in time.");
                    }
                    catch (Win32Exception) when (name == "AVREngineService")
                    {
                        problems.Add("The LegacyAVR Encoder Engine service is still running: stopping a Windows service needs " +
                                     "administrator rights. Restart it in Windows Services (services.msc) if it is not responding.");
                    }
                    catch (InvalidOperationException) { /* exited meanwhile */ }
                    catch (Exception ex) { problems.Add($"{name} (process {p.Id}): {ex.Message}"); }
                }
            }
        }
        return problems;
    }

    /// <summary>Starts the dashboard again; the new instance waits for this one to exit first.</summary>
    /// <returns>null on success, otherwise a user-facing error.</returns>
    public static string? RelaunchDashboard()
    {
        var exe = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exe)) return "Could not find LegacyAVR.UI.exe to start it again. Start LegacyAVR from the Start menu.";
        try
        {
            var psi = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = AppContext.BaseDirectory };
            psi.ArgumentList.Add(WaitForExitArg);
            psi.ArgumentList.Add(Environment.ProcessId.ToString(CultureInfo.InvariantCulture));
            Process.Start(psi)?.Dispose();
            return null;
        }
        catch (Exception ex)
        {
            return $"Could not start LegacyAVR again ({ex.Message}). Start it from the Start menu.";
        }
    }

    /// <summary>The process id after <see cref="WaitForExitArg"/>, if present.</summary>
    public static int? ParseWaitForExit(IReadOnlyList<string> args)
    {
        for (int i = 0; i + 1 < args.Count; i++)
            if (string.Equals(args[i], WaitForExitArg, StringComparison.OrdinalIgnoreCase) &&
                int.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out var pid) && pid > 0)
                return pid;
        return null;
    }

    /// <summary>Relaunched dashboard: waits (up to <paramref name="timeoutMs"/>) for the old one to exit.</summary>
    public static void WaitForPreviousInstance(IReadOnlyList<string> args, int timeoutMs = 15000)
    {
        if (ParseWaitForExit(args) is not int pid || pid == Environment.ProcessId) return;
        try
        {
            using var old = Process.GetProcessById(pid);
            old.WaitForExit(timeoutMs);
        }
        catch (Exception)
        {
            // Already gone (ArgumentException) or not accessible: nothing to wait for.
        }
    }
}
