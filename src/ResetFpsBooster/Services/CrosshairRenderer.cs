using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

/// <summary>
/// Draws a crosshair into a bitmap, one bitmap pixel per screen pixel.
///
/// Rendered at 96 DPI so one drawing unit is one pixel, and every bar is an
/// axis-aligned rectangle on whole pixels, drawn without anti-aliasing - a
/// 1 px line stays one sharp pixel instead of two half-grey ones. Only the
/// circle is anti-aliased; a round shape needs it.
///
/// The centre: bitmap pixel Size/2 sits on the screen's centre pixel. A bar
/// of thickness t covers [Size/2 - t/2, Size/2 - t/2 + t), so odd widths are
/// centred on that pixel and even ones straddle it the same way every time.
/// </summary>
public static class CrosshairRenderer
{
    public static BitmapSource? Render(CrosshairStyle s)
    {
        return s.Shape == CrosshairShape.Image ? RenderImage(s) : RenderShape(s);
    }

    private static BitmapSource RenderShape(CrosshairStyle s)
    {
        int t = Math.Clamp(s.Thickness, 1, 20);
        int len = Math.Clamp(s.Length, 0, 100);
        int gap = Math.Clamp(s.Gap, 0, 60);
        int o = s.Outline ? Math.Clamp(s.OutlineThickness, 1, 4) : 0;

        int reach = gap + len + t + o + 4;
        int size = Math.Max(16, 2 * reach);
        size += size % 2;
        int c = size / 2;
        int st = c - t / 2;   // first pixel of the centre block

        var bars = new List<Int32Rect>();
        bool arms = s.Shape is CrosshairShape.Cross or CrosshairShape.CrossDot or CrosshairShape.TShape;
        bool dot = s.Shape is CrosshairShape.CrossDot or CrosshairShape.Dot or CrosshairShape.CircleDot;
        bool circle = s.Shape is CrosshairShape.Circle or CrosshairShape.CircleDot;

        if (arms && len > 0)
        {
            bars.Add(new Int32Rect(st + t + gap, st, len, t));          // right
            bars.Add(new Int32Rect(st - gap - len, st, len, t));        // left
            bars.Add(new Int32Rect(st, st + t + gap, t, len));          // down
            if (s.Shape != CrosshairShape.TShape)
                bars.Add(new Int32Rect(st, st - gap - len, t, len));    // up
        }
        if (dot)
            bars.Add(new Int32Rect(st, st, t, t));

        var fill = new SolidColorBrush(ParseColor(s.Color));
        fill.Freeze();
        var black = Brushes.Black;
        var bmp = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);

        // 1) outlines of the bars, crisp
        if (o > 0 && bars.Count > 0)
            bmp.Render(Visual(aliased: true, dc =>
            {
                foreach (var b in bars)
                    dc.DrawRectangle(black, null, new Rect(b.X - o, b.Y - o, b.Width + 2 * o, b.Height + 2 * o));
            }));

        // 2) the circle, smooth; its radius is the length, at least clear of the dot
        if (circle)
        {
            double radius = Math.Max(len, t + 2);
            var centre = new Point(st + t / 2.0, st + t / 2.0);
            bmp.Render(Visual(aliased: false, dc =>
            {
                if (o > 0) dc.DrawEllipse(null, new Pen(black, t + 2 * o), centre, radius, radius);
                dc.DrawEllipse(null, new Pen(fill, t), centre, radius, radius);
            }));
        }

        // 3) the bars themselves, crisp
        if (bars.Count > 0)
            bmp.Render(Visual(aliased: true, dc =>
            {
                foreach (var b in bars)
                    dc.DrawRectangle(fill, null, new Rect(b.X, b.Y, b.Width, b.Height));
            }));

        bmp.Freeze();
        return bmp;
    }

    private static BitmapSource? RenderImage(CrosshairStyle s)
    {
        if (string.IsNullOrWhiteSpace(s.ImagePath) || !File.Exists(s.ImagePath)) return null;
        try
        {
            var src = new BitmapImage();
            src.BeginInit();
            src.CacheOption = BitmapCacheOption.OnLoad;   // do not keep the file locked
            src.UriSource = new Uri(s.ImagePath, UriKind.Absolute);
            src.EndInit();
            src.Freeze();

            double scale = Math.Clamp(s.ImageScale, 10, 300) / 100.0;
            int w = Math.Max(1, (int)Math.Round(src.PixelWidth * scale));
            int h = Math.Max(1, (int)Math.Round(src.PixelHeight * scale));
            int size = Math.Max(w, h) + 2;
            size += size % 2;
            int c = size / 2;

            var bmp = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);
            var v = new DrawingVisual();
            RenderOptions.SetBitmapScalingMode(v, scale == 1.0 ? BitmapScalingMode.NearestNeighbor : BitmapScalingMode.HighQuality);
            using (var dc = v.RenderOpen())
                dc.DrawImage(src, new Rect(c - w / 2, c - h / 2, w, h));
            bmp.Render(v);
            bmp.Freeze();
            return bmp;
        }
        catch
        {
            return null;   // not a readable image; the page says so
        }
    }

    private static DrawingVisual Visual(bool aliased, Action<DrawingContext> draw)
    {
        var v = new DrawingVisual();
        RenderOptions.SetEdgeMode(v, aliased ? EdgeMode.Aliased : EdgeMode.Unspecified);
        using (var dc = v.RenderOpen()) draw(dc);
        return v;
    }

    public static Color ParseColor(string? hex)
    {
        try
        {
            if (!string.IsNullOrWhiteSpace(hex) && ColorConverter.ConvertFromString(hex.Trim()) is Color col)
                return Color.FromRgb(col.R, col.G, col.B);
        }
        catch { /* fall through */ }
        return Color.FromRgb(0x00, 0xFF, 0x66);
    }
}
