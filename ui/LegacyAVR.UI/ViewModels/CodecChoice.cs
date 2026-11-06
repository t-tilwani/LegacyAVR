using LegacyAVR.UI.Interop;

namespace LegacyAVR.UI.ViewModels;

/// <summary>
/// One entry of the Format drop-down. Marks formats the selected output device did not
/// report in its exclusive-mode probe (DeviceItem.IecCaps: bit0 AC-3, bit1 DTS, bit2 E-AC-3,
/// bit7 = the device answered the probe). Unsupported formats stay selectable (the probe
/// can be wrong behind some TVs) but are clearly labelled.
/// </summary>
public sealed class CodecChoice : ObservableObject
{
    public const uint CapsKnown = 0x80;

    private bool _supported = true;
    private bool _known;
    private string _note = string.Empty;

    public CodecChoice(string label, CodecKind value)
    {
        Label = label;
        Value = value;
    }

    public string Label { get; }
    public CodecKind Value { get; }

    /// <summary>False when the device answered the probe and this format was not accepted.</summary>
    public bool Supported { get => _supported; private set => Set(ref _supported, value); }
    /// <summary>True when the device's capabilities are known.</summary>
    public bool Known { get => _known; private set => Set(ref _known, value); }
    /// <summary>Short explanation shown next to the label ("" when supported).</summary>
    public string Note { get => _note; private set => Set(ref _note, value); }

    /// <summary>null = unknown (device was busy when probed).</summary>
    public static bool? IsSupported(uint caps, CodecKind codec)
    {
        if (codec == CodecKind.PCM) return true;
        if ((caps & CapsKnown) == 0) return null;
        uint bit = codec switch { CodecKind.AC3 => 1u, CodecKind.DTS => 2u, CodecKind.EAC3 => 4u, _ => 0u };
        return (caps & bit) != 0;
    }

    public void Update(uint caps)
    {
        var s = IsSupported(caps, Value);
        Known = s.HasValue;
        Supported = s ?? true;
        Note = s == false ? "not supported by this output - would fall back to PCM" : string.Empty;
    }

    public override string ToString() => Supported ? Label : Label + "  (not supported by this output)";
}
