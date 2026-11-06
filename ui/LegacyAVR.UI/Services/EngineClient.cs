using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace LegacyAVR.UI.Services;

/// <summary>Response to one control command: status line + data lines (protocol in ControlProtocol.h).</summary>
public sealed record EngineResponse(bool Ok, string Message, IReadOnlyList<string> Lines)
{
    public static EngineResponse Fail(string message) => new(false, message, Array.Empty<string>());

    public Dictionary<string, string> KeyValues()
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var l in Lines)
        {
            int eq = l.IndexOf('=');
            if (eq > 0) d[l[..eq]] = l[(eq + 1)..];
        }
        return d;
    }
}

/// <summary>Named-pipe client for \\.\pipe\LegacyAVR.Control. Thread-safe; one request at a time.</summary>
public sealed class EngineClient : IDisposable
{
    public const string PipeName = "LegacyAVR.Control";
    private static readonly UTF8Encoding Utf8 = new(false);

    private readonly SemaphoreSlim _gate = new(1, 1);
    private NamedPipeClientStream? _pipe;
    private StreamReader? _reader;
    private StreamWriter? _writer;

    public bool IsConnected => _pipe?.IsConnected == true;
    public event EventHandler<bool>? ConnectionChanged;

    public async Task<bool> ConnectAsync(int timeoutMs, CancellationToken ct = default)
    {
        await _gate.WaitAsync(ct).ConfigureAwait(false);
        try { return await ConnectCoreAsync(timeoutMs, ct).ConfigureAwait(false); }
        finally { _gate.Release(); }
    }

    private async Task<bool> ConnectCoreAsync(int timeoutMs, CancellationToken ct)
    {
        if (IsConnected) return true;
        DisconnectCore();
        var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            await pipe.ConnectAsync(timeoutMs, ct).ConfigureAwait(false);
        }
        catch (Exception) when (!ct.IsCancellationRequested)
        {
            await pipe.DisposeAsync().ConfigureAwait(false);
            return false;
        }
        _pipe = pipe;
        _reader = new StreamReader(pipe, Utf8, false, 4096, leaveOpen: true);
        _writer = new StreamWriter(pipe, Utf8, 4096, leaveOpen: true) { AutoFlush = false, NewLine = "\n" };
        ConnectionChanged?.Invoke(this, true);
        return true;
    }

    /// <summary>Sends one command line and waits for the "END" terminated response.</summary>
    public async Task<EngineResponse> SendAsync(string command, CancellationToken ct = default)
    {
        await _gate.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            if (!IsConnected && !await ConnectCoreAsync(400, ct).ConfigureAwait(false))
                return EngineResponse.Fail("Engine not reachable");

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromSeconds(8));   // LIST_DEVICES probes hardware

            await _writer!.WriteLineAsync(command.AsMemory(), timeout.Token).ConfigureAwait(false);
            await _writer.FlushAsync(timeout.Token).ConfigureAwait(false);

            string? status = await _reader!.ReadLineAsync(timeout.Token).ConfigureAwait(false);
            if (status is null) throw new IOException("pipe closed");
            var lines = new List<string>();
            while (true)
            {
                string? l = await _reader.ReadLineAsync(timeout.Token).ConfigureAwait(false);
                if (l is null) throw new IOException("pipe closed");
                if (l == "END") break;
                lines.Add(l);
            }
            bool ok = status.StartsWith("OK", StringComparison.Ordinal);
            string msg = status.Length > 3 ? status[3..] : string.Empty;
            return new EngineResponse(ok, msg, lines);
        }
        catch (Exception ex) when (ex is IOException or OperationCanceledException or ObjectDisposedException)
        {
            DisconnectCore();
            return EngineResponse.Fail("Engine connection lost");
        }
        finally
        {
            _gate.Release();
        }
    }

    private void DisconnectCore()
    {
        bool was = _pipe != null;
        _reader?.Dispose();
        _writer?.Dispose();
        _pipe?.Dispose();
        _reader = null;
        _writer = null;
        _pipe = null;
        if (was) ConnectionChanged?.Invoke(this, false);
    }

    public void Dispose()
    {
        DisconnectCore();
        _gate.Dispose();
    }
}

/// <summary>Formats numbers for the protocol with the invariant culture ("-4.5", never "-4,5").</summary>
public static class Proto
{
    public static string F(double v) => v.ToString("0.###", CultureInfo.InvariantCulture);
    public static string Quote(string s) => "\"" + s.Replace("\"", string.Empty) + "\"";
}
