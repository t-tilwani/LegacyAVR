using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using LegacyAVR.UI.Services;

namespace LegacyAVR.UI.ViewModels;

/// <summary>
/// Coalesces rapid-fire commands (slider drags) per key: at most one request per key
/// is in flight, and only the newest pending value is sent after it completes.
/// </summary>
public sealed class CommandThrottle
{
    private readonly EngineClient _client;
    private readonly Dictionary<string, string> _pending = new();
    private readonly HashSet<string> _inFlight = new();
    private readonly object _lock = new();

    public CommandThrottle(EngineClient client) => _client = client;

    public DateTime LastLocalChangeUtc { get; private set; } = DateTime.MinValue;
    public event Action<string>? Error;

    public void Post(string key, string command)
    {
        LastLocalChangeUtc = DateTime.UtcNow;
        lock (_lock)
        {
            _pending[key] = command;
            if (!_inFlight.Add(key)) return;   // the running pump will pick it up
        }
        _ = PumpAsync(key);
    }

    private async Task PumpAsync(string key)
    {
        while (true)
        {
            string? cmd;
            lock (_lock)
            {
                if (!_pending.Remove(key, out cmd) || cmd is null)
                {
                    _inFlight.Remove(key);
                    return;
                }
            }
            var r = await _client.SendAsync(cmd).ConfigureAwait(false);
            if (!r.Ok) Error?.Invoke(r.Message);
        }
    }
}
