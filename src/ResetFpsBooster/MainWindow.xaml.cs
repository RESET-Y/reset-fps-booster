using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Hardcodet.Wpf.TaskbarNotification;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Services;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster;

public partial class MainWindow : Window
{
    private TaskbarIcon? _tray;
    private bool _exiting;

    /// <summary>Set by the app before the window is shown: decides whether
    /// closing the window keeps the app in the tray.</summary>
    public ISettingsService? Settings { get; set; }

    /// <summary>Stops the FrameBoost and Smooth Motion engine. Called whenever the window
    /// goes to the tray, so no full-screen overlay outlives the window that controls it.</summary>
    public Action? StopOverlays { get; set; }

    public MainWindow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        // Windows is shutting down or logging off: never hold that up.
        Application.Current.SessionEnding += (_, _) => _exiting = true;
        Loaded += (_, _) =>
        {
            StartAmbientMotion();
            // the first page (the dashboard) builds itself in as well
            Dispatcher.BeginInvoke(new Action(() => Motion.EnterCards(ContentHost)), DispatcherPriority.ApplicationIdle);
        };
    }

    // Two slow loops that give the window a pulse. Both are plain animations
    // on opacity and a transform; WPF stops drawing them while the window is
    // hidden or minimized.
    private void StartAmbientMotion()
    {
        if (!Motion.Enabled) return;

        // the red horizon breathes
        HorizonGlowRect.BeginAnimation(OpacityProperty, new DoubleAnimation(0.55, 1.0, TimeSpan.FromSeconds(4.5))
        {
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
        });

        // a light runs along the line under the title bar, rests, and runs again
        var run = new DoubleAnimationUsingKeyFrames { RepeatBehavior = RepeatBehavior.Forever };
        var end = Math.Max(ActualWidth, 1280) + 320;
        run.KeyFrames.Add(new LinearDoubleKeyFrame(-300, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        run.KeyFrames.Add(new EasingDoubleKeyFrame(end, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(2.2)),
            new CubicEase { EasingMode = EasingMode.EaseInOut }));
        run.KeyFrames.Add(new DiscreteDoubleKeyFrame(end, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(6.5))));
        LineShimmerShift.BeginAnimation(TranslateTransform.XProperty, run);
    }

    // Our own title bar costs the Windows 11 frame its rounded corners; ask
    // DWM for them back (small radius) and for a 1px border in the accent.
    // Both are ignored on Windows 10, which keeps square corners.
    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    private const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    private const int DWMWA_BORDER_COLOR = 34;
    private const int DWMWCP_ROUNDSMALL = 3;

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        var hwnd = new WindowInteropHelper(this).Handle;
        var corner = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref corner, sizeof(int));
        if (TryFindResource("Color.Accent") is Color accent)
        {
            var colorRef = accent.R | (accent.G << 8) | (accent.B << 16);   // COLORREF is 0x00BBGGRR
            DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, ref colorRef, sizeof(int));
        }
    }

    private void OnMinimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void OnMaximize(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void OnClose(object sender, RoutedEventArgs e) => Close();

    public const string DiscordInvite = "https://discord.gg/a2Uz4vdCh9";

    private void OnJoinDiscord(object sender, RoutedEventArgs e)
    {
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(DiscordInvite) { UseShellExecute = true }); }
        catch { /* no browser registered - the link is in the tooltip */ }
    }

    private void OnStateChanged(object? sender, EventArgs e) =>
        RootPanel.Margin = WindowState == WindowState.Maximized ? new Thickness(7) : new Thickness(0);

    // Closing (X, Alt+F4, taskbar "Close") hides the window into the tray
    // instead; the app keeps running, FrameBoost and the crosshair with it.
    // Only the tray menu's "Beenden" really closes it.
    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_exiting && Settings?.Current.CloseToTray == true)
        {
            e.Cancel = true;
            StopOverlays?.Invoke();
            SendToTray();
            return;
        }
        base.OnClosing(e);
    }

    // Hidden, the window has no taskbar button; the tray icon brings it back.
    private void SendToTray()
    {
        _tray ??= CreateTrayIcon();
        _tray.Visibility = Visibility.Visible;
        Hide();
        _tray.ShowBalloonTip("RESET FPS BOOSTER", "Läuft im Tray weiter. FrameBoost und Smooth Motion sind aus. Rechtsklick → Beenden.", BalloonIcon.Info);
    }

    private void RestoreFromTray()
    {
        if (_tray is not null) _tray.Visibility = Visibility.Collapsed;
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    private TaskbarIcon CreateTrayIcon()
    {
        var open = new System.Windows.Controls.MenuItem { Header = "Öffnen" };
        open.Click += (_, _) => RestoreFromTray();
        var exit = new System.Windows.Controls.MenuItem { Header = "Beenden" };
        exit.Click += (_, _) => { _exiting = true; Close(); };

        var tray = new TaskbarIcon
        {
            IconSource = new BitmapImage(new Uri("pack://application:,,,/Assets/logo_square.png")),
            ToolTipText = "RESET FPS BOOSTER",
            Visibility = Visibility.Collapsed,
            ContextMenu = new System.Windows.Controls.ContextMenu { Items = { open, exit } },
        };
        tray.TrayMouseDoubleClick += (_, _) => RestoreFromTray();
        return tray;
    }

    protected override void OnClosed(EventArgs e)
    {
        _tray?.Dispose();   // otherwise the icon lingers in the tray after exit
        base.OnClosed(e);
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (e.OldValue is INotifyPropertyChanged oldVm)
            oldVm.PropertyChanged -= OnMainViewModelPropertyChanged;

        if (e.NewValue is INotifyPropertyChanged newVm)
            newVm.PropertyChanged += OnMainViewModelPropertyChanged;
    }

    private void OnMainViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName != nameof(MainViewModel.CurrentViewModel)) return;

        // once the new page has its layout, its cards rise in one after the other
        Dispatcher.BeginInvoke(new Action(() => Motion.EnterCards(ContentHost)), DispatcherPriority.Loaded);
        // (No fade or slide of the whole page: a page that goes invisible for a moment
        // reads as a flash. The cards above move; the page itself stays put.)
        ContentHost.BeginAnimation(OpacityProperty, null);
        ContentHost.Opacity = 1;
        ContentHost.RenderTransform = Transform.Identity;
    }
}
