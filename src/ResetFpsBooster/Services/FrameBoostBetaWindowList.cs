#if RFB_BETA
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace ResetFpsBooster.Services;

/// One entry in the "game window" list. Handle 0 is the automatic choice:
/// the engine's own five-second countdown.
public sealed record GameWindowChoice(long Handle, string Title, string ProcessName)
{
    public string Label => Handle == 0 ? Title : $"{Title}  –  {ProcessName}";
}

/// THE WINDOWS A GAME COULD BE IN.
///
/// Top-level, visible, titled windows that belong to another process. Left
/// out: this app, tool windows (overlays, tray pop-ups), windows owned by
/// another window (dialogs), and windows Windows keeps "cloaked" - suspended
/// Store apps and windows on other virtual desktops, which are listed as
/// visible but cannot be captured.
public static class FrameBoostBetaWindowList
{
    public static List<GameWindowChoice> Read()
    {
        var result = new List<GameWindowChoice>();
        var ownPid = (uint)Environment.ProcessId;
        var names = new Dictionary<uint, string>();

        EnumWindows((hwnd, _) =>
        {
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != IntPtr.Zero) return true;
            if ((GetWindowLongPtr(hwnd, GWL_EXSTYLE).ToInt64() & WS_EX_TOOLWINDOW) != 0) return true;
            if (DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0 && cloaked != 0) return true;

            int length = GetWindowTextLength(hwnd);
            if (length == 0) return true;
            var title = new StringBuilder(length + 1);
            GetWindowText(hwnd, title, title.Capacity);

            GetWindowThreadProcessId(hwnd, out uint pid);
            if (pid == ownPid) return true;
            if (!names.TryGetValue(pid, out var process))
            {
                try { process = Process.GetProcessById((int)pid).ProcessName + ".exe"; }
                catch { process = "?"; }
                names[pid] = process;
            }
            // Windows' own shell surfaces are never a game.
            if (process.Equals("explorer.exe", StringComparison.OrdinalIgnoreCase) && title.ToString() == "Program Manager")
                return true;

            result.Add(new GameWindowChoice(hwnd.ToInt64(), title.ToString(), process));
            return true;
        }, IntPtr.Zero);

        return result.OrderBy(w => w.Title, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    public static bool StillExists(long handle) => handle != 0 && IsWindow(new IntPtr(handle));

    private const int GW_OWNER = 4;
    private const int GWL_EXSTYLE = -20;
    private const long WS_EX_TOOLWINDOW = 0x00000080;
    private const int DWMWA_CLOAKED = 14;

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowTextLength(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);
}
#endif
