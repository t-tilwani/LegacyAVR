using System;
using System.ComponentModel;
using System.Windows;
using System.Windows.Interop;
using LegacyAVR.UI.Interop;

namespace LegacyAVR.UI;

/// <summary>
/// Dashboard window. Closing (X) hides it to the notification area - the engine keeps
/// running headless. Only the tray "Exit" item really quits.
/// </summary>
public partial class MainWindow : Window
{
    public event EventHandler? ClosedToTray;

    public MainWindow()
    {
        InitializeComponent();
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        NativeMethods.ApplyDarkChrome(new WindowInteropHelper(this).Handle);
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!App.IsExiting)
        {
            e.Cancel = true;   // close-to-tray
            Hide();
            ClosedToTray?.Invoke(this, EventArgs.Empty);
            return;
        }
        base.OnClosing(e);
    }

    protected override void OnStateChanged(EventArgs e)
    {
        base.OnStateChanged(e);
        // Minimise also goes to the tray so the taskbar stays clean.
        if (WindowState == WindowState.Minimized)
        {
            Hide();
            WindowState = WindowState.Normal;
        }
    }
}
