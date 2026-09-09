using System.Windows;
using System.Windows.Media;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Minimal 16x16 navigation icons built entirely from primitive Geometry objects (rectangles,
/// ellipses, straight line segments) rather than hand-written path mini-language strings — this
/// guarantees they render correctly with no risk of a typo producing an invisible/garbled icon.
/// </summary>
public static class IconGeometries
{
    public static Geometry Get(IconKind kind) => kind switch
    {
        IconKind.Dashboard => Dashboard(),
        IconKind.Optimizer => Optimizer(),
        IconKind.Games => Games(),
        IconKind.Performance => Performance(),
        IconKind.BottleneckEngine => BottleneckEngineIcon(),
#if RFB_BETA
        IconKind.FrameBoostBeta => FrameBoostBetaIcon(),
#endif
        IconKind.System => SystemIcon(),
        IconKind.Backups => Backups(),
        IconKind.Logs => Logs(),
        IconKind.Settings => Settings(),
        _ => Geometry.Empty
    };

    private static Geometry Dashboard()
    {
        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(new RectangleGeometry(new Rect(1, 1, 6, 6), 1.5, 1.5));
        group.Children.Add(new RectangleGeometry(new Rect(9, 1, 6, 6), 1.5, 1.5));
        group.Children.Add(new RectangleGeometry(new Rect(1, 9, 6, 6), 1.5, 1.5));
        group.Children.Add(new RectangleGeometry(new Rect(9, 9, 6, 6), 1.5, 1.5));
        return group;
    }

    private static Geometry Optimizer()
    {
        var figure = new PathFigure { StartPoint = new Point(9, 1), IsClosed = true, IsFilled = true };
        figure.Segments.Add(new LineSegment(new Point(3, 9), true));
        figure.Segments.Add(new LineSegment(new Point(7, 9), true));
        figure.Segments.Add(new LineSegment(new Point(6, 15), true));
        figure.Segments.Add(new LineSegment(new Point(13, 6), true));
        figure.Segments.Add(new LineSegment(new Point(9, 6), true));
        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        return geometry;
    }

    private static Geometry Games()
    {
        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(new RectangleGeometry(new Rect(1, 4, 14, 8), 4, 4));
        group.Children.Add(new RectangleGeometry(new Rect(3.3, 6.8, 3, 1)));
        group.Children.Add(new RectangleGeometry(new Rect(4.3, 5.8, 1, 3)));
        group.Children.Add(new EllipseGeometry(new Point(11, 6.7), 1, 1));
        group.Children.Add(new EllipseGeometry(new Point(12.6, 8.3), 1, 1));
        return group;
    }

    private static Geometry Performance()
    {
        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(new RectangleGeometry(new Rect(2, 8, 3, 6)));
        group.Children.Add(new RectangleGeometry(new Rect(6.5, 5, 3, 9)));
        group.Children.Add(new RectangleGeometry(new Rect(11, 2, 3, 12)));
        return group;
    }

    private static Geometry BottleneckEngineIcon()
    {
        var center = new Point(8, 8);
        var ring = new GeometryGroup { FillRule = FillRule.EvenOdd };
        ring.Children.Add(new EllipseGeometry(center, 6.4, 6.4));
        ring.Children.Add(new EllipseGeometry(center, 4.8, 4.8));

        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(ring);
        group.Children.Add(new EllipseGeometry(center, 1.6, 1.6));
        group.Children.Add(new RectangleGeometry(new Rect(7.4, 0.4, 1.2, 3)));
        group.Children.Add(new RectangleGeometry(new Rect(7.4, 12.6, 1.2, 3)));
        group.Children.Add(new RectangleGeometry(new Rect(0.4, 7.4, 3, 1.2)));
        group.Children.Add(new RectangleGeometry(new Rect(12.6, 7.4, 3, 1.2)));
        return group;
    }

#if RFB_BETA
    private static Geometry FrameBoostBetaIcon()
    {
        // Two overlapping window frames - the real capture-window relationship.
        var group = new GeometryGroup { FillRule = FillRule.EvenOdd };
        group.Children.Add(new RectangleGeometry(new Rect(1, 1, 10, 8), 1, 1));
        group.Children.Add(new RectangleGeometry(new Rect(2.4, 2.4, 7.2, 5.2)));
        group.Children.Add(new RectangleGeometry(new Rect(5, 7, 10, 8), 1, 1));
        group.Children.Add(new RectangleGeometry(new Rect(6.4, 8.4, 7.2, 5.2)));
        return group;
    }
#endif

    private static Geometry SystemIcon()
    {
        var screen = new GeometryGroup { FillRule = FillRule.EvenOdd };
        screen.Children.Add(new RectangleGeometry(new Rect(1, 2, 14, 9), 1, 1));
        screen.Children.Add(new RectangleGeometry(new Rect(2.6, 3.5, 10.8, 6)));

        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(screen);
        group.Children.Add(new RectangleGeometry(new Rect(6.5, 11, 3, 2)));
        group.Children.Add(new RectangleGeometry(new Rect(4, 13.2, 8, 1.2), 0.5, 0.5));
        return group;
    }

    private static Geometry Backups()
    {
        var center = new Point(8, 8);
        var ring = new GeometryGroup { FillRule = FillRule.EvenOdd };
        ring.Children.Add(new EllipseGeometry(center, 7, 7));
        ring.Children.Add(new EllipseGeometry(center, 5.4, 5.4));

        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(ring);
        group.Children.Add(new RectangleGeometry(new Rect(7.5, 3.6, 1, 4.6)));
        group.Children.Add(new RectangleGeometry(new Rect(8, 7.5, 3.8, 1)));
        return group;
    }

    private static Geometry Logs()
    {
        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(new RectangleGeometry(new Rect(1, 3, 14, 2)));
        group.Children.Add(new RectangleGeometry(new Rect(1, 7, 10, 2)));
        group.Children.Add(new RectangleGeometry(new Rect(1, 11, 12, 2)));
        return group;
    }

    private static Geometry Settings()
    {
        var center = new Point(8, 8);
        var ring = new GeometryGroup { FillRule = FillRule.EvenOdd };
        ring.Children.Add(new EllipseGeometry(center, 5, 5));
        ring.Children.Add(new EllipseGeometry(center, 3.1, 3.1));

        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        group.Children.Add(ring);

        for (var i = 0; i < 6; i++)
        {
            var tooth = new RectangleGeometry(new Rect(7, 0.3, 2, 3));
            tooth.Transform = new RotateTransform(i * 60, center.X, center.Y);
            group.Children.Add(tooth);
        }

        return group;
    }
}
