#if RFB_BETA
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

namespace ResetFpsBooster.Services;

/// One thing Smooth Motion can cover: a whole monitor, or one game window.
/// Index is what the engine gets as "screen n": the primary is 0, the rest
/// follow in Windows' order - the engine sorts them the same way
/// (MonitorByIndex in main.cpp). Window is 0 for a monitor, otherwise the
/// game window's handle, captured on its own like FrameBoost does.
public sealed record SmoothScreen(int Index, string Label, long Window = 0)
{
    public bool IsWindow => Window != 0;
}

public static class SmoothMotionScreens
{
    public static List<SmoothScreen> Read(Func<int, bool, int, int, string> label)
    {
        var monitors = new List<(bool Primary, int W, int H)>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (h, _, _, _) =>
        {
            var mi = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
            if (GetMonitorInfo(h, ref mi))
                monitors.Add(((mi.dwFlags & 1) != 0, mi.rcMonitor.Right - mi.rcMonitor.Left, mi.rcMonitor.Bottom - mi.rcMonitor.Top));
            return true;
        }, IntPtr.Zero);

        // Primary first, others in enumeration order - a stable sort, like the engine's.
        var ordered = monitors.Where(m => m.Primary).Concat(monitors.Where(m => !m.Primary)).ToList();
        return ordered.Select((m, i) => new SmoothScreen(i, label(i + 1, m.Primary, m.W, m.H))).ToList();
    }

    /// The slider's value for the running engine: a multiplier on the
    /// automatic strength, written where the engine reads it twice a second.
    /// 0..100 maps to 0.75x..6x with 50 = 3x.
    public static void WriteStrength(int slider)
    {
        var multiplier = Multiplier(slider);
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ResetFpsBooster");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "smooth_strength.txt"), multiplier.ToString("0.###", CultureInfo.InvariantCulture));
        }
        catch { /* the engine keeps the last value it read */ }
    }

    // 50 = 3x: what used to be the top of the slider, which Lukas judged the
    // right look in Apex (2026-09-30). Below it halves every 25 steps down to
    // 0.75x; above it doubles once more to 6x. The engine's own limits (1.5
    // frame intervals, 96 px) still bound the streak at the top.
    public static double Multiplier(int slider)
    {
        var s = Math.Clamp(slider, 0, 100) - 50;
        return 3.0 * Math.Pow(2.0, s < 0 ? s / 25.0 : s / 50.0);
    }

    private delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }

    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
}
#endif
