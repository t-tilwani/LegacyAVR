using System;
using System.Windows;
using System.Windows.Interop;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.ViewModels;

namespace LegacyAVR.UI;

/// <summary>v1.4.0: per-channel custom EQ dialog (over the main window).</summary>
public partial class EqWindow : Window
{
    private readonly EqDialogViewModel _vm;

    public EqWindow(MainViewModel main, int channel)
    {
        InitializeComponent();
        _vm = new EqDialogViewModel(main, channel);
        DataContext = _vm;
    }

    /// <summary>Shows another channel in the already open dialog.</summary>
    public void SelectChannel(int channel)
    {
        if (channel >= 0 && channel < _vm.Main.EqChannels.Count) _vm.Channel = _vm.Main.EqChannels[channel];
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        NativeMethods.ApplyDarkChrome(new WindowInteropHelper(this).Handle);
    }

    protected override void OnClosed(EventArgs e)
    {
        _vm.Dispose();   // ends auditioning
        base.OnClosed(e);
    }

    private void OnClose(object sender, RoutedEventArgs e) => Close();
}
