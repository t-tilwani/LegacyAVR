using System;
using System.IO.MemoryMappedFiles;
using System.Threading;
using LegacyAVR.UI.Interop;

namespace LegacyAVR.UI.Services;

/// <summary>
/// Reads the engine's 1 KiB telemetry block from shared memory (seqlock reader side).
/// Tries the service mapping (Global\) first, then the per-user engine (Local\).
/// </summary>
public sealed unsafe class TelemetryReader : IDisposable
{
    public const string GlobalName = @"Global\LegacyAVR.Telemetry.v1";
    public const string LocalName = @"Local\LegacyAVR.Telemetry.v1";

    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _view;
    private byte* _ptr;
    private ulong _lastCounter;
    private DateTime _lastChange = DateTime.MinValue;

    public bool IsOpen => _ptr != null;
    public bool IsServiceMapping { get; private set; }

    public bool TryOpen()
    {
        if (IsOpen) return true;
        foreach (var (name, service) in new[] { (GlobalName, true), (LocalName, false) })
        {
            try
            {
                _mmf = MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.Read);
                _view = _mmf.CreateViewAccessor(0, TelemetryBlock.Size, MemoryMappedFileAccess.Read);
                byte* p = null;
                _view.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
                _ptr = p + _view.PointerOffset;
                IsServiceMapping = service;
                _lastChange = DateTime.UtcNow;
                return true;
            }
            catch (Exception)
            {
                Close();
            }
        }
        return false;
    }

    /// <summary>Consistent snapshot, or false if the engine is mid-write / gone / stale.</summary>
    public bool TryRead(out TelemetryBlock block)
    {
        block = default;
        if (!IsOpen) return false;
        uint* seq = (uint*)(_ptr + 8);
        for (int attempt = 0; attempt < 16; attempt++)
        {
            uint s1 = Volatile.Read(ref *seq);
            if ((s1 & 1) != 0) { Thread.SpinWait(32); continue; }
            block = *(TelemetryBlock*)_ptr;
            Interlocked.MemoryBarrier();
            uint s2 = Volatile.Read(ref *seq);
            if (s1 != s2) continue;
            if (!block.IsValid) return false;

            if (block.UpdateCounter != _lastCounter)
            {
                _lastCounter = block.UpdateCounter;
                _lastChange = DateTime.UtcNow;
            }
            return true;
        }
        return false;
    }

    /// <summary>The engine publishes at ~30 Hz; no change for 2 s means it exited or hung.</summary>
    public bool IsStale => !IsOpen || (DateTime.UtcNow - _lastChange).TotalSeconds > 2.0;

    public void Close()
    {
        if (_view != null)
        {
            if (_ptr != null) _view.SafeMemoryMappedViewHandle.ReleasePointer();
            _view.Dispose();
        }
        _mmf?.Dispose();
        _view = null;
        _mmf = null;
        _ptr = null;
        _lastCounter = 0;
    }

    public void Dispose() => Close();
}
