using System;
using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;

namespace LegacyAVR.UI.Services;

/// <summary>
/// Notification-area icon. Double-click restores the dashboard. Context menu:
/// Show Dashboard, Mute All, Quick Switch Profile, Exit. Uses a dark menu renderer.
/// </summary>
public sealed class TrayIconService : IDisposable
{
    private readonly NotifyIcon _icon;
    private readonly ToolStripMenuItem _muteItem;
    private readonly ToolStripMenuItem _presetMenu;
    // v1.6.0: the notification area shows small icons - use the image drawn for that size
    // (16 px at 100 % scaling, larger at higher system DPI) rather than shrinking a 32 px one.
    private static int TrayIconSize => Math.Max(16, SystemInformation.SmallIconSize.Width);
    private readonly Icon _normal = IconFactory.CreateIcon(TrayIconSize, false);
    private readonly Icon _muted = IconFactory.CreateIcon(TrayIconSize, true);

    public event EventHandler? ShowRequested;
    public event EventHandler<bool>? MuteAllToggled;
    public event EventHandler<Profile>? ProfileSelected;
    public event EventHandler? ExitRequested;

    public TrayIconService()
    {
        var menu = new ContextMenuStrip
        {
            Renderer = new ToolStripProfessionalRenderer(new DarkColors()),
            ShowImageMargin = false,
            ShowCheckMargin = true,   // keeps the "Mute All" / active-preset check marks
        };
        menu.ForeColor = Color.FromArgb(0xE8, 0xE6, 0xE6);
        menu.Font = new Font("Segoe UI", 9.5f);

        var show = new ToolStripMenuItem("Show Dashboard") { Font = new Font("Segoe UI", 9.5f, FontStyle.Bold) };
        show.Click += (_, _) => ShowRequested?.Invoke(this, EventArgs.Empty);
        _muteItem = new ToolStripMenuItem("Mute All") { CheckOnClick = true };
        _muteItem.CheckedChanged += (_, _) => MuteAllToggled?.Invoke(this, _muteItem.Checked);
        _presetMenu = new ToolStripMenuItem("Quick Switch Profile");
        _presetMenu.DropDown.Renderer = menu.Renderer;
        if (_presetMenu.DropDown is ToolStripDropDownMenu dd)
        {
            dd.ShowImageMargin = false;
            dd.ShowCheckMargin = true;
            dd.Font = menu.Font;
        }
        var exit = new ToolStripMenuItem("Exit");
        exit.Click += (_, _) => ExitRequested?.Invoke(this, EventArgs.Empty);

        menu.Items.AddRange(new ToolStripItem[] { show, _muteItem, _presetMenu, new ToolStripSeparator(), exit });
        foreach (ToolStripItem i in menu.Items) i.ForeColor = menu.ForeColor;

        _icon = new NotifyIcon
        {
            Icon = _normal,
            Text = "LegacyAVR",
            ContextMenuStrip = menu,
            Visible = true,
        };
        _icon.DoubleClick += (_, _) => ShowRequested?.Invoke(this, EventArgs.Empty);
    }

    public void SetProfiles(IEnumerable<Profile> profiles, string? active)
    {
        _presetMenu.DropDownItems.Clear();
        foreach (var p in profiles)
        {
            var item = new ToolStripMenuItem(p.Name)
            {
                Checked = string.Equals(p.Name, active, StringComparison.OrdinalIgnoreCase),
                ForeColor = Color.FromArgb(0xE8, 0xE6, 0xE6),
                ToolTipText = p.Display,
            };
            var captured = p;
            item.Click += (_, _) => ProfileSelected?.Invoke(this, captured);
            _presetMenu.DropDownItems.Add(item);
        }
    }

    /// <summary>Updates icon, tooltip (max 127 chars) and the Mute All check without re-raising.</summary>
    public void UpdateStatus(string tooltip, bool muteAll)
    {
        if (_disposed) return;
        var text = tooltip.Length > 127 ? tooltip[..127] : tooltip;
        if (_icon.Text != text) _icon.Text = text;
        var icon = muteAll ? _muted : _normal;
        if (!ReferenceEquals(_icon.Icon, icon)) _icon.Icon = icon;
        if (_muteItem.Checked != muteAll)
        {
            _muteItem.CheckOnClick = false;
            _muteItem.Checked = muteAll;
            _muteItem.CheckOnClick = true;
        }
    }

    public void ShowBalloon(string title, string text) =>
        _icon.ShowBalloonTip(3000, title, text, ToolTipIcon.Info);

    private bool _disposed;

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _icon.Visible = false;
        _icon.Dispose();
        _normal.Dispose();
        _muted.Dispose();
    }

    private sealed class DarkColors : ProfessionalColorTable
    {
        private static readonly Color Bg = Color.FromArgb(0x1F, 0x1F, 0x23);
        private static readonly Color Hover = Color.FromArgb(0x29, 0x29, 0x2E);
        private static readonly Color Line = Color.FromArgb(0x2E, 0x2E, 0x34);
        public override Color ToolStripDropDownBackground => Bg;
        public override Color MenuBorder => Line;
        public override Color MenuItemBorder => Hover;
        public override Color MenuItemSelected => Hover;
        public override Color MenuItemSelectedGradientBegin => Hover;
        public override Color MenuItemSelectedGradientEnd => Hover;
        public override Color MenuItemPressedGradientBegin => Hover;
        public override Color MenuItemPressedGradientEnd => Hover;
        public override Color ImageMarginGradientBegin => Bg;
        public override Color ImageMarginGradientMiddle => Bg;
        public override Color ImageMarginGradientEnd => Bg;
        public override Color SeparatorDark => Line;
        public override Color SeparatorLight => Bg;
        // v1.6.0: brand colours (Themes\Dark.xaml): check mark tile in the accent red.
        public override Color CheckBackground => Color.FromArgb(0xA3, 0x28, 0x2C);
        public override Color CheckSelectedBackground => Color.FromArgb(0xA3, 0x28, 0x2C);
        public override Color CheckPressedBackground => Color.FromArgb(0x73, 0x10, 0x12);
    }
}
