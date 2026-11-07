using System;
using System.Linq;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.Services;
using LegacyAVR.UI.ViewModels;
using Xunit;

namespace LegacyAVR.UI.Tests;

// ============================================================================ v1.5.0
public class EmergencyResetTests
{
    [Fact]
    public void RelaunchArgumentCarriesTheOldProcessId()
    {
        Assert.Equal(1234, EmergencyReset.ParseWaitForExit(new[] { "--wait-for-exit", "1234" }));
        Assert.Equal(42, EmergencyReset.ParseWaitForExit(new[] { "--minimized", "--WAIT-FOR-EXIT", "42" }));
        Assert.Null(EmergencyReset.ParseWaitForExit(Array.Empty<string>()));
        Assert.Null(EmergencyReset.ParseWaitForExit(new[] { "--wait-for-exit" }));          // no id
        Assert.Null(EmergencyReset.ParseWaitForExit(new[] { "--wait-for-exit", "-5" }));    // not a process id
        Assert.Null(EmergencyReset.ParseWaitForExit(new[] { "--wait-for-exit", "abc" }));
    }

    [Fact]
    public void WaitingForAnExitedOrOwnProcessReturnsAtOnce()
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        EmergencyReset.WaitForPreviousInstance(new[] { "--wait-for-exit", Environment.ProcessId.ToString() }, 5000);
        EmergencyReset.WaitForPreviousInstance(new[] { "--wait-for-exit", int.MaxValue.ToString() }, 5000);
        Assert.True(sw.ElapsedMilliseconds < 2000);
    }

    [Fact]
    public void StopsBothEngineFlavours() =>
        Assert.Equal(new[] { "AVREngine", "AVREngineService" }, EmergencyReset.EngineProcessNames);
}

public class LfeTelemetryMirrorTests
{
    [Fact]
    public void NewFlagsAndStatusMatchTheEngine()
    {
        // engine/ipc/TelemetryLayout.h and engine/core/Types.h
        Assert.Equal(1u << 9, (uint)TelemetryFlags2.LfeFold);
        Assert.Equal(1u << 10, (uint)TelemetryFlags2.NoLfeOutput);
        Assert.Equal(6u, (uint)LfeStatusKind.Dropped);
        Assert.Equal(5u, TelemetryBlock.ExpectedVersion);   // additions only - the layout version is unchanged
    }
}

public class EqChannelNameTests
{
    [Fact]
    public void ChannelShowsItsDisplayNameEverywhere()
    {
        // The closed EQ channel box fell back to ToString() and showed the type name.
        var ch = new ChannelEqViewModel(3, "Subwoofer (LFE)", (_, _) => { }, _ => { });
        Assert.Equal("Subwoofer (LFE)", ch.ToString());
        ch.Name = "LFE → FL/FR (no subwoofer)";
        Assert.Equal("LFE → FL/FR (no subwoofer)", ch.ToString());
    }
}
