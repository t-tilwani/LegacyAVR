using System.Windows;
using System.Windows.Controls;
using LegacyAVR.UI.ViewModels;

namespace LegacyAVR.UI.Controls;

/// <summary>
/// Settings guide (v1.2.0): picks the live control for a help entry - the DataTemplate
/// with key "HelpCtl.&lt;HelpEntry.Control&gt;" from the window resources. The templates bind to
/// the same MainViewModel properties as the real controls, so using them changes the setting.
/// </summary>
public sealed class HelpControlSelector : DataTemplateSelector
{
    public override DataTemplate? SelectTemplate(object? item, DependencyObject container)
    {
        if (item is not HelpEntry { HasControl: true } e || container is not FrameworkElement fe) return null;
        return fe.TryFindResource("HelpCtl." + e.Control) as DataTemplate;
    }
}
