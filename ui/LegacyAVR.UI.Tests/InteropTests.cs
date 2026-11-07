using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.Services;
using LegacyAVR.UI.ViewModels;
using Xunit;

namespace LegacyAVR.UI.Tests;

/// <summary>
/// Pins the managed TelemetryBlock to the C++ layout in engine/ipc/TelemetryLayout.h.
/// Every offset below is also static_assert-ed (or unit-tested) on the C++ side.
/// </summary>
public class TelemetryLayoutTests
{
    [Fact]
    public void SizeIs2048() => Assert.Equal(2048, Unsafe.SizeOf<TelemetryBlock>());

    [Fact]
    public void MarshalSizeIs2048() => Assert.Equal(2048, Marshal.SizeOf<TelemetryBlock>());

    [Fact]
    public void SizeConstantMatches() => Assert.Equal(TelemetryBlock.Size, Unsafe.SizeOf<TelemetryBlock>());

    [Theory]
    [InlineData(nameof(TelemetryBlock.Sequence), 8)]
    [InlineData(nameof(TelemetryBlock.UpdateCounter), 16)]
    [InlineData(nameof(TelemetryBlock.InSampleRate), 32)]
    [InlineData(nameof(TelemetryBlock.InContentMask), 48)]
    [InlineData(nameof(TelemetryBlock.OutCodec), 52)]
    [InlineData(nameof(TelemetryBlock.OutChannels), 64)]
    [InlineData(nameof(TelemetryBlock.Flags), 72)]
    [InlineData(nameof(TelemetryBlock.MasterDb), 76)]
    [InlineData(nameof(TelemetryBlock.LimiterReductionDb), 116)]
    [InlineData(nameof(TelemetryBlock.FramesEncoded), 120)]
    [InlineData(nameof(TelemetryBlock.ConfigRevision), 156)]
    [InlineData(nameof(TelemetryBlock.InPeakDb), 160)]
    [InlineData(nameof(TelemetryBlock.InRmsDb), 192)]
    [InlineData(nameof(TelemetryBlock.OutPeakDb), 224)]
    [InlineData(nameof(TelemetryBlock.OutRmsDb), 248)]
    [InlineData(nameof(TelemetryBlock.OutGainDb), 272)]
    [InlineData(nameof(TelemetryBlock.OutMuted), 296)]
    [InlineData(nameof(TelemetryBlock.InputDescription), 304)]
    [InlineData(nameof(TelemetryBlock.OutputDescription), 368)]
    [InlineData(nameof(TelemetryBlock.InputDevice), 464)]
    [InlineData(nameof(TelemetryBlock.OutputDevice), 592)]
    [InlineData(nameof(TelemetryBlock.LastError), 720)]
    [InlineData(nameof(TelemetryBlock.RequestedCodec), 880)]
    [InlineData(nameof(TelemetryBlock.OutputStatus), 892)]
    [InlineData(nameof(TelemetryBlock.EffectiveReason), 896)]
    [InlineData(nameof(TelemetryBlock.LatCaptureMs), 1024)]
    [InlineData(nameof(TelemetryBlock.LatCapturePacketMs), 1068)]
    [InlineData(nameof(TelemetryBlock.FreqLowHz), 1072)]
    [InlineData(nameof(TelemetryBlock.ProcState), 1080)]
    [InlineData(nameof(TelemetryBlock.Flags2), 1084)]
    [InlineData(nameof(TelemetryBlock.PcmSwitchStatus), 1104)]
    [InlineData(nameof(TelemetryBlock.EmitChannels), 1116)]
    [InlineData(nameof(TelemetryBlock.BufferMsSetting), 1124)]
    [InlineData(nameof(TelemetryBlock.EmitPeakDb), 1128)]
    [InlineData(nameof(TelemetryBlock.EmitRmsDb), 1160)]
    [InlineData(nameof(TelemetryBlock.LfeFoldPeakDb), 1192)]
    [InlineData(nameof(TelemetryBlock.ProcessingText), 1200)]
    [InlineData(nameof(TelemetryBlock.PcmSwitchText), 1296)]
    [InlineData(nameof(TelemetryBlock.GameName), 1424)]
    [InlineData(nameof(TelemetryBlock.LfeStatus), 1488)]
    [InlineData(nameof(TelemetryBlock.LfeResetSeconds), 1492)]
    [InlineData(nameof(TelemetryBlock.LfeStatusText), 1500)]
    [InlineData(nameof(TelemetryBlock.EqHeadroomDb), 1596)]
    [InlineData(nameof(TelemetryBlock.AuditionChannel), 1600)]
    [InlineData(nameof(TelemetryBlock.EqActiveMask), 1604)]
    [InlineData(nameof(TelemetryBlock.Reserved5), 1608)]
    public void FieldOffsetsMatchNative(string field, int offset) =>
        Assert.Equal(offset, (int)Marshal.OffsetOf<TelemetryBlock>(field));

    [Fact]
    public unsafe void DecodesUtf8Strings()
    {
        var b = new TelemetryBlock { Magic = TelemetryBlock.ExpectedMagic, Version = TelemetryBlock.ExpectedVersion };
        var bytes = Encoding.UTF8.GetBytes("PCM 5.1 @ 48 kHz");
        for (int i = 0; i < bytes.Length; i++) b.InputDescription[i] = bytes[i];
        Assert.Equal("PCM 5.1 @ 48 kHz", b.InputDescriptionText);
        Assert.True(b.IsValid);
    }
}

public class ProtocolTests
{
    [Fact]
    public void NumbersAreCultureInvariant()
    {
        var saved = System.Globalization.CultureInfo.CurrentCulture;
        try
        {
            System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("de-DE");
            Assert.Equal("-4.5", Proto.F(-4.5));
        }
        finally
        {
            System.Globalization.CultureInfo.CurrentCulture = saved;
        }
    }

    [Fact]
    public void ResponseKeyValues()
    {
        var r = new EngineResponse(true, "", new[] { "codec=AC3", "gain0=-3.0", "junk" });
        var kv = r.KeyValues();
        Assert.Equal("AC3", kv["codec"]);
        Assert.Equal("-3.0", kv["GAIN0"]);
        Assert.False(kv.ContainsKey("junk"));
    }

    [Theory]
    [InlineData(UpmixKind.Passthrough, "PASSTHROUGH")]
    [InlineData(UpmixKind.Upmix31, "UPMIX31")]
    [InlineData(UpmixKind.Upmix51, "UPMIX51")]
    [InlineData(UpmixKind.PcmDirect, "PCM_DIRECT")]
    public void UpmixTokens(UpmixKind k, string token) => Assert.Equal(token, ProfileStore.UpmixToken(k));

    [Theory]
    [InlineData(LfeGenKind.Off, "OFF")]
    [InlineData(LfeGenKind.Auto, "AUTO")]
    [InlineData(LfeGenKind.Always, "ALWAYS")]
    public void LfeGenTokens(LfeGenKind k, string token) => Assert.Equal(token, ProfileStore.LfeGenToken(k));

    [Theory]
    [InlineData(GamingKind.Auto, "AUTO")]
    [InlineData(GamingKind.On, "ON")]
    [InlineData(GamingKind.Off, "OFF")]
    public void GamingTokens(GamingKind k, string token) => Assert.Equal(token, ProfileStore.GamingToken(k));
}

public class FormatSupportTests
{
    [Theory]
    [InlineData(0x80u | 0x1u, CodecKind.AC3, true)]
    [InlineData(0x80u | 0x1u, CodecKind.DTS, false)]
    [InlineData(0x80u | 0x7u, CodecKind.EAC3, true)]
    [InlineData(0x80u, CodecKind.AC3, false)]          // probed: nothing supported
    [InlineData(0x0u, CodecKind.AC3, null)]            // not probed (device busy): unknown
    [InlineData(0x80u, CodecKind.PCM, true)]           // PCM always works
    public void Support(uint caps, CodecKind codec, bool? expected) =>
        Assert.Equal(expected, CodecChoice.IsSupported(caps, codec));

    [Fact]
    public void ChoiceTextFlagsUnsupported()
    {
        var c = new CodecChoice("DTS Interactive", CodecKind.DTS);
        c.Update(0x81);
        Assert.False(c.Supported);
        Assert.Contains("not supported", c.Note);
        c.Update(0x83);
        Assert.True(c.Supported);
        Assert.Equal(string.Empty, c.Note);
    }
}

public class GameDetectorTests
{
    [Theory]
    [InlineData("chrome", false)]
    [InlineData("msedge", false)]
    [InlineData("vlc", false)]
    [InlineData("LegacyAVR.UI", false)]
    [InlineData("eldenring", true)]
    [InlineData("Cyberpunk2077", true)]
    public void ExcludesBrowsersAndPlayers(string process, bool candidate) =>
        Assert.Equal(candidate, GameDetector.IsGameCandidate(process));
}

public class ChannelStripTests
{
    [Fact]
    public void GainClampsAndSnapsToMute()
    {
        int sends = 0;
        var ch = new ChannelStripViewModel(0, "FL", "Front Left", _ => sends++, _ => { });
        ch.GainDb = 9;
        Assert.Equal(6.0, ch.GainDb);
        ch.GainDb = -48.4;                       // below -48.25 => mute position
        Assert.Equal(ChannelStripViewModel.MuteDb, ch.GainDb);
        Assert.Equal("-inf", ch.GainText);
        ch.GainDb = -3.26;
        Assert.Equal(-3.5, ch.GainDb);            // 0.5 dB steps
        Assert.Equal(3, sends);
    }

    [Fact]
    public void EngineSyncDoesNotEcho()
    {
        int sends = 0;
        var ch = new ChannelStripViewModel(1, "FR", "Front Right", _ => sends++, _ => sends++);
        ch.SetFromEngine(-6, true);
        Assert.Equal(0, sends);
        Assert.True(ch.IsMuted);
    }
}

public class OutputLayoutTests
{
    [Theory]
    [InlineData(0x60Fu, 6u, true, "5.1")]     // FL FR C LFE SL SR
    [InlineData(0x60Fu, 6u, false, "5.0")]    // same stream, LFE folded into the fronts
    [InlineData(0x607u, 5u, false, "5.0")]    // 5.0 bitstream
    [InlineData(0x3u, 2u, true, "2.0")]
    [InlineData(0x7u, 3u, false, "3.0")]      // v1.5.0: AC-3 / DD+ 3.0 without a subwoofer
    [InlineData(0xFu, 4u, true, "3.1")]       // v1.5.0: AC-3 / DD+ 3.1
    [InlineData(0x63Fu, 8u, true, "5.1 in 7.1")]    // the bus is 5.1: a 7.1 device has a silent back pair
    [InlineData(0x63Fu, 8u, false, "5.0 in 7.1")]
    [InlineData(0u, 0u, true, "")]            // passthrough / stopped: unknown
    public void Layout(uint mask, uint channels, bool lfeOut, string expected) =>
        Assert.Equal(expected, MainViewModel.OutputLayout(mask, channels, lfeOut));

    [Fact]
    public void SubLabel()
    {
        Assert.Equal("Dedicated subwoofer active (5.1)", MainViewModel.SubLabel(true, "5.1"));
        Assert.Equal("Dedicated subwoofer inactive (5.0)", MainViewModel.SubLabel(false, "5.0"));
        Assert.Equal("Dedicated subwoofer active", MainViewModel.SubLabel(true, ""));
    }
}
