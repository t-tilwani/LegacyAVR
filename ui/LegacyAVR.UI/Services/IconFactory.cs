using System;
using System.Drawing;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace LegacyAVR.UI.Services;

/// <summary>
/// App / tray icons. v1.6.0: the LegacyAVR logo's "AVR" lettering in white on a #731012 tile
/// (Assets\LegacyAVR.ico, built from <see cref="BrandLogo"/> by scripts\make-icons.py) and a grey
/// tile with a slash for Mute All (Assets\LegacyAVR-muted.ico). Each .ico holds images drawn for
/// 16-64 px and 256 px, so Windows and WPF pick a sharp image for every use and DPI instead of
/// scaling one bitmap.
/// </summary>
public static class IconFactory
{
    private static readonly Uri NormalUri = new("pack://application:,,,/Assets/LegacyAVR.ico", UriKind.Absolute);
    private static readonly Uri MutedUri = new("pack://application:,,,/Assets/LegacyAVR-muted.ico", UriKind.Absolute);

    /// <summary>A System.Drawing icon (tray) using the image made for <paramref name="size"/> px.</summary>
    public static Icon CreateIcon(int size, bool muted)
    {
        var info = System.Windows.Application.GetResourceStream(muted ? MutedUri : NormalUri)
                   ?? throw new InvalidOperationException("The LegacyAVR icon resource is missing from the build.");
        using var stream = info.Stream;
        return new Icon(stream, size, size);
    }

    /// <summary>The window icon: every image of the .ico, so WPF uses the 16 px one in the title
    /// bar and the 32 / 48 px ones in the taskbar and Alt+Tab, at any DPI.</summary>
    public static ImageSource CreateWindowIcon()
    {
        var frame = BitmapFrame.Create(NormalUri, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
        frame.Freeze();
        return frame;
    }
}
