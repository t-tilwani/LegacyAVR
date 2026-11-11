using System;
using System.Runtime.InteropServices;
using System.Text;

namespace LegacyAVR.UI.Interop;

/// <summary>
/// Managed mirror of avr::ipc::TelemetryBlock (engine/ipc/TelemetryLayout.h).
/// 2048 bytes (layout v5, LegacyAVR 1.4.0 - 1.5.0), Pack = 1. Field order and sizes MUST match the C++ struct exactly;
/// LegacyAVR.UI.Tests pins every offset that the C++ side static_asserts.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct TelemetryBlock
{
    public const uint ExpectedMagic = 0x5256414C; // "LAVR"
    public const uint ExpectedVersion = 5;   // v5: LegacyAVR 1.4.0 (custom EQ, audition)
    public const int Size = 2048;

    public uint Magic;                 //   0
    public uint Version;               //   4
    public uint Sequence;              //   8
    public uint EngineState;           //  12
    public ulong UpdateCounter;        //  16
    public ulong QpcTimestamp;         //  24
    // input
    public uint InSampleRate;          //  32
    public ushort InChannels;          //  36
    public ushort InBitsPerSample;     //  38
    public uint InChannelMask;         //  40
    public uint InStreamKind;          //  44
    public uint InContentMask;         //  48
    // output
    public uint OutCodec;              //  52
    public uint OutBitrate;            //  56
    public uint OutSampleRate;         //  60
    public ushort OutChannels;         //  64
    public ushort OutMode;             //  66
    public uint UpmixMode;             //  68
    public uint Flags;                 //  72
    public float MasterDb;             //  76
    public float CpuPercent;           //  80
    public float DspLoadPercent;       //  84
    public float LatencyMs;            //  88
    public float CaptureBufferMs;      //  92
    public float OutputBufferMs;       //  96
    public float DriftPpm;             // 100
    public float Correlation;          // 104
    public float DialogActivity;       // 108
    public float NightGainReductionDb; // 112
    public float LimiterReductionDb;   // 116
    public ulong FramesEncoded;        // 120
    public uint Underruns;             // 128
    public uint Overruns;              // 132
    public uint InputDrops;            // 136
    public uint TestSignal;            // 140
    public int TestChannel;            // 144
    public float TestFrequencyHz;      // 148
    public uint TestStep;              // 152
    public uint ConfigRevision;        // 156
    public fixed float InPeakDb[8];    // 160
    public fixed float InRmsDb[8];     // 192
    public fixed float OutPeakDb[6];   // 224
    public fixed float OutRmsDb[6];    // 248
    public fixed float OutGainDb[6];   // 272
    public fixed byte OutMuted[6];     // 296
    public ushort Reserved0;           // 302
    public fixed byte InputDescription[64];   // 304
    public fixed byte OutputDescription[96];  // 368
    public fixed byte InputDevice[128];       // 464
    public fixed byte OutputDevice[128];      // 592
    public fixed byte LastError[160];         // 720
    // v2: requested vs. effective output
    public uint RequestedCodec;               // 880
    public uint RequestedBitrate;             // 884
    public uint RequestedMode;                // 888
    public uint OutputStatus;                 // 892
    public fixed byte EffectiveReason[128];   // 896
    // v3: per-stage latency (ms)
    public float LatCaptureMs;                // 1024
    public float LatCaptureQueueMs;           // 1028
    public float LatResamplerMs;              // 1032
    public float LatAssemblyMs;               // 1036
    public float LatOutputQueueMs;            // 1040
    public float LatEncoderMs;                // 1044
    public float LatDeviceBufferMs;           // 1048
    public float LatDeviceStreamMs;           // 1052
    public float LatMarginMs;                 // 1056
    public float LatFloorMs;                  // 1060
    public float LatRenderPeriodMs;           // 1064
    public float LatCapturePacketMs;          // 1068
    // v3: analysis / processing
    public float FreqLowHz;                   // 1072
    public float FreqHighHz;                  // 1076
    public uint ProcState;                    // 1080
    public uint Flags2;                       // 1084
    public float LfeGenGain;                  // 1088
    public float BassDb;                      // 1092
    public uint LfeGenMode;                   // 1096
    public uint GamingMode;                   // 1100
    public uint PcmSwitchStatus;              // 1104
    public uint PcmBlockMask;                 // 1108
    public float PcmHoldSeconds;              // 1112
    public uint EmitChannels;                 // 1116
    public uint EmitMask;                     // 1120
    public uint BufferMsSetting;              // 1124
    public fixed float EmitPeakDb[8];         // 1128
    public fixed float EmitRmsDb[8];          // 1160
    public float LfeFoldPeakDb;               // 1192
    public float LfeFoldRmsDb;                // 1196
    public fixed byte ProcessingText[96];     // 1200
    public fixed byte PcmSwitchText[128];     // 1296
    public fixed byte GameName[64];           // 1424
    // v4: LFE status / native-LFE inhibit
    public uint LfeStatus;                    // 1488
    public float LfeResetSeconds;             // 1492
    public float LfeSilenceSeconds;           // 1496
    public fixed byte LfeStatusText[96];      // 1500
    // ---- v5 (LegacyAVR 1.4.0)
    public float EqHeadroomDb;                // 1596 level taken off so EQ boosts cannot clip (<= 0)
    public int AuditionChannel;               // 1600 soloed bus channel, -1 = off
    public uint EqActiveMask;                 // 1604 bus channels the EQ is filtering
    public fixed byte Reserved5[440];         // 1608 -> 2048

    public bool IsValid => Magic == ExpectedMagic && Version == ExpectedVersion;
    public bool HasFlag(TelemetryFlags f) => (Flags & (uint)f) != 0;

    public string InputDescriptionText { get { fixed (byte* p = InputDescription) return Utf8(p, 64); } }
    public string OutputDescriptionText { get { fixed (byte* p = OutputDescription) return Utf8(p, 96); } }
    public string InputDeviceText { get { fixed (byte* p = InputDevice) return Utf8(p, 128); } }
    public string OutputDeviceText { get { fixed (byte* p = OutputDevice) return Utf8(p, 128); } }
    public string LastErrorText { get { fixed (byte* p = LastError) return Utf8(p, 160); } }
    public string EffectiveReasonText { get { fixed (byte* p = EffectiveReason) return Utf8(p, 128); } }
    public string ProcessingTextValue { get { fixed (byte* p = ProcessingText) return Utf8(p, 96); } }
    public string PcmSwitchTextValue { get { fixed (byte* p = PcmSwitchText) return Utf8(p, 128); } }
    public string GameNameText { get { fixed (byte* p = GameName) return Utf8(p, 64); } }
    public string LfeStatusTextValue { get { fixed (byte* p = LfeStatusText) return Utf8(p, 96); } }
    public bool HasFlag2(TelemetryFlags2 f) => (Flags2 & (uint)f) != 0;

    private static string Utf8(byte* p, int max)
    {
        int n = 0;
        while (n < max && p[n] != 0) n++;
        return n == 0 ? string.Empty : Encoding.UTF8.GetString(p, n);
    }
}

[Flags]
public enum TelemetryFlags : uint
{
    NightMode = 1 << 0,
    MuteAll = 1 << 1,
    Passthrough = 1 << 2,
    InputActive = 1 << 3,
    TestActive = 1 << 4,
    BitstreamBlocked = 1 << 5,
    ServiceMode = 1 << 6,
    SpeechAvailable = 1 << 7,
    StereoAs20 = 1 << 8,
    AudioStopped = 1 << 9,
    StereoPcmActive = 1 << 10,
}

/// <summary>Second flag word (layout v3).</summary>
[Flags]
public enum TelemetryFlags2 : uint
{
    DedicatedSub = 1 << 0,
    GamingActive = 1 << 1,
    LfeGenActive = 1 << 2,
    BufferAuto = 1 << 3,
    Encoder50 = 1 << 4,
    GameLfeGen = 1 << 5,
    LfeGameOverride = 1 << 6,
    LfeInhibited = 1 << 7,
    EqEnabled = 1 << 8,          // v5: Custom EQ master switch on (replaces the bass control)
    LfeFold = 1 << 9,            // v1.5.0: source LFE folded into FL/FR when the output has no LFE channel
    NoLfeOutput = 1 << 10,       // v1.5.0: the last block left without an LFE channel (v1.6.0: per block)
    ExpLfeGen = 1 << 11,         // v1.6.0: experimental LFE generation enabled
    ExpLfeFold = 1 << 12,        // v1.6.0: experimental optional LFE fold enabled
    LfeFolded = 1 << 13,         // v1.6.0: the source LFE is folded into FL/FR right now
    ChimePlaying = 1 << 14,      // v1.6.0: the activation chime is playing
}

/// <summary>Numeric values match avr::EngineState / Codec / OutputMode / UpmixMode / TestSignal.</summary>
public enum EngineState : uint { Stopped = 0, Starting = 1, Running = 2, Reconfiguring = 3, Error = 4, Idle = 5 }
public enum CodecKind : uint { AC3 = 0, DTS = 1, EAC3 = 2, PCM = 3 }
public enum OutputModeKind : uint { Exclusive = 0, Shared = 1 }
public enum UpmixKind : uint { Passthrough = 0, Upmix31 = 1, Upmix51 = 2, PcmDirect = 3 }
public enum OutputStatusKind : uint { Match = 0, FallbackShared = 1, BitstreamPassthrough = 2, StereoPcmDirect = 3, StoppedByUser = 4, Error = 5 }
public enum TestSignalKind : uint { Off = 0, PinkNoise = 1, ChannelId = 2, CenterBleed = 3, LfeSweep = 4, PhaseCheck = 5 }
public enum LfeGenKind : uint { Off = 0, Auto = 1, Always = 2 }
public enum LfeStatusKind : uint { None = 0, Source = 1, Generated = 2, Upmixed = 3, Inhibited = 4, TestSignal = 5, Dropped = 6, Folded = 7 }
public enum GamingKind : uint { Auto = 0, On = 1, Off = 2 }
public enum ProcessingStateKind : uint
{
    Idle = 0, StereoUpmix51 = 1, StereoUpmix31 = 2, StereoKept = 3, StereoPcm = 4, StereoLfeGen = 5,
    Discrete = 6, LfeGen = 7, FoldDown71 = 8, FoldDown71LfeGen = 9, Bitstream = 10, TestSignal = 11,
}
public enum PcmSwitchStatusKind : uint
{
    NotApplicable = 0, WaitingForStereo = 1, Requested = 2, Active = 3, BlockedSurround = 4, BlockedTest = 5,
    BlockedPassthrough = 6, BlockedOutput = 7, BlockedGaming = 8, NoSignal = 9, ReturningToBitstream = 10,
}
