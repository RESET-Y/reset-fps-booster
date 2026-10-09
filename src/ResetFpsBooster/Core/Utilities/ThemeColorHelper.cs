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

        // The stage's lights follow the accent too (Colors.xaml has the same shapes in red).
        var navActive = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
        navActive.GradientStops.Add(new GradientStop(WithAlpha(accent, 0x40), 0));
        navActive.GradientStops.Add(new GradientStop(WithAlpha(accent, 0x00), 1));
        resources["Brush.NavActive"] = Freeze(navActive);

        var cardAccent = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        cardAccent.GradientStops.Add(new GradientStop(WithAlpha(accent, 0x33), 0));
        cardAccent.GradientStops.Add(new GradientStop(Color.FromArgb(0xFF, 0x0C, 0x0C, 0x0C), 0.55));
        resources["Brush.CardAccent"] = Freeze(cardAccent);

        var horizonGlow = new RadialGradientBrush
        {
            Center = new Point(0.5, 1), GradientOrigin = new Point(0.5, 1), RadiusX = 0.8, RadiusY = 1.4,
        };
        horizonGlow.GradientStops.Add(new GradientStop(WithAlpha(accent, 0x55), 0));
        horizonGlow.GradientStops.Add(new GradientStop(WithAlpha(accent, 0x00), 0.7));
        resources["Brush.HorizonGlow"] = Freeze(horizonGlow);

        // The horizon band and the stage inside the dashboard cards: the accent, deepened to near black.
        var night = Blend(accent, Colors.Black, 0.8);
        var horizon = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        horizon.GradientStops.Add(new GradientStop(WithAlpha(deep, 0x00), 0));
        horizon.GradientStops.Add(new GradientStop(WithAlpha(deep, 0x44), 0.6));
        horizon.GradientStops.Add(new GradientStop(WithAlpha(night, 0x99), 1));
        resources["Brush.Horizon"] = Freeze(horizon);

        var stage = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        stage.GradientStops.Add(new GradientStop(Color.FromArgb(0xF0, 0x0C, 0x0C, 0x0C), 0));
        stage.GradientStops.Add(new GradientStop(Color.FromArgb(0xF0, 0x0C, 0x0C, 0x0C), 0.5));
        stage.GradientStops.Add(new GradientStop(WithAlpha(night, 0xE0), 1));
        resources["Brush.StageFill"] = Freeze(stage);
    }

    private static Color WithAlpha(Color c, byte alpha) => Color.FromArgb(alpha, c.R, c.G, c.B);

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
