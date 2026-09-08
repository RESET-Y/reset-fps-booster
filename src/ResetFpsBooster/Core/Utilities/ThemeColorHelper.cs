using System.Windows;
using System.Windows.Media;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Overrides the app's accent-color resources with a user-chosen color. Must run before any
/// Window is constructed — WPF resolves StaticResource lookups once, at the moment each XAML
/// element loads, so mutating these dictionary entries only affects windows created afterward.
/// That means an accent-color change takes effect on the next app start, not live — a fair
/// trade-off for not having to convert every StaticResource in the app to DynamicResource.
/// </summary>
public static class ThemeColorHelper
{
    public const string DefaultAccentHex = "#E8121F";

    public static readonly (string Name, string Hex)[] Presets =
    {
        ("RFB Red", "#E8121F"),
        ("Electric Blue", "#1268E8"),
        ("Emerald", "#14B872"),
        ("Violet", "#7B2FE0"),
        ("Amber", "#E88A12"),
        ("Ice Cyan", "#12C4E8"),
    };

    public static void ApplyAccentColor(string? hex)
    {
        if (string.IsNullOrWhiteSpace(hex)) return;

        Color accent;
        try
        {
            accent = (Color)ColorConverter.ConvertFromString(hex);
        }
        catch
        {
            return;
        }

        var bright = Blend(accent, Colors.White, 0.22);
        var deep = Blend(accent, Colors.Black, 0.45);
        var muted = Blend(accent, Color.FromRgb(0x0D, 0x0D, 0x0D), 0.72);

        var resources = Application.Current.Resources;

        resources["Color.Accent"] = accent;
        resources["Color.AccentBright"] = bright;
        resources["Color.AccentDeep"] = deep;
        resources["Color.AccentMuted"] = muted;
        resources["Color.Cyan"] = bright;

        resources["Brush.Accent"] = Freeze(new SolidColorBrush(accent));
        resources["Brush.AccentBright"] = Freeze(new SolidColorBrush(bright));
        resources["Brush.AccentDeep"] = Freeze(new SolidColorBrush(deep));
        resources["Brush.AccentMuted"] = Freeze(new SolidColorBrush(muted));
        resources["Brush.Cyan"] = Freeze(new SolidColorBrush(bright));

        resources["Brush.AccentGradient"] = Freeze(new LinearGradientBrush(bright, deep, new Point(0, 0), new Point(1, 1)));

        var gradientHorizontal = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
        gradientHorizontal.GradientStops.Add(new GradientStop(accent, 0));
        gradientHorizontal.GradientStops.Add(new GradientStop(bright, 0.5));
        gradientHorizontal.GradientStops.Add(new GradientStop(accent, 1));
        resources["Brush.AccentGradientHorizontal"] = Freeze(gradientHorizontal);

        var heroGlow = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        heroGlow.GradientStops.Add(new GradientStop(Color.FromArgb(0x33, accent.R, accent.G, accent.B), 0));
        heroGlow.GradientStops.Add(new GradientStop(Color.FromArgb(0x00, 0, 0, 0), 1));
        resources["Brush.HeroGlow"] = Freeze(heroGlow);
    }

    private static Color Blend(Color from, Color to, double amount) => Color.FromRgb(
        (byte)(from.R + (to.R - from.R) * amount),
        (byte)(from.G + (to.G - from.G) * amount),
        (byte)(from.B + (to.B - from.B) * amount));

    private static T Freeze<T>(T freezable) where T : Freezable
    {
        freezable.Freeze();
        return freezable;
    }
}
