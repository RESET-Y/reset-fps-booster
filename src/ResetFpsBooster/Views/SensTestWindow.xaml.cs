using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using ResetFpsBooster.Core.Sensitivity;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.Views;

/// <summary>
/// THE AIM TEST WINDOW: full screen on the primary monitor, with its own small
/// "shooting range". It reads the mouse through Raw Input - the counts the
/// mouse itself reports, as a game would see them - turns a camera by them and
/// draws what the <see cref="AimSession"/> says. It touches no game and no
/// other window; the pointer is only held inside this window while the test
/// runs, as any game does, and released when it ends.
///
/// Esc, or any other window taking the focus, ends the test unfinished.
///
/// WPF sizes a borderless window by its own rules after it is created, so the
/// window is put on the monitor again once it is shown and checked every
/// frame; the pointer is held to the window's real rectangle, not to the one
/// that was asked for. A short log of what happened is written next to the
/// app's other logs (sens-test.log).
/// </summary>
public partial class SensTestWindow : Window
{
    private readonly RawMouseInput _mouse = new();
    private readonly Stopwatch _clock = new();
    private readonly MonitorInfo? _monitor;
    private double _last;
    private double _lastLog;
    private bool _armed;
    private bool _closing;
    private DateTime? _doneAt;
    private RECT _clip;
    private IntPtr _hwnd;

    public AimSession Session { get; }

    /// <summary>True when every round was played to the end.</summary>
    public bool Completed => Session.Phase == AimPhase.Done;

    /// <param name="fov">The game's horizontal field of view in degrees.</param>
    /// <param name="resWidth">The game's resolution; its picture is drawn at that shape.</param>
    /// <param name="stretch">Fill the whole monitor even when the shape differs.</param>
    public SensTestWindow(double dpi, double centerCm360, IReadOnlyList<double> multipliers,
                          double fov, int resWidth, int resHeight, bool stretch)
    {
        InitializeComponent();
        _monitor = MonitorList.Read().FirstOrDefault();
        Session = new AimSession(dpi, centerCm360, multipliers, Environment.TickCount, fov, (double)resWidth / resHeight);
        Canvas.Session = Session;
        Canvas.ViewWidth = resWidth;
        Canvas.ViewHeight = resHeight;
        Canvas.Stretch = stretch;

        SourceInitialized += (_, _) => { _hwnd = new WindowInteropHelper(this).Handle; Fit(); };
        Loaded += OnLoaded;
        ContentRendered += (_, _) => { Fit(); Log("content rendered"); };
        Activated += (_, _) => { _armed = true; Log("activated"); };
        Deactivated += (_, _) =>
        {
            if (!_armed) return;
            Log("deactivated - " + DescribeForeground());
            End();
        };
        KeyDown += (_, e) => { if (e.Key == Key.Escape) { Log("Esc"); End(); } };
        Closed += OnClosed;
    }

    /// <summary>Puts the window exactly on the primary monitor and holds the pointer inside it.</summary>
    private void Fit()
    {
        if (_monitor is null || _hwnd == IntPtr.Zero) return;
        SetWindowPos(_hwnd, new IntPtr(-1), _monitor.Left, _monitor.Top, _monitor.Width, _monitor.Height,
                     0x0010 /* NOACTIVATE */ | 0x0040 /* SHOWWINDOW */);
        if (GetWindowRect(_hwnd, out var r)) _clip = r;
        if (_clip.Right > _clip.Left) ClipCursor(ref _clip);
    }

    private bool IsFitted() =>
        _monitor is not null && GetWindowRect(_hwnd, out var r) &&
        r.Left == _monitor.Left && r.Top == _monitor.Top &&
        r.Right - r.Left == _monitor.Width && r.Bottom - r.Top == _monitor.Height;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        Fit();
        Activate();
        Focus();
        Log($"loaded; monitor {(_monitor is null ? "none" : $"{_monitor.Left},{_monitor.Top} {_monitor.Width}x{_monitor.Height}")}; " +
            $"window {Describe(_hwnd)}; clip {_clip.Left},{_clip.Top},{_clip.Right},{_clip.Bottom}; dpi scale {VisualTreeHelper.GetDpi(this).DpiScaleX}");

        _mouse.Moved += (dx, dy) => Session.Mouse(dx, dy);
        _mouse.LeftButton += down => { if (down) Session.Click(); };
        if (!_mouse.Start(this))
        {
            // No raw mouse input available: nothing sensible to measure.
            Log("raw input could not be started");
            Session.Abort();
            End();
            return;
        }

        _clock.Start();
        CompositionTarget.Rendering += OnRendering;
    }

    private void OnRendering(object? sender, EventArgs e)
    {
        var now = _clock.Elapsed.TotalSeconds;
        // A stall (the window was dragged, the machine hiccuped) must not count as aiming time.
        var dt = Math.Min(now - _last, 0.05);
        _last = now;

        // Windows can lift the pointer clip (another program, a focus change), and WPF may resize
        // the window after it is shown: put both back, every frame.
        if (!IsFitted()) { Log("window was not on the monitor: " + Describe(_hwnd)); Fit(); }
        else ClipCursor(ref _clip);

        Session.Update(dt);
        Canvas.InvalidateVisual();

        if (now - _lastLog >= 1.0)
        {
            _lastLog = now;
            GetClipCursor(out var c);
            GetCursorPos(out var p);
            Log($"t={now:0} phase={Session.Phase} round={Session.RoundIndex} cursor {p.X},{p.Y} clip {c.Left},{c.Top},{c.Right},{c.Bottom} foreground={(GetForegroundWindow() == _hwnd)}");
        }

        if (Session.Phase == AimPhase.Done)
        {
            _doneAt ??= DateTime.UtcNow;
            if ((DateTime.UtcNow - _doneAt.Value).TotalSeconds > 2.0) End();
        }
        else if (Session.Phase == AimPhase.Aborted)
        {
            End();
        }
    }

    private void End()
    {
        if (_closing) return;
        _closing = true;
        if (Session.Phase != AimPhase.Done) Session.Abort();
        Log("end, completed=" + Completed);
        try { DialogResult = Completed; } catch (InvalidOperationException) { }
        Close();
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        CompositionTarget.Rendering -= OnRendering;
        _mouse.Dispose();
        ClipCursor(IntPtr.Zero);
    }

    // ------------------------------------------------------------------ log

    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ResetFpsBooster", "Logs", "sens-test.log");

    private void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
            File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}");
        }
        catch { /* a log must never break the test */ }
    }

    private static string Describe(IntPtr hwnd) =>
        GetWindowRect(hwnd, out var r) ? $"{r.Left},{r.Top} {r.Right - r.Left}x{r.Bottom - r.Top}" : "?";

    // Who took the focus: the program and the window title, so a stray pop-up can be told from a click.
    private static string DescribeForeground()
    {
        try
        {
            var fg = GetForegroundWindow();
            if (fg == IntPtr.Zero) return "no foreground window";
            var title = new StringBuilder(120);
            GetWindowText(fg, title, title.Capacity);
            GetWindowThreadProcessId(fg, out var pid);
            string name;
            try { name = Process.GetProcessById((int)pid).ProcessName; } catch { name = "pid " + pid; }
            return $"foreground: {name} \"{title}\"";
        }
        catch { return "foreground unknown"; }
    }

    // ---------------------------------------------------------------- native

    [StructLayout(LayoutKind.Sequential)] private struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct POINT { public int X, Y; }

    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] private static extern bool ClipCursor(ref RECT rect);
    [DllImport("user32.dll")] private static extern bool ClipCursor(IntPtr rect);
    [DllImport("user32.dll")] private static extern bool GetClipCursor(out RECT rect);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
}
