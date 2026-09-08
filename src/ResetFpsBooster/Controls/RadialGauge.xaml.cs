using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace ResetFpsBooster.Controls;

/// <summary>A circular percentage gauge: a background track ring plus an animated foreground arc.</summary>
public partial class RadialGauge : UserControl
{
    private const double StartAngleDeg = 215;
    private const double SweepAngleDeg = 290;

    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(RadialGauge),
        new PropertyMetadata(0.0, OnValueChanged));

    public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(
        nameof(Maximum), typeof(double), typeof(RadialGauge), new PropertyMetadata(100.0, OnGeometryAffectingChanged));

    public static readonly DependencyProperty DiameterProperty = DependencyProperty.Register(
        nameof(Diameter), typeof(double), typeof(RadialGauge), new PropertyMetadata(96.0, OnGeometryAffectingChanged));

    public static readonly DependencyProperty ThicknessProperty = DependencyProperty.Register(
        nameof(Thickness), typeof(double), typeof(RadialGauge), new PropertyMetadata(8.0, OnGeometryAffectingChanged));

    public static readonly DependencyProperty AccentBrushProperty = DependencyProperty.Register(
        nameof(AccentBrush), typeof(Brush), typeof(RadialGauge));

    public static readonly DependencyProperty UnitProperty = DependencyProperty.Register(
        nameof(Unit), typeof(string), typeof(RadialGauge), new PropertyMetadata("%"));

    public static readonly DependencyProperty ShowUnitProperty = DependencyProperty.Register(
        nameof(ShowUnit), typeof(bool), typeof(RadialGauge), new PropertyMetadata(true));

    public static readonly DependencyProperty CenterFontSizeProperty = DependencyProperty.Register(
        nameof(CenterFontSize), typeof(double), typeof(RadialGauge), new PropertyMetadata(22.0));

    public static readonly DependencyProperty DecimalsProperty = DependencyProperty.Register(
        nameof(Decimals), typeof(int), typeof(RadialGauge), new PropertyMetadata(0, OnValueChanged));

    /// <summary>Internal animated value driving the drawn arc, separate from the target Value.</summary>
    private static readonly DependencyProperty DisplayValueProperty = DependencyProperty.Register(
        "DisplayValue", typeof(double), typeof(RadialGauge), new PropertyMetadata(0.0, OnGeometryAffectingChanged));

    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    public double Maximum
    {
        get => (double)GetValue(MaximumProperty);
        set => SetValue(MaximumProperty, value);
    }

    public double Diameter
    {
        get => (double)GetValue(DiameterProperty);
        set => SetValue(DiameterProperty, value);
    }

    public double Thickness
    {
        get => (double)GetValue(ThicknessProperty);
        set => SetValue(ThicknessProperty, value);
    }

    public Brush AccentBrush
    {
        get => (Brush)GetValue(AccentBrushProperty);
        set => SetValue(AccentBrushProperty, value);
    }

    public string Unit
    {
        get => (string)GetValue(UnitProperty);
        set => SetValue(UnitProperty, value);
    }

    public bool ShowUnit
    {
        get => (bool)GetValue(ShowUnitProperty);
        set => SetValue(ShowUnitProperty, value);
    }

    public double CenterFontSize
    {
        get => (double)GetValue(CenterFontSizeProperty);
        set => SetValue(CenterFontSizeProperty, value);
    }

    public int Decimals
    {
        get => (int)GetValue(DecimalsProperty);
        set => SetValue(DecimalsProperty, value);
    }

    public string CenterLabel => Value.ToString(Decimals == 0 ? "0" : "0." + new string('#', Decimals));

    public RadialGauge()
    {
        InitializeComponent();
        AccentBrush ??= (Brush)FindResource("Brush.Accent");
        Loaded += (_, _) => RedrawTrack();
    }

    private static void OnValueChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not RadialGauge gauge) return;
        gauge.CenterText.Text = gauge.CenterLabel;

        var clamped = Math.Clamp(gauge.Value, 0, gauge.Maximum <= 0 ? 100 : gauge.Maximum);
        var animation = new DoubleAnimation
        {
            To = clamped,
            Duration = TimeSpan.FromMilliseconds(900),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
        };
        gauge.BeginAnimation(DisplayValueProperty, animation);
    }

    private static void OnGeometryAffectingChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        (d as RadialGauge)?.RedrawArc();
    }

    private void RedrawTrack()
    {
        TrackPath.Data = BuildArcGeometry(1.0);
        RedrawArc();
    }

    private void RedrawArc()
    {
        if (!IsLoaded) return;
        var max = Maximum <= 0 ? 100 : Maximum;
        var fraction = Math.Clamp((double)GetValue(DisplayValueProperty) / max, 0.0, 1.0);
        FillPath.Data = BuildArcGeometry(fraction);
        TrackPath.Data = BuildArcGeometry(1.0);
    }

    private Geometry BuildArcGeometry(double fraction)
    {
        var d = Diameter;
        var thickness = Thickness;
        var radius = (d - thickness) / 2.0;
        var center = new Point(d / 2.0, d / 2.0);

        var sweep = SweepAngleDeg * Math.Max(fraction, 0.0001);
        var startPoint = PointOnCircle(center, radius, StartAngleDeg);
        var endPoint = PointOnCircle(center, radius, StartAngleDeg + sweep);
        var isLargeArc = sweep > 180.0;

        var figure = new PathFigure { StartPoint = startPoint, IsClosed = false };
        figure.Segments.Add(new ArcSegment(endPoint, new Size(radius, radius), 0, isLargeArc, SweepDirection.Clockwise, true));

        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        return geometry;
    }

    private static Point PointOnCircle(Point center, double radius, double angleDegrees)
    {
        var rad = angleDegrees * Math.PI / 180.0;
        return new Point(
            center.X + radius * Math.Sin(rad),
            center.Y - radius * Math.Cos(rad));
    }
}
