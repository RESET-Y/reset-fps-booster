using System.Runtime.InteropServices;

namespace ResetFpsBooster.Core.Utilities;

internal static class NativeMethods
{
    // --- DWM window backdrop (Mica / Acrylic) ---------------------------------
    [DllImport("dwmapi.dll", PreserveSig = true)]
    internal static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);

    internal const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    internal const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    internal const int DWMSBT_MAINWINDOW = 2; // Mica
    internal const int DWMSBT_TRANSIENTWINDOW = 3; // Acrylic

    // --- Fallback acrylic blur for older Windows 10 builds --------------------
    [StructLayout(LayoutKind.Sequential)]
    internal struct AccentPolicy
    {
        public int AccentState;
        public int AccentFlags;
        public int GradientColor;
        public int AnimationId;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct WindowCompositionAttributeData
    {
        public int Attribute;
        public IntPtr Data;
        public int SizeOfData;
    }

    internal const int WCA_ACCENT_POLICY = 19;
    internal const int ACCENT_ENABLE_ACRYLICBLURBEHIND = 4;

    [DllImport("user32.dll")]
    internal static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WindowCompositionAttributeData data);

    // --- System visual effects (animations etc.) ------------------------------
    [DllImport("user32.dll", SetLastError = true)]
    internal static extern bool SystemParametersInfo(uint uiAction, uint uiParam, IntPtr pvParam, uint fWinIni);

    internal const uint SPI_SETMENUANIMATION = 0x1003;
    internal const uint SPI_SETCOMBOBOXANIMATION = 0x1005;
    internal const uint SPI_SETLISTBOXSMOOTHSCROLLING = 0x1007;
    internal const uint SPI_SETTOOLTIPANIMATION = 0x1041;
    internal const uint SPI_SETCURSORSHADOW = 0x101B;
    internal const uint SPI_SETUIEFFECTS = 0x103E;
    internal const uint SPIF_SENDCHANGE = 0x0002;

    // --- Process working set trimming ------------------------------------------
    [DllImport("psapi.dll", SetLastError = true)]
    internal static extern bool EmptyWorkingSet(IntPtr hProcess);

    // --- Foreground window lookup (used to never trim/hitch the active game) --
    [DllImport("user32.dll")]
    internal static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", SetLastError = true)]
    internal static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    // --- Standby list purge (the same mechanism Sysinternals RAMMap and the
    // well-known "Empty Standby List" tool use to release reclaimable file-cache
    // memory back to the OS) ----------------------------------------------------
    [DllImport("ntdll.dll")]
    internal static extern int NtSetSystemInformation(int systemInformationClass, ref int systemInformation, int systemInformationLength);

    internal const int SystemMemoryListInformation = 80;
    internal const int MemoryPurgeStandbyList = 4;

    [DllImport("advapi32.dll", SetLastError = true)]
    internal static extern bool OpenProcessToken(IntPtr processHandle, uint desiredAccess, out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    internal static extern bool LookupPrivilegeValue(string? lpSystemName, string lpName, out LUID lpLuid);

    [DllImport("advapi32.dll", SetLastError = true)]
    internal static extern bool AdjustTokenPrivileges(
        IntPtr tokenHandle, bool disableAllPrivileges,
        ref TOKEN_PRIVILEGES newState, uint bufferLength,
        IntPtr previousState, IntPtr returnLength);

    internal const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
    internal const uint TOKEN_QUERY = 0x0008;
    internal const uint SE_PRIVILEGE_ENABLED = 0x0002;

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool CloseHandle(IntPtr hObject);

    internal static void CloseHandleSafe(IntPtr handle)
    {
        if (handle != IntPtr.Zero) CloseHandle(handle);
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct LUID
    {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct TOKEN_PRIVILEGES
    {
        public uint PrivilegeCount;
        public LUID Luid;
        public uint Attributes;
    }
}
