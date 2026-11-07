using System;
using System.Linq;
using System.Windows;
using System.Windows.Interop;
using LegacyAVR.UI.Interop;

namespace LegacyAVR.UI;

/// <summary>v1.5.0: small dark dialog that asks for a name (used by the footer quick save).</summary>
public partial class NamePromptWindow : Window
{
    public NamePromptWindow(string prompt, string suggestion)
    {
        InitializeComponent();
        Prompt.Text = prompt;
        NameBox.Text = suggestion;
        Loaded += (_, _) =>
        {
            NameBox.Focus();
            NameBox.SelectAll();
        };
    }

    /// <summary>The name entered, or null when cancelled / empty.</summary>
    public string? Result { get; private set; }

    /// <summary>Shows the dialog over the active LegacyAVR window and returns the name (null = cancelled).</summary>
    public static string? Ask(string prompt, string suggestion)
    {
        var dlg = new NamePromptWindow(prompt, suggestion);
        var owner = Application.Current?.Windows.OfType<Window>().FirstOrDefault(w => w.IsActive)
                    ?? Application.Current?.MainWindow;
        if (owner is { IsVisible: true } && !ReferenceEquals(owner, dlg)) dlg.Owner = owner;
        else dlg.WindowStartupLocation = WindowStartupLocation.CenterScreen;
        return dlg.ShowDialog() == true ? dlg.Result : null;
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        NativeMethods.ApplyDarkChrome(new WindowInteropHelper(this).Handle);
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        var name = NameBox.Text.Trim();
        if (name.Length == 0)
        {
            NameBox.Focus();
            return;
        }
        Result = name;
        DialogResult = true;
    }
}
