using System;
using System.Windows;
using System.Windows.Interop;
using LegacyAVR.UI.Interop;
using LegacyAVR.UI.ViewModels;

namespace LegacyAVR.UI;

/// <summary>v1.6.0: "Apply band to other channels" dialog (Apply asks before overwriting; Cancel changes nothing).</summary>
public partial class EqCopyWindow : Window
{
    public EqCopyWindow(EqBandCopyViewModel vm)
    {
        InitializeComponent();
        DataContext = vm;
        vm.Applied += (_, _) => DialogResult = true;
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        NativeMethods.ApplyDarkChrome(new WindowInteropHelper(this).Handle);
    }
}
