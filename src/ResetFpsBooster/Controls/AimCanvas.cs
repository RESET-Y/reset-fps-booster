using System.Globalization;
using System.Windows;
using System.Windows.Media;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Core.Sensitivity;

namespace ResetFpsBooster.Controls;

/// <summary>
/// DRAWS THE AIM TEST: a faint grid that moves as the view turns (without it
/// a turn would not be visible at all), the targets, a crosshair, and the
/// numbers. It only reads an <see cref="AimSession"/>; the window redraws it
/// every frame.
/// </summary>
public sealed class AimCanvas : FrameworkElement
{
    private static readonly Brush Background = Frozen(new SolidColorBrush(Colors.Black));
    private static readonly Pen GridPen = new(Frozen(new SolidColorBrush(Color.FromArgb(0x78, 0xE8, 0x12, 0x1F))), 1);
    private static readonly Pen HorizonPen = new(Frozen(new SolidColorBrush(Color.FromArgb(0xD0, 0xE8, 0x12, 0x1F))), 2);
    private static readonly Brush FlickFill = Frozen(new SolidColorBrush(Color.FromRgb(0xFF, 0x32, 0x42)));
    private static readonly Brush FlickGlow = Frozen(new SolidColorBrush(Color.FromArgb(0x40, 0xFF, 0x32, 0x42)));
    private static readonly Brush TrackFill = Frozen(new SolidColorBrush(Colors.White));
    private static readonly Pen WhiteRing = new(Frozen(new SolidColorBrush(Colors.White)), 2);
    private static readonly Pen RedRing = new(Frozen(new SolidColorBrush(Color.FromRgb(0xE8, 0x12, 0x1F))), 3);
    private static readonly Pen CrossDark = new(Frozen(new SolidColorBrush(Color.FromArgb(0xB0, 0, 0, 0))), 4);
    private static readonly Pen CrossLight = new(Frozen(new SolidColorBrush(Colors.White)), 2);
    private static readonly Brush Dim = Frozen(new SolidColorBrush(Color.FromArgb(0xC8, 0, 0, 0)));
    private static readonly Brush White = Frozen(new SolidColorBrush(Colors.White));
    private static readonly Brush Muted = Frozen(new SolidColorBrush(Color.FromRgb(0xA8, 0xA8, 0xA8)));
    private static readonly Brush Red = Frozen(new SolidColorBrush(Color.FromRgb(0xFF, 0x32, 0x42)));

    private static readonly Typeface Display = new(
        new FontFamily("pack://application:,,,/Assets/Fonts/#Saira Condensed"), FontStyles.Italic, FontWeights.Black, FontStretches.Normal);
    private static readonly Typeface Body = new(
        new FontFamily("pack://application:,,,/Assets/Fonts/#Saira Condensed"), FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal);

    public AimSession? Session { get; set; }

    /// <summary>The game's resolution in pixels; its picture is drawn at this size and fitted to the screen.
    /// Zero means "the whole canvas".</summary>
    public double ViewWidth { get; set; }
    public double ViewHeight { get; set; }

    /// <summary>The picture fills the whole screen even if its shape differs (a stretched resolution,
    /// as in 4:3 on a wide monitor); otherwise it keeps its shape with black bars.</summary>
    public bool Stretch { get; set; }

    private static readonly Pen FramePen = new(Frozen(new SolidColorBrush(Color.FromArgb(0x70, 0xFF, 0xFF, 0xFF))), 1);
    private static readonly Brush View = Frozen(new SolidColorBrush(Color.FromRgb(0x07, 0x07, 0x07)));

    private static T Frozen<T>(T freezable) where T : Freezable { freezable.Freeze(); return freezable; }

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth;
        var h = ActualHeight;
        dc.DrawRectangle(Background, null, new Rect(0, 0, w, h));
        var s = Session;
        if (s is null || w < 10) return;

        // The game's picture, in its own pixels, mapped onto the screen.
        var rw = ViewWidth > 0 ? ViewWidth : w;
        var rh = ViewHeight > 0 ? ViewHeight : h;
        double sx, sy;
        if (Stretch) { sx = w / rw; sy = h / rh; }
        else { sx = sy = Math.Min(w / rw, h / rh); }
        var ox = (w - rw * sx) / 2;
        var oy = (h - rh * sy) / 2;

        dc.PushTransform(new MatrixTransform(sx, 0, 0, sy, ox, oy));
        dc.PushClip(new RectangleGeometry(new Rect(0, 0, rw, rh)));
        dc.DrawRectangle(View, null, new Rect(0, 0, rw, rh));
        DrawGrid(dc, s, rw, rh);
        DrawTargets(dc, s, rw, rh);
        dc.Pop();
        dc.Pop();

        // With bars around it, the edge of the game's picture is marked.
        if (ox > 1 || oy > 1)
            dc.DrawRectangle(null, FramePen, new Rect(ox, oy, rw * sx, rh * sy));

        DrawCrosshair(dc, ox + rw * sx / 2, oy + rh * sy / 2);
        DrawHud(dc, s, w, h);
    }

    private static void DrawGrid(DrawingContext dc, AimSession s, double w, double h)
    {
        // horizontal rings at fixed elevations
        foreach (var el in new[] { -30.0, -15.0, 0.0, 15.0, 30.0 })
        {
            var pen = el == 0 ? HorizonPen : GridPen;
            Point? last = null;
            for (var az = s.Yaw - 80; az <= s.Yaw + 80; az += 4)
            {
                if (AimSession.TryProject(s.Yaw, s.Pitch, az, el, w, h, out var x, out var y, out _, s.HorizontalFov))
                {
                    var p = new Point(x, y);
                    if (last is Point l) dc.DrawLine(pen, l, p);
                    last = p;
                }
                else last = null;
            }
        }
        // vertical lines every 15 degrees of the world, so the grid slides past as you turn
        var first = Math.Floor((s.Yaw - 80) / 15) * 15;
        for (var az = first; az <= s.Yaw + 80; az += 15)
        {
            Point? last = null;
            for (var el = -45.0; el <= 45; el += 5)
            {
                if (AimSession.TryProject(s.Yaw, s.Pitch, az, el, w, h, out var x, out var y, out _, s.HorizontalFov))
                {
                    var p = new Point(x, y);
                    if (last is Point l) dc.DrawLine(GridPen, l, p);
                    last = p;
                }
                else last = null;
            }
        }
    }

    private static void DrawTargets(DrawingContext dc, AimSession s, double w, double h)
    {
        if (s.Phase == AimPhase.Flick && s.HasFlickTarget &&
            AimSession.TryProject(s.Yaw, s.Pitch, s.FlickAz, s.FlickEl, w, h, out var fx, out var fy, out var ppd, s.HorizontalFov))
        {
            var r = ppd * AimSession.FlickRadius;
            dc.DrawEllipse(FlickGlow, null, new Point(fx, fy), r * 2.4, r * 2.4);
            dc.DrawEllipse(FlickFill, WhiteRing, new Point(fx, fy), r, r);
        }
        if (s.Phase == AimPhase.Track)
        {
            var (az, el) = s.TrackTarget;
            if (AimSession.TryProject(s.Yaw, s.Pitch, az, el, w, h, out var tx, out var ty, out var ppd2, s.HorizontalFov))
            {
                var r = ppd2 * AimSession.TrackRadius;
                dc.DrawEllipse(TrackFill, RedRing, new Point(tx, ty), r, r);
            }
        }
    }

    private static void DrawCrosshair(DrawingContext dc, double cx, double cy)
    {
        const double gap = 4, len = 11;
        foreach (var pen in new[] { CrossDark, CrossLight })
        {
            dc.DrawLine(pen, new Point(cx - gap - len, cy), new Point(cx - gap, cy));
            dc.DrawLine(pen, new Point(cx + gap, cy), new Point(cx + gap + len, cy));
            dc.DrawLine(pen, new Point(cx, cy - gap - len), new Point(cx, cy - gap));
            dc.DrawLine(pen, new Point(cx, cy + gap), new Point(cx, cy + gap + len));
        }
    }

    private void DrawHud(DrawingContext dc, AimSession s, double w, double h)
    {
        // top left: where we are in the test; top right: the score so far
        var round = s.RoundIndex == 0 ? Loc.T("Sens.Test.Warmup") : Loc.F("Sens.Test.Round", s.RoundIndex, s.ScoredRounds);
        Text(dc, round, Body, 30, White, 40, 30, TextAlignment.Left);
        var cur = s.CurrentRound;
        Text(dc, Loc.F("Sens.Test.Hits", cur.Hits), Body, 30, White, w - 40, 30, TextAlignment.Right);

        // top centre: what to do now
        string? hint = s.Phase switch
        {
            AimPhase.Flick => Loc.F("Sens.Test.Flick", s.TargetsLeft),
            AimPhase.Track => Loc.F("Sens.Test.Track", Math.Max(0, (int)Math.Ceiling(s.PhaseLength - s.PhaseTime))),
            _ => null
        };
        if (hint is not null) Text(dc, hint, Display, 44, Muted, w / 2, 24, TextAlignment.Center);

        Text(dc, Loc.T("Sens.Test.Esc"), Body, 22, Muted, w / 2, h - 52, TextAlignment.Center);

        // full-screen messages between the parts
        switch (s.Phase)
        {
            case AimPhase.Intro:
                Overlay(dc, w, h, Loc.T("Sens.Test.Ready"), Math.Max(1, (int)Math.Ceiling(s.PhaseLength - s.PhaseTime)).ToString(CultureInfo.InvariantCulture), Loc.T("Sens.Test.IntroHint"));
                break;
            case AimPhase.Break:
                Overlay(dc, w, h,
                    s.RoundIndex == 0 ? Loc.T("Sens.Test.WarmupDone") : Loc.F("Sens.Test.RoundDone", s.RoundIndex, s.ScoredRounds),
                    null, Loc.T("Sens.Test.Changes"));
                break;
            case AimPhase.Done:
                Overlay(dc, w, h, Loc.T("Sens.Test.Done"), null, null);
                break;
        }
    }

    private void Overlay(DrawingContext dc, double w, double h, string title, string? big, string? small)
    {
        dc.DrawRectangle(Dim, null, new Rect(0, 0, w, h));
        Text(dc, title, Display, 96, White, w / 2, h / 2 - 150, TextAlignment.Center);
        if (big is not null) Text(dc, big, Display, 220, Red, w / 2, h / 2 - 40, TextAlignment.Center);
        if (small is not null) Text(dc, small, Body, 36, Muted, w / 2, big is not null ? h / 2 + 200 : h / 2 + 10, TextAlignment.Center);
    }

    private void Text(DrawingContext dc, string text, Typeface face, double size, Brush brush, double x, double y, TextAlignment align)
    {
        var ft = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, face, size, brush,
                                   VisualTreeHelper.GetDpi(this).PixelsPerDip);
        var left = align switch
        {
            TextAlignment.Center => x - ft.Width / 2,
            TextAlignment.Right => x - ft.Width,
            _ => x
        };
        dc.DrawText(ft, new Point(left, y));
    }
}
