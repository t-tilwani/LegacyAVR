using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using LegacyAVR.UI.Services;
using LegacyAVR.UI.ViewModels;

namespace LegacyAVR.UI;

/// <summary>
/// Composition root. Single instance (a second launch just restores the first window),
/// close-to-tray, tray menu wiring, orderly exit (optionally stopping the engine).
/// </summary>
public partial class App : System.Windows.Application
{
    private const string InstanceMutexName = "LegacyAVR.UI.SingleInstance";
    private const string ShowEventName = "LegacyAVR.UI.ShowWindow";

    private Mutex? _instanceMutex;
    private EventWaitHandle? _showEvent;
    private EngineClient? _client;
    private TelemetryReader? _reader;
    private SettingsService? _settings;
    private ProfileStore? _profiles;
    private MainViewModel? _vm;
    private MainWindow? _window;
    private TrayIconService? _tray;

    public static bool IsExiting { get; private set; }

    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // v1.5.0: an unexpected error in a button handler is reported (and logged) instead of
        // closing the dashboard without a word.
        DispatcherUnhandledException += OnDispatcherUnhandledException;

        // v1.5.0: started again by the emergency reset - let the old instance finish exiting first,
        // so its single-instance lock is released.
        EmergencyReset.WaitForPreviousInstance(e.Args);

        _instanceMutex = new Mutex(true, InstanceMutexName, out bool first);
        if (!first)
        {
            // Ask the running instance to show itself, then quit.
            try { EventWaitHandle.OpenExisting(ShowEventName).Set(); } catch (Exception) { }
            Shutdown();
            return;
        }
        _showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName);
        _ = Task.Run(() =>
        {
            while (_showEvent.WaitOne())
            {
                if (IsExiting) return;
                Dispatcher.BeginInvoke(ShowDashboard);
            }
        });

        // Closed drop-downs must not change their selection when the mouse wheel passes over
        // them (it scrolls the page instead). Applies to every ComboBox in the app.
        EventManager.RegisterClassHandler(typeof(ComboBox), UIElement.PreviewMouseWheelEvent,
                                          new MouseWheelEventHandler(OnComboBoxMouseWheel));

        _settings = new SettingsService();
        _settings.Load();
        // v1.4.0: profiles replace presets + device profiles; the first run converts both files.
        _profiles = new ProfileStore(_settings.Folder);
        _profiles.Load(seedDefaults: !_settings.Settings.DefaultPresetsSeeded);
        if (!_settings.Settings.ProfilesMigrated)
        {
            _settings.Settings.LastProfile ??= _settings.Settings.LastPreset;
            _settings.Settings.ProfilesMigrated = true;
            _settings.Settings.DefaultPresetsSeeded = true;
            _settings.Save();
        }
        _client = new EngineClient();
        _reader = new TelemetryReader();
        _vm = new MainViewModel(_client, _reader, _settings, _profiles);
        _vm.OpenEqRequested += (_, ch) => OpenEqDialog(ch);
        _vm.EmergencyRequested += async (_, reopen) => await EmergencyExitAsync(reopen);

        _window = new MainWindow { DataContext = _vm };
        _window.Icon = IconFactory.CreateWindowIcon();

        _tray = new TrayIconService();
        _tray.SetProfiles(_profiles.Profiles, _settings.Settings.LastProfile);
        _tray.ShowRequested += (_, _) => ShowDashboard();
        _tray.MuteAllToggled += (_, m) => _vm.MuteAll = m;
        _tray.ProfileSelected += async (_, p) =>
        {
            ShowDashboard();   // a question may be needed (profile made for another output)
            await _vm.ApplyProfileFromTrayAsync(p);
        };
        _tray.ExitRequested += async (_, _) => await ExitApplicationAsync();
        _vm.ProfilesChanged += (_, _) => _tray.SetProfiles(_profiles.Profiles, _settings.Settings.LastProfile);
        _vm.PropertyChanged += (_, a) =>
        {
            if (a.PropertyName is nameof(MainViewModel.TrayTooltip) or nameof(MainViewModel.MuteAll))
                _tray.UpdateStatus(_vm.TrayTooltip, _vm.MuteAll);
        };

        _window.ClosedToTray += (_, _) =>
        {
            if (_settings.Settings.TrayHintShown) return;
            _tray.ShowBalloon("LegacyAVR is still running",
                "Audio encoding continues in the background. Double-click the tray icon to reopen the dashboard.");
            _settings.Settings.TrayHintShown = true;
            _settings.Save();
        };

        bool startMinimized = e.Args.Any(a => a.Equals("--minimized", StringComparison.OrdinalIgnoreCase));
        if (!startMinimized) _window.Show();

        await _vm.InitialiseAsync();
    }

    private void OnDispatcherUnhandledException(object sender, System.Windows.Threading.DispatcherUnhandledExceptionEventArgs e)
    {
        var path = UiLog.Write(e.Exception);
        e.Handled = true;
        if (IsExiting) return;
        MessageBox.Show("Something went wrong in the dashboard:\n" + e.Exception.Message +
                        (path is null ? string.Empty : "\n\nDetails were written to " + path) +
                        "\n\nLegacyAVR keeps running. If it misbehaves, use Engine Settings > Emergency reset.",
                        "LegacyAVR", MessageBoxButton.OK, MessageBoxImage.Warning);
    }

    private static void OnComboBoxMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (sender is not ComboBox cb || cb.IsDropDownOpen) return;   // open list: normal scrolling
        e.Handled = true;
        // Re-raise on the parent so the surrounding ScrollViewer still scrolls.
        if (VisualTreeHelper.GetParent(cb) is UIElement parent)
        {
            parent.RaiseEvent(new MouseWheelEventArgs(e.MouseDevice, e.Timestamp, e.Delta)
            {
                RoutedEvent = UIElement.MouseWheelEvent,
                Source = cb,
            });
        }
    }

    private EqWindow? _eqWindow;

    /// <summary>v1.4.0: the per-channel EQ dialog, over the main window (one at a time).</summary>
    private void OpenEqDialog(int channel)
    {
        if (_vm is null || _window is null) return;
        if (_eqWindow is { IsLoaded: true })
        {
            _eqWindow.SelectChannel(channel);
            _eqWindow.Activate();
            return;
        }
        _eqWindow = new EqWindow(_vm, channel) { Owner = _window };
        _eqWindow.Closed += (_, _) => _eqWindow = null;
        _eqWindow.Show();
    }

    private void ShowDashboard()
    {
        if (_window is null) return;
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
        _window.Topmost = true;    // reliably bring to front from the tray
        _window.Topmost = false;
        _window.Focus();
    }

    public async Task ExitApplicationAsync()
    {
        if (IsExiting) return;
        IsExiting = true;
        if (_settings?.Settings.StopEngineOnExit == true && _vm?.IsServiceMode == false && _client != null)
            await _client.SendAsync("SHUTDOWN");   // the service ignores this by design
        _vm?.Dispose();
        _tray?.Dispose();
        _reader?.Dispose();
        _client?.Dispose();
        _showEvent?.Set();
        _window?.Close();
        Shutdown();
    }

    /// <summary>
    /// v1.5.0 emergency reset: close the dashboard, force-stop every engine process and, when
    /// <paramref name="reopen"/>, start the dashboard again (it starts a fresh engine). Does not
    /// depend on the engine answering: a polite SHUTDOWN gets one second, then the processes are killed.
    /// </summary>
    private async Task EmergencyExitAsync(bool reopen)
    {
        if (IsExiting) return;
        IsExiting = true;
        if (_vm?.IsServiceMode == false && _client != null)
        {
            try { await Task.WhenAny(_client.SendAsync("SHUTDOWN"), Task.Delay(1000)); }
            catch (Exception) { /* the engine is not answering - it is killed below */ }
        }
        _eqWindow?.Close();
        _vm?.Dispose();
        _reader?.Dispose();
        _client?.Dispose();

        var problems = await Task.Run(() => EmergencyReset.StopEngines());
        string? relaunchError = reopen ? EmergencyReset.RelaunchDashboard() : null;
        var messages = problems.ToList();
        if (relaunchError != null) messages.Add(relaunchError);
        if (messages.Count > 0)
            MessageBox.Show(string.Join("\n\n", messages), "LegacyAVR - emergency reset", MessageBoxButton.OK, MessageBoxImage.Warning);
        _tray?.Dispose();
        _showEvent?.Set();
        _window?.Close();
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _tray?.Dispose();
        _instanceMutex?.Dispose();
        base.OnExit(e);
    }
}
