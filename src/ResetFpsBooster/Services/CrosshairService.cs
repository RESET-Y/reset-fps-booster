using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface ICrosshairService
{
    bool IsShown { get; }
    event EventHandler? StateChanged;

    /// <summary>Shows the crosshair, or redraws it if it is already up. Returns
    /// false when there is nothing to show (an image that cannot be read).</summary>
    bool Show(CrosshairStyle style);
    void Hide();

    /// <summary>At start-up: back on if it was on last time and premium holds.</summary>
    Task RestoreAsync(ISettingsService settings, IAuthService auth);
}

/// <summary>
/// THE CROSSHAIR OVERLAY: a small borderless window in the middle of the
/// chosen monitor, always on top, that every click goes straight through.
///
/// It is a window of this app and nothing else - no hook, no injection, the
/// game is never touched or read. That is also why it cannot appear over a
/// game in EXCLUSIVE fullscreen, which owns the display; borderless and
/// windowed modes work.
///
/// Kept out of screen capture (WDA_EXCLUDEFROMCAPTURE). Otherwise Smooth
/// Motion's whole-screen capture would pick it up and draw a blurred copy of
/// it under the sharp one. The price: recordings do not show it.
/// </summary>
public sealed class CrosshairService : ICrosshairService
{
    private Window? _window;
    private Image? _image;
    private BitmapSource? _bitmap;
    private MonitorInfo? _monitor;
    private DispatcherTimer? _topmost;

    public bool IsShown => _window is { IsVisible: true };
    public event EventHandler? StateChanged;

    public bool Show(CrosshairStyle style)
    {
        var bitmap = CrosshairRenderer.Render(style);
        if (bitmap is null) return false;

        var monitors = MonitorList.Read();
        if (monitors.Count == 0) return false;
        _monitor = monitors[Math.Clamp(style.Screen, 0, monitors.Count - 1)];
        _bitmap = bitmap;

        if (_window is null) Create();
        _image!.Source = _bitmap;
        _image.Opacity = Math.Clamp(style.Opacity, 10, 100) / 100.0;

        if (!_window!.IsVisible) _window.Show();
        Place();
        _topmost!.Start();
        StateChanged?.Invoke(this, EventArgs.Empty);
        return true;
    }

    public void Hide()
    {
        _topmost?.Stop();
        if (_window is not null)
        {
            _window.Close();
            _window = null;
            _image = null;
        }
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    public async Task RestoreAsync(ISettingsService settings, IAuthService auth)
    {
        if (!settings.Current.CrosshairEnabled) return;
        try
        {
            if (await auth.IsPremiumAsync())
                Application.Current.Dispatcher.Invoke(() => Show(settings.Current.Crosshair));
        }
        catch { /* stays off; the page can turn it on */ }
    }

    private void Create()
    {
        _image = new Image { Stretch = Stretch.None, SnapsToDevicePixels = true };
        RenderOptions.SetBitmapScalingMode(_image, BitmapScalingMode.NearestNeighbor);

        _window = new Window
        {
            WindowStyle = WindowStyle.None,
            AllowsTransparency = true,
            Background = Brushes.Transparent,
            ResizeMode = ResizeMode.NoResize,
            ShowInTaskbar = false,
            ShowActivated = false,
            Topmost = true,
            Focusable = false,
            IsHitTestVisible = false,
            UseLayoutRounding = true,
            WindowStartupLocation = WindowStartupLocation.Manual,
            Left = -32000, Top = -32000, Width = 16, Height = 16,
            Title = "RESET Crosshair",
            Content = _image,
        };

        _window.SourceInitialized += (_, _) =>
        {
            var hwnd = new WindowInteropHelper(_window).Handle;
            var ex = GetWindowLong(hwnd, GWL_EXSTYLE);
            SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
        };

        // The window can move to a monitor with a different scale; the image
        // has to be re-fitted so it stays one bitmap pixel per screen pixel.
        _window.DpiChanged += (_, _) => Place();

        // Games and other overlays (FrameBoost's own included) take the top
        // spot for themselves; take it back once a second.
        _topmost = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _topmost.Tick += (_, _) =>
        {
            if (_window is null) return;
            var hwnd = new WindowInteropHelper(_window).Handle;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        };
    }

    // Everything in physical pixels: the window is exactly as big as the
    // bitmap, and the bitmap's centre pixel lands on the monitor's centre pixel.
    private void Place()
    {
        if (_window is null || _image is null || _bitmap is null || _monitor is null) return;

        int size = _bitmap.PixelWidth;
        int cx = _monitor.Left + _monitor.Width / 2;
        int cy = _monitor.Top + _monitor.Height / 2;
        var hwnd = new WindowInteropHelper(_window).Handle;
        SetWindowPos(hwnd, HWND_TOPMOST, cx - size / 2, cy - _bitmap.PixelHeight / 2, size, _bitmap.PixelHeight,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);

        // WPF lays out in 1/96 inch; undo the monitor's scale so the 96-DPI
        // bitmap is drawn 1:1.
        var dpi = VisualTreeHelper.GetDpi(_window);
        _image.LayoutTransform = new ScaleTransform(1 / dpi.DpiScaleX, 1 / dpi.DpiScaleY);
        _window.Width = size / dpi.DpiScaleX;
        _window.Height = _bitmap.PixelHeight / dpi.DpiScaleY;
        SetWindowPos(hwnd, HWND_TOPMOST, cx - size / 2, cy - _bitmap.PixelHeight / 2, size, _bitmap.PixelHeight,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TRANSPARENT = 0x20;
    private const int WS_EX_TOOLWINDOW = 0x80;
    private const int WS_EX_LAYERED = 0x80000;
    private const int WS_EX_NOACTIVATE = 0x08000000;
    private const uint WDA_EXCLUDEFROMCAPTURE = 0x11;
    private static readonly IntPtr HWND_TOPMOST = new(-1);
    private const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10, SWP_NOOWNERZORDER = 0x200;

    [DllImport("user32.dll")] private static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")] private static extern int SetWindowLong(IntPtr hwnd, int index, int value);
    [DllImport("user32.dll")] private static extern bool SetWindowDisplayAffinity(IntPtr hwnd, uint affinity);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
