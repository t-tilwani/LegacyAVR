using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;

namespace LegacyAVR.UI.Controls;

/// <summary>
/// v1.6.0: for value boxes bound with UpdateSourceTrigger=LostFocus. Enter commits the typed text
/// (the view model validates it), Escape throws the typing away and shows the current value again.
/// Both keys are handled here, so Escape does not also close the window.
/// </summary>
public static class TextBoxCommit
{
    public static readonly DependencyProperty EnabledProperty = DependencyProperty.RegisterAttached(
        "Enabled", typeof(bool), typeof(TextBoxCommit), new PropertyMetadata(false, OnEnabledChanged));

    public static bool GetEnabled(DependencyObject d) => (bool)d.GetValue(EnabledProperty);
    public static void SetEnabled(DependencyObject d, bool value) => d.SetValue(EnabledProperty, value);

    private static void OnEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not TextBox tb) return;
        tb.PreviewKeyDown -= OnPreviewKeyDown;
        tb.GotKeyboardFocus -= OnGotFocus;
        if (e.NewValue is true)
        {
            tb.PreviewKeyDown += OnPreviewKeyDown;
            tb.GotKeyboardFocus += OnGotFocus;
        }
    }

    private static void OnGotFocus(object sender, KeyboardFocusChangedEventArgs e)
    {
        if (sender is TextBox tb) tb.Dispatcher.InvokeAsync(tb.SelectAll);
    }

    private static void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (sender is not TextBox tb) return;
        BindingExpression? be = tb.GetBindingExpression(TextBox.TextProperty);
        if (e.Key == Key.Enter)
        {
            be?.UpdateSource();
            be?.UpdateTarget();   // show the formatted (or, if refused, the last valid) value
            tb.SelectAll();
            e.Handled = true;
        }
        else if (e.Key == Key.Escape)
        {
            be?.UpdateTarget();
            tb.SelectAll();
            e.Handled = true;
        }
    }
}
