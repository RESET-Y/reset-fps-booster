using System.Windows;
using System.Windows.Interop;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Applies a native dark title bar and, on Windows 11, the real Mica system backdrop.
/// Falls back silently on older builds where the DWM attribute is not supported.
/// </summary>
public static class WindowBackdrop
{
    public static void ApplyDarkModeAndBackdrop(Window window)
    {
        window.SourceInitialized += (_, _) =>
        {
            var hwnd = new WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero) return;

            var useDark = 1;
            NativeMethods.DwmSetWindowAttribute(hwnd, NativeMethods.DWMWA_USE_IMMERSIVE_DARK_MODE, ref useDark, sizeof(int));

            var backdrop = NativeMethods.DWMSBT_MAINWINDOW;
            var result = NativeMethods.DwmSetWindowAttribute(hwnd, NativeMethods.DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));

            if (result != 0)
            {
                // Not Windows 11 / DWM backdrop unsupported: fall back to blur-behind acrylic.
                TryApplyLegacyAcrylic(hwnd);
            }
        };
    }

    private static void TryApplyLegacyAcrylic(IntPtr hwnd)
    {
        try
        {
            var accent = new NativeMethods.AccentPolicy
            {
                AccentState = NativeMethods.ACCENT_ENABLE_ACRYLICBLURBEHIND,
                GradientColor = unchecked((int)0xCC1A1A1F) // ARGB, subtle dark tint
            };

            var accentSize = System.Runtime.InteropServices.Marshal.SizeOf(accent);
            var accentPtr = System.Runtime.InteropServices.Marshal.AllocHGlobal(accentSize);
            try
            {
                System.Runtime.InteropServices.Marshal.StructureToPtr(accent, accentPtr, false);

                var data = new NativeMethods.WindowCompositionAttributeData
                {
                    Attribute = NativeMethods.WCA_ACCENT_POLICY,
                    SizeOfData = accentSize,
                    Data = accentPtr
                };

                NativeMethods.SetWindowCompositionAttribute(hwnd, ref data);
            }
            finally
            {
                System.Runtime.InteropServices.Marshal.FreeHGlobal(accentPtr);
            }
        }
        catch
        {
            // Purely cosmetic — never let a backdrop failure affect the app.
        }
    }
}
