using System.Runtime.InteropServices;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Whether NVIDIA's driver API (nvapi64.dll) can be loaded on this PC.
///
/// Ask this BEFORE creating anything from NvAPIWrapper that has a finalizer. A
/// DriverSettingsSession whose constructor throws (no NVIDIA driver) is still finalized, and
/// its finalizer calls DestroySession into the missing DLL on the GC thread. No try/catch can
/// stop that: the app dies seconds later on every PC with AMD or Intel graphics.
/// </summary>
public static class NvidiaDriver
{
    // The handle is deliberately kept: NvAPIWrapper loads the same DLL right after, and
    // unloading it in between gains nothing.
    private static readonly Lazy<bool> Available = new(() => NativeLibrary.TryLoad("nvapi64", out _));

    public static bool IsAvailable => Available.Value;
}
