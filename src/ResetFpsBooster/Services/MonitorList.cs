using System.Runtime.InteropServices;

namespace ResetFpsBooster.Services;

/// <summary>One monitor, in physical pixels on the virtual desktop.</summary>
public sealed record MonitorInfo(bool Primary, int Left, int Top, int Width, int Height);

/// <summary>
/// The monitors, primary first, the rest in Windows' enumeration order. The
/// FrameBoost engine sorts them the same way (MonitorByIndex in main.cpp), so
/// "screen 1" means the same monitor to the app and to the engine.
/// </summary>
public static class MonitorList
{
    public static List<MonitorInfo> Read()
    {
        var monitors = new List<MonitorInfo>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (h, _, _, _) =>
        {
            var mi = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
            if (GetMonitorInfo(h, ref mi))
                monitors.Add(new MonitorInfo((mi.dwFlags & 1) != 0, mi.rcMonitor.Left, mi.rcMonitor.Top,
                                             mi.rcMonitor.Right - mi.rcMonitor.Left, mi.rcMonitor.Bottom - mi.rcMonitor.Top));
            return true;
        }, IntPtr.Zero);

        // A stable sort: primary first, the others keep their order.
        return monitors.Where(m => m.Primary).Concat(monitors.Where(m => !m.Primary)).ToList();
    }

    private delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }

    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
}
