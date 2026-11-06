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
    private readonly Icon _normal = IconFactory.CreateIcon(32, false);
    private readonly Icon _muted = IconFactory.CreateIcon(32, true);

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
        menu.ForeColor = Color.FromArgb(0xE6, 0xE8, 0xEC);
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
                ForeColor = Color.FromArgb(0xE6, 0xE8, 0xEC),
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
        private static readonly Color Bg = Color.FromArgb(0x1E, 0x22, 0x2B);
        private static readonly Color Hover = Color.FromArgb(0x2C, 0x33, 0x40);
        private static readonly Color Line = Color.FromArgb(0x2A, 0x2F, 0x3A);
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
        public override Color CheckBackground => Color.FromArgb(0x4F, 0x8C, 0xFF);
        public override Color CheckSelectedBackground => Color.FromArgb(0x4F, 0x8C, 0xFF);
    }
}
