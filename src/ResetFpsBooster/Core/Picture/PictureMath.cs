namespace ResetFpsBooster.Core.Picture;

/// <summary>
/// THE PICTURE SETTINGS AS NUMBERS. Every pixel gets the same rule: there is no look at the content,
/// no place where anything is found in the image. That is the line the product keeps to.
/// </summary>
public static class PictureMath
{
    // Brightness moves the curve by at most 15 % of the range, contrast scales it by at most 25 % either
    // way. Kept small on purpose: Windows refuses gamma ramps that are too steep.
    public const double MaxBrightnessShift = 0.15;
    public const double MaxContrastChange = 0.25;

    public static bool IsNeutral(int saturation, int brightness, int contrast) =>
        saturation == 0 && brightness == 0 && contrast == 0;

    /// <summary>
    /// The gamma ramp for brightness and contrast: 256 values per colour channel, red then green then blue,
    /// each 0..65535. Neutral values give Windows' own straight ramp (i x 257).
    /// </summary>
    public static ushort[] BuildRamp(int brightness, int contrast)
    {
        var shift = Math.Clamp(brightness, -100, 100) / 100.0 * MaxBrightnessShift;
        var scale = 1.0 + Math.Clamp(contrast, -100, 100) / 100.0 * MaxContrastChange;

        var ramp = new ushort[768];
        for (var i = 0; i < 256; i++)
        {
            var x = i / 255.0;
            // contrast around mid grey first, then brightness
            var y = Math.Clamp(0.5 + (x - 0.5) * scale + shift, 0.0, 1.0);
            var value = (ushort)Math.Round(y * 65535.0);
            ramp[i] = value;
            ramp[256 + i] = value;
            ramp[512 + i] = value;
        }
        return ramp;
    }

    /// <summary>
    /// The driver's Digital Vibrance level for a saturation of -100..+100. 0 is the driver's own default,
    /// +100 its maximum, -100 its minimum; the levels in between are linear on each side.
    /// </summary>
    public static int VibranceLevel(int saturation, int minimum, int defaultLevel, int maximum)
    {
        var s = Math.Clamp(saturation, -100, 100) / 100.0;
        var level = s >= 0
            ? defaultLevel + (maximum - defaultLevel) * s
            : defaultLevel + (defaultLevel - minimum) * s;
        return (int)Math.Round(Math.Clamp(level, Math.Min(minimum, maximum), Math.Max(minimum, maximum)));
    }
}
