using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;

namespace ResetFpsBooster;

/// <summary>
/// The application's cinematic boot sequence. Every visual element reuses the exact palette and
/// motifs of the real dashboard (pure black, white typography, #FF1A25 red accents, thin technical
/// lines) so the intro reads as the product itself starting up rather than a separate movie.
/// </summary>
public partial class SplashWindow : Window
{
    private static readonly Color AccentColor = Color.FromRgb(0xFF, 0x1A, 0x25);
    private static readonly Brush AccentBrush = new SolidColorBrush(AccentColor);
    private static readonly Brush LeaderBrush = new SolidColorBrush(Color.FromRgb(0x2A, 0x20, 0x20));

    private readonly List<Rectangle> _fragments = new();
    private readonly List<(Grid Row, TranslateTransform Transform)> _initRows = new();
    private readonly List<TextBlock> _scanLabels = new();
    private Rectangle _scanLine = null!;

    public SplashWindow()
    {
        InitializeComponent();
    }

    public async Task RunIntroAsync()
    {
        BuildLogoFragments();
        BuildInitRows();
        BuildScanElements();

        await Task.Delay(300);

        await PhaseCenterLineAsync();
        await PhaseLogoAssembleAsync();
        await PhaseGlitchAsync();
        await Task.Delay(220);
        await FadeOutAsync(LogoStage, 220);

        await PhaseInitListAsync();
        await FadeOutAsync(InitStage, 220);

        await PhaseScanAsync();

        await PhaseBlackoutAsync();
        await PhaseHeroAsync();
        await Task.Delay(200);
        await FadeOutAsync(HeroStage, 220);

        await PhaseScoreAsync();
        await PhaseFinalTransitionAsync();
    }

    // ---------------------------------------------------------------- Phase 1: center line

    private async Task PhaseCenterLineAsync()
    {
        Anim(CenterLine, FrameworkElement.WidthProperty, 300, 480, ease: new CubicEase { EasingMode = EasingMode.EaseOut });
        Anim(CenterLineGlow, System.Windows.Media.Effects.DropShadowEffect.OpacityProperty, 0.6, 480);
        await Task.Delay(520);
    }

    // ---------------------------------------------------------------- Phase 2: logo assembly

    private void BuildLogoFragments()
    {
        const int columns = 6;
        const double hostWidth = 360;
        var stripWidth = hostWidth / columns;

        for (var i = 0; i < columns; i++)
        {
            var rect = new Rectangle
            {
                Width = stripWidth,
                Height = 60,
                Fill = Brushes.Black
            };
            Canvas.SetLeft(rect, i * stripWidth);
            Canvas.SetTop(rect, 0);
            FragmentOverlay.Children.Add(rect);
            _fragments.Add(rect);
        }
    }

    private async Task PhaseLogoAssembleAsync()
    {
        LogoHost.Opacity = 1;

        int[] revealOrder = { 2, 4, 1, 5, 0, 3 };
        for (var i = 0; i < revealOrder.Length; i++)
        {
            Anim(_fragments[revealOrder[i]], UIElement.OpacityProperty, 0, 100, delayMs: i * 55);
        }

        Anim(SubtitleText, UIElement.OpacityProperty, 1, 320, delayMs: 380);
        await Task.Delay(520);
    }

    private async Task PhaseGlitchAsync()
    {
        var jitter = new DoubleAnimationUsingKeyFrames();
        jitter.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        jitter.KeyFrames.Add(new LinearDoubleKeyFrame(-4, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(30))));
        jitter.KeyFrames.Add(new LinearDoubleKeyFrame(3, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(60))));
        jitter.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(100))));
        LogoJitter.BeginAnimation(TranslateTransform.XProperty, jitter);

        var flicker = new DoubleAnimationUsingKeyFrames();
        flicker.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        flicker.KeyFrames.Add(new LinearDoubleKeyFrame(0.25, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(25))));
        flicker.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(45))));
        flicker.KeyFrames.Add(new LinearDoubleKeyFrame(0.4, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(70))));
        flicker.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(100))));
        LogoImage.BeginAnimation(UIElement.OpacityProperty, flicker);

        await Task.Delay(180);
    }

    // ---------------------------------------------------------------- Phase 3: system initialization

    private void BuildInitRows()
    {
        string[] labels = { "CPU", "GPU", "MEMORY", "NETWORK", "SYSTEM SERVICES" };

        foreach (var label in labels)
        {
            var row = new Grid { Margin = new Thickness(0, 7, 0, 7), Opacity = 0 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(170) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var labelText = new TextBlock
            {
                Text = label,
                FontFamily = new FontFamily("Segoe UI"),
                FontSize = 12,
                Foreground = Brushes.White,
                VerticalAlignment = VerticalAlignment.Center
            };
            Grid.SetColumn(labelText, 0);

            var leader = new Border
            {
                Height = 1,
                Background = LeaderBrush,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(12, 0, 12, 0)
            };
            Grid.SetColumn(leader, 1);

            var status = new TextBlock
            {
                Text = "READY",
                FontFamily = new FontFamily("Segoe UI Semibold"),
                FontSize = 12,
                Foreground = AccentBrush,
                VerticalAlignment = VerticalAlignment.Center
            };
            Grid.SetColumn(status, 2);

            row.Children.Add(labelText);
            row.Children.Add(leader);
            row.Children.Add(status);

            var translate = new TranslateTransform(-18, 0);
            row.RenderTransform = translate;

            InitRowsPanel.Children.Add(row);
            _initRows.Add((row, translate));
        }
    }

    private async Task PhaseInitListAsync()
    {
        Anim(InitStage, UIElement.OpacityProperty, 1, 220);
        await Task.Delay(260);

        foreach (var (row, translate) in _initRows)
        {
            Anim(row, UIElement.OpacityProperty, 1, 220);
            Anim(translate, TranslateTransform.XProperty, 0, 220, ease: new CubicEase { EasingMode = EasingMode.EaseOut });
            await Task.Delay(125);
        }

        await Task.Delay(150);
    }

    // ---------------------------------------------------------------- Phase 4: system scan

    private void BuildScanElements()
    {
        _scanLine = new Rectangle
        {
            Width = Width,
            Height = 2,
            Fill = AccentBrush,
            Effect = new System.Windows.Media.Effects.DropShadowEffect
            {
                Color = AccentColor,
                BlurRadius = 16,
                ShadowDepth = 0,
                Opacity = 0.6
            }
        };
        Canvas.SetLeft(_scanLine, 0);
        Canvas.SetTop(_scanLine, -2);
        ScanStage.Children.Add(_scanLine);

        string[] labels = { "CPU", "GPU", "RAM", "STORAGE", "NETWORK", "FPS" };
        var spacing = Height / (labels.Length + 1);

        foreach (var (label, i) in labels.Select((l, i) => (l, i)))
        {
            var tb = new TextBlock
            {
                Text = label,
                FontFamily = new FontFamily("Segoe UI Semibold"),
                FontSize = 13,
                Foreground = Brushes.White,
                Opacity = 0
            };
            Canvas.SetLeft(tb, Width / 2 - 30);
            Canvas.SetTop(tb, spacing * (i + 1) - 10);
            ScanStage.Children.Add(tb);
            _scanLabels.Add(tb);
        }
    }

    private async Task PhaseScanAsync()
    {
        const int sweepMs = 850;
        Anim(ScanStage, UIElement.OpacityProperty, 1, 150);

        var lineAnim = new DoubleAnimation(-2, Height + 2, TimeSpan.FromMilliseconds(sweepMs))
        {
            EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
        };
        _scanLine.BeginAnimation(Canvas.TopProperty, lineAnim);

        foreach (var label in _scanLabels)
        {
            var top = Canvas.GetTop(label);
            var fraction = (top + 2) / (Height + 4);
            var delay = Math.Max(0, (int)(fraction * sweepMs) - 90);

            var fade = new DoubleAnimationUsingKeyFrames { BeginTime = TimeSpan.FromMilliseconds(delay) };
            fade.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
            fade.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(80))));
            fade.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(220))));
            fade.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(320))));
            label.BeginAnimation(UIElement.OpacityProperty, fade);
        }

        await Task.Delay(sweepMs + 130);
        await FadeOutAsync(ScanStage, 160);
    }

    // ---------------------------------------------------------------- Phase 5: hero moment

    private async Task PhaseBlackoutAsync()
    {
        Anim(BlackCover, UIElement.OpacityProperty, 1, 180);
        await Task.Delay(340);
    }

    private async Task PhaseHeroAsync()
    {
        // BlackCover was driven by an animation in PhaseBlackoutAsync, which holds its end value
        // (Opacity 1) even after completing — a direct property assignment would be silently
        // ignored while that animation clock is still attached, so clear the clock instead.
        BlackCover.BeginAnimation(UIElement.OpacityProperty, null);
        BlackCover.Opacity = 0;

        Anim(HeroStage, UIElement.OpacityProperty, 1, 420);
        Anim(HeroScale, ScaleTransform.ScaleXProperty, 1.0, 460, ease: new CubicEase { EasingMode = EasingMode.EaseOut });
        Anim(HeroScale, ScaleTransform.ScaleYProperty, 1.0, 460, ease: new CubicEase { EasingMode = EasingMode.EaseOut });
        Anim(HeroGlow, UIElement.OpacityProperty, 1, 550, delayMs: 120);
        await Task.Delay(220);

        Anim(HeroLine, FrameworkElement.WidthProperty, 220, 380, ease: new CubicEase { EasingMode = EasingMode.EaseOut });
        await Task.Delay(480);
    }

    // ---------------------------------------------------------------- Phase 6: performance score

    private async Task PhaseScoreAsync()
    {
        Anim(ScoreStage, UIElement.OpacityProperty, 1, 260);
        await Task.Delay(180);

        int[] steps = { 0, 32, 58, 74, 87 };
        foreach (var value in steps)
        {
            ScoreNumberText.Text = value.ToString();
            await Task.Delay(105);
        }

        Anim(ReadyText, UIElement.OpacityProperty, 1, 350);
        await Task.Delay(700);
    }

    // ---------------------------------------------------------------- Final transition

    private async Task PhaseFinalTransitionAsync()
    {
        Anim(ScoreStage, UIElement.OpacityProperty, 0, 300);

        DockingLogo.Width = 240;
        DockingLogo.Margin = new Thickness(520, 380, 0, 0);
        DockingLogo.Opacity = 0;

        Anim(DockingLogo, UIElement.OpacityProperty, 1, 200, delayMs: 100);

        var marginAnim = new ThicknessAnimation(new Thickness(520, 380, 0, 0), new Thickness(24, 26, 0, 0), TimeSpan.FromMilliseconds(430))
        {
            BeginTime = TimeSpan.FromMilliseconds(100),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };
        DockingLogo.BeginAnimation(MarginProperty, marginAnim);

        var widthAnim = new DoubleAnimation(240, 84, TimeSpan.FromMilliseconds(430))
        {
            BeginTime = TimeSpan.FromMilliseconds(100),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };
        DockingLogo.BeginAnimation(WidthProperty, widthAnim);

        Anim(TopStripe, UIElement.OpacityProperty, 1, 300, delayMs: 200);

        await Task.Delay(460);

        Anim(RootGrid, UIElement.OpacityProperty, 0, 280);
        await Task.Delay(300);
    }

    // ---------------------------------------------------------------- helpers

    private static async Task FadeOutAsync(UIElement element, int ms)
    {
        Anim(element, UIElement.OpacityProperty, 0, ms);
        await Task.Delay(ms + 20);
    }

    private static void Anim(IAnimatable target, DependencyProperty property, double to, int ms, int delayMs = 0, IEasingFunction? ease = null)
    {
        var animation = new DoubleAnimation(to, TimeSpan.FromMilliseconds(ms))
        {
            BeginTime = TimeSpan.FromMilliseconds(delayMs),
            EasingFunction = ease ?? new QuadraticEase { EasingMode = EasingMode.EaseOut }
        };
        target.BeginAnimation(property, animation);
    }
}
