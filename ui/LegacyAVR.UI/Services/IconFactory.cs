using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using LegacyAVR.UI.Interop;

namespace LegacyAVR.UI.Services;

/// <summary>
/// Draws the app/tray icon at runtime (no binary .ico in the repo): a rounded accent
/// tile with five "speaker level" bars; muted state uses a red tile with a slash.
/// </summary>
public static class IconFactory
{
    public static Icon CreateIcon(int size, bool muted)
    {
        using var bmp = new Bitmap(size, size);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(System.Drawing.Color.Transparent);
            var tile = muted ? System.Drawing.Color.FromArgb(0xEF, 0x44, 0x44) : System.Drawing.Color.FromArgb(0x4F, 0x8C, 0xFF);
            float r = size * 0.22f;
            using (var path = RoundedRect(new RectangleF(0.5f, 0.5f, size - 1f, size - 1f), r))
            using (var b = new SolidBrush(tile))
                g.FillPath(b, path);

            float[] heights = { 0.35f, 0.6f, 0.85f, 0.6f, 0.35f };
            float barW = size * 0.11f, gap = size * 0.06f;
            float total = heights.Length * barW + (heights.Length - 1) * gap;
            float x = (size - total) / 2f;
            using var white = new SolidBrush(System.Drawing.Color.White);
            foreach (float barHeight in heights)
            {
                float bh = size * 0.62f * barHeight;
                g.FillRectangle(white, x, (size - bh) / 2f, barW, bh);
                x += barW + gap;
            }
            if (muted)
            {
                using var pen = new System.Drawing.Pen(System.Drawing.Color.White, size * 0.09f);
                g.DrawLine(pen, size * 0.2f, size * 0.8f, size * 0.8f, size * 0.2f);
            }
        }
        IntPtr hIcon = bmp.GetHicon();
        try
        {
            using var tmp = Icon.FromHandle(hIcon);
            return (Icon)tmp.Clone();
        }
        finally
        {
            NativeMethods.DestroyIcon(hIcon);
        }
    }

    public static ImageSource CreateImageSource(int size)
    {
        using var icon = CreateIcon(size, false);
        var src = Imaging.CreateBitmapSourceFromHIcon(icon.Handle, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
        src.Freeze();
        return src;
    }

    private static GraphicsPath RoundedRect(RectangleF r, float radius)
    {
        var p = new GraphicsPath();
        float d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }
}
