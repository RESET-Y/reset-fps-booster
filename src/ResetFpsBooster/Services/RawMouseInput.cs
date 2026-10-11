using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace ResetFpsBooster.Services;

/// <summary>
/// RAW MOUSE INPUT: the steps the mouse itself reports, in counts.
///
/// A game turns the view by those counts. Windows' own pointer is a different
/// thing - it is scaled by "pointer speed" and may be accelerated - so a test
/// that used it would measure a sensitivity nobody plays with. Raw Input is
/// the ordinary Windows interface for exactly this (it is what games and mouse
/// software use). It is received by a window of OUR OWN app and touches no
/// other program: no hook, no injection, nothing read from a game.
/// </summary>
public sealed class RawMouseInput : IDisposable
{
    private const int WM_INPUT = 0x00FF;
    private const uint RID_INPUT = 0x10000003;
    private const uint RIM_TYPEMOUSE = 0;
    private const uint RIDEV_REMOVE = 0x00000001;
    private const uint RIDEV_INPUTSINK = 0x00000100;
    private const ushort MOUSE_MOVE_ABSOLUTE = 0x0001;
    private const ushort RI_MOUSE_LEFT_BUTTON_DOWN = 0x0001;
    private const ushort RI_MOUSE_LEFT_BUTTON_UP = 0x0002;

    [StructLayout(LayoutKind.Sequential)]
    private struct RawInputDevice
    {
        public ushort UsagePage;
        public ushort Usage;
        public uint Flags;
        public IntPtr Target;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RawInputHeader
    {
        public uint Type;
        public uint Size;
        public IntPtr Device;
        public IntPtr WParam;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterRawInputDevices(RawInputDevice[] devices, uint count, uint size);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputData(IntPtr rawInput, uint command, IntPtr data, ref uint size, uint headerSize);

    private HwndSource? _source;
    private IntPtr _hwnd;

    /// <summary>Counts moved since the last event: dx right, dy down. Raised on the UI thread.</summary>
    public event Action<int, int>? Moved;

    /// <summary>The left button went down (true) or up (false).</summary>
    public event Action<bool>? LeftButton;

    public bool IsRunning => _source is not null;

    /// <summary>Starts listening on this window. With <paramref name="background"/> the input
    /// also arrives while another window has the focus (what a game in front needs).</summary>
    public bool Start(Window window, bool background = false)
    {
        Stop();
        _hwnd = new WindowInteropHelper(window).EnsureHandle();
        _source = HwndSource.FromHwnd(_hwnd);
        if (_source is null) return false;
        _source.AddHook(Hook);

        var device = new RawInputDevice
        {
            UsagePage = 0x01,       // generic desktop
            Usage = 0x02,           // mouse
            Flags = background ? RIDEV_INPUTSINK : 0,
            Target = _hwnd
        };
        if (!RegisterRawInputDevices(new[] { device }, 1, (uint)Marshal.SizeOf<RawInputDevice>()))
        {
            _source.RemoveHook(Hook);
            _source = null;
            return false;
        }
        return true;
    }

    public void Stop()
    {
        if (_source is null) return;
        var device = new RawInputDevice { UsagePage = 0x01, Usage = 0x02, Flags = RIDEV_REMOVE, Target = IntPtr.Zero };
        RegisterRawInputDevices(new[] { device }, 1, (uint)Marshal.SizeOf<RawInputDevice>());
        _source.RemoveHook(Hook);
        _source = null;
    }

    private IntPtr Hook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg != WM_INPUT) return IntPtr.Zero;

        var headerSize = (uint)Marshal.SizeOf<RawInputHeader>();
        uint size = 0;
        if (GetRawInputData(lParam, RID_INPUT, IntPtr.Zero, ref size, headerSize) != 0 || size == 0) return IntPtr.Zero;

        var buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            if (GetRawInputData(lParam, RID_INPUT, buffer, ref size, headerSize) != size) return IntPtr.Zero;
            if ((uint)Marshal.ReadInt32(buffer, 0) != RIM_TYPEMOUSE) return IntPtr.Zero;

            // RAWMOUSE follows the header: flags (2), padding (2), button flags (2) and data (2),
            // raw buttons (4), then the signed counts - x at +12, y at +16.
            var at = (int)headerSize;
            var flags = (ushort)Marshal.ReadInt16(buffer, at);
            var buttons = (ushort)Marshal.ReadInt16(buffer, at + 4);
            var dx = Marshal.ReadInt32(buffer, at + 12);
            var dy = Marshal.ReadInt32(buffer, at + 16);

            if ((flags & MOUSE_MOVE_ABSOLUTE) == 0 && (dx != 0 || dy != 0)) Moved?.Invoke(dx, dy);
            if ((buttons & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) LeftButton?.Invoke(true);
            if ((buttons & RI_MOUSE_LEFT_BUTTON_UP) != 0) LeftButton?.Invoke(false);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
        return IntPtr.Zero;
    }

    public void Dispose() => Stop();
}
