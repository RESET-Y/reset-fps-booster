using System.Runtime.InteropServices;
using NvAPIWrapper.Display;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Picture;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public enum PictureResult { Ok, DriverMissing, WindowsRefused, GammaUnavailable, Failed }

public interface IPictureService
{
    /// <summary>Saturation is set through the NVIDIA driver; false without an NVIDIA display.</summary>
    bool SaturationAvailable { get; }

    /// <summary>Brightness and contrast use Windows' gamma ramp; false where Windows does not let the app read or set it.</summary>
    bool GammaAvailable { get; }

    /// <summary>The app has changed the screens and has not put them back yet.</summary>
    bool IsApplied { get; }

    PictureResult Apply(PicturePreset picture);

    /// <summary>Puts back what the screens had before the app changed them.</summary>
    void Restore();
}

/// <summary>
/// THE SCREEN'S PICTURE. Saturation goes through the NVIDIA driver (Digital Vibrance, per display);
/// brightness and contrast through Windows' gamma ramp, the same mechanism the Windows colour
/// calibration uses. Before the first change, the values the screens have are written to the
/// settings file, so Restore puts them back even after a crash: the next start restores them.
/// </summary>
public sealed class PictureService : IPictureService
{
    private readonly ISettingsService _settings;
    private readonly Lazy<bool> _saturation;
    private readonly Lazy<bool> _gamma;

    public PictureService(ISettingsService settings)
    {
        _settings = settings;
        _saturation = new Lazy<bool>(() => DisplaysSafely().Count > 0);
        _gamma = new Lazy<bool>(() => ReadGammaRampOrNull() is not null);
    }

    public bool SaturationAvailable => _saturation.Value;

    public bool GammaAvailable => _gamma.Value;

    public bool IsApplied => _settings.Current.PictureOriginal is not null;

    public PictureResult Apply(PicturePreset picture)
    {
        if (PictureMath.IsNeutral(picture.Saturation, picture.Brightness, picture.Contrast))
        {
            Restore();
            return PictureResult.Ok;
        }

        try
        {
            if (_settings.Current.PictureOriginal is null) Capture();

            var result = PictureResult.Ok;
            if (SaturationAvailable && !SetSaturation(picture.Saturation)) result = PictureResult.DriverMissing;

            // Only touched when the player asks for it, and only where Windows allows the ramp to be read.
            if (picture.Brightness != 0 || picture.Contrast != 0)
            {
                if (!GammaAvailable) result = PictureResult.GammaUnavailable;
                else if (!SetGammaRamp(PictureMath.BuildRamp(picture.Brightness, picture.Contrast))) result = PictureResult.WindowsRefused;
            }
            return result;
        }
        catch
        {
            return PictureResult.Failed;
        }
    }

    public void Restore()
    {
        var original = _settings.Current.PictureOriginal;
        if (original is null) return;

        try
        {
            if (SaturationAvailable)
            {
                foreach (var display in DisplaysSafely())
                {
                    if (original.Displays.FirstOrDefault(d => d.Name == display.Name) is { } saved)
                        display.DigitalVibranceControl.CurrentLevel = saved.Level;
                }
            }

            if (original.GammaRampBase64 is { } encoded)
            {
                var bytes = Convert.FromBase64String(encoded);
                if (bytes.Length == 768 * 2) SetGammaRamp(FromBytes(bytes));
            }
        }
        catch
        {
            // Keep the record: the watcher tries again in a few seconds, and the next start does too.
            return;
        }

        _settings.Current.PictureOriginal = null;
        _settings.Save();
    }

    // The values before the first change, written to disk before anything changes.
    private void Capture()
    {
        var original = new PictureOriginals();
        foreach (var display in DisplaysSafely())
        {
            original.Displays.Add(new PictureDisplay
            {
                Name = display.Name,
                Level = display.DigitalVibranceControl.CurrentLevel
            });
        }
        // Recorded only when it could be read: a ramp that was not read must never be "put back".
        var ramp = ReadGammaRampOrNull();
        original.GammaRampBase64 = ramp is null ? null : Convert.ToBase64String(ToBytes(ramp));

        _settings.Current.PictureOriginal = original;
        _settings.Save();
    }

    private bool SetSaturation(int saturation)
    {
        var any = false;
        foreach (var display in DisplaysSafely())
        {
            var dvc = display.DigitalVibranceControl;
            dvc.CurrentLevel = PictureMath.VibranceLevel(saturation, dvc.MinimumLevel, dvc.DefaultLevel, dvc.MaximumLevel);
            any = true;
        }
        return any;
    }

    // Only asks the driver when it is installed: the library's objects have finalizers that call into
    // nvapi64.dll, and without the driver that crashes the app on the GC thread (see NvidiaDriver).
    private static List<Display> DisplaysSafely()
    {
        if (!NvidiaDriver.IsAvailable) return new List<Display>();
        try
        {
            return Display.GetDisplays().ToList();
        }
        catch
        {
            return new List<Display>();
        }
    }

    // ----------------------------------------------------- Windows gamma ramp

    // The current ramp, or null when Windows does not return a plausible one (on some PCs the call
    // simply fails, and an all-zero result must never be mistaken for a real ramp).
    private static ushort[]? ReadGammaRampOrNull()
    {
        var ramp = new ushort[768];
        var dc = GetDC(IntPtr.Zero);
        bool ok;
        try
        {
            ok = GetDeviceGammaRamp(dc, ramp);
        }
        finally
        {
            ReleaseDC(IntPtr.Zero, dc);
        }
        return ok && ramp[0] < 5000 && ramp[255] > 60000 ? ramp : null;
    }

    private static bool SetGammaRamp(ushort[] ramp)
    {
        var dc = GetDC(IntPtr.Zero);
        try
        {
            return SetDeviceGammaRamp(dc, ramp);
        }
        finally
        {
            ReleaseDC(IntPtr.Zero, dc);
        }
    }

    private static byte[] ToBytes(ushort[] ramp)
    {
        var bytes = new byte[ramp.Length * 2];
        Buffer.BlockCopy(ramp, 0, bytes, 0, bytes.Length);
        return bytes;
    }

    private static ushort[] FromBytes(byte[] bytes)
    {
        var ramp = new ushort[bytes.Length / 2];
        Buffer.BlockCopy(bytes, 0, ramp, 0, bytes.Length);
        return ramp;
    }

    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
    [DllImport("gdi32.dll")] private static extern bool GetDeviceGammaRamp(IntPtr hdc, [Out] ushort[] ramp);
    [DllImport("gdi32.dll")] private static extern bool SetDeviceGammaRamp(IntPtr hdc, [In] ushort[] ramp);
}
