using System.Management;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Hardware;

namespace ResetFpsBooster;

/// <summary>
/// The start-up screen, in red, black and white. The hardware is read out cue by
/// cue (each value counts or types in), the red horizon brightens with every
/// value, and the finish is the brand plate slamming across the screen with
/// "BEREIT", hit by a white flash. A click or a key press skips the show. The hardware read starts at once, so the values are
/// normally ready before the first cue. Then App.xaml.cs shows the dashboard.
/// </summary>
public partial class SplashWindow : Window
{
    private const double BarWidth = 1152;
    private const string SairaFamily = "pack://application:,,,/Assets/Fonts/#Saira Condensed";

    private static readonly Brush Accent = new SolidColorBrush(Color.FromRgb(0xE8, 0x12, 0x1F));
    private static readonly Brush Muted = new SolidColorBrush(Color.FromRgb(0xA8, 0xA8, 0xA8));
    private static readonly Brush Line = new SolidColorBrush(Color.FromRgb(0x1C, 0x1C, 0x1C));

    private double _progress;
    private bool _skip;

    public SplashWindow()
    {
        InitializeComponent();
        Accent.Freeze();
        Muted.Freeze();
        Line.Freeze();
    }

    public async Task RunIntroAsync()
    {
        var system = LoadSystemAsync();   // starts now; the screen does not wait for it
        await ShowSystemCheckAsync(system);
        await Animate(400, p => RootGrid.Opacity = 1 - p);   // the dashboard takes over
    }

    private void OnSkipMouse(object sender, MouseButtonEventArgs e) => _skip = true;

    private void OnSkipKey(object sender, KeyEventArgs e) => _skip = true;

    /// <summary>One cue of the check. Number set: the number counts up to it,
    /// then Suffix follows. Number unset: Text types in.</summary>
    private sealed record Row(string Label, string Text, double? Number = null, string Suffix = "");

    private static async Task<(SystemSnapshot? Snapshot, int RefreshHz)> LoadSystemAsync()
    {
        try
        {
            var snapshotTask = new HardwareService().GetSnapshotAsync();
            var hzTask = Task.Run(ReadRefreshHz);
            await Task.WhenAll(snapshotTask, hzTask);
            return (snapshotTask.Result, hzTask.Result);
        }
        catch
        {
            return (null, 0);   // the check still runs, with what it has
        }
    }

    // The primary display's refresh rate; 0 when Windows does not say.
    private static int ReadRefreshHz()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT CurrentRefreshRate FROM Win32_VideoController");
            foreach (var obj in searcher.Get())
            {
                var hz = Convert.ToInt32(obj["CurrentRefreshRate"] ?? 0);
                if (hz > 0) return hz;
            }
        }
        catch { /* no WMI: no number */ }
        return 0;
    }

    private static List<Row> BuildRows(SystemSnapshot? s, int hz)
    {
        if (s is null) return new List<Row> { new("SYSTEM", "nicht lesbar") };

        const double GiB = 1024.0 * 1024.0 * 1024.0;
        var rows = new List<Row>
        {
            new("PROZESSOR", CpuName(s.Cpu.Name)),
            new("KERNE", "", s.Cpu.Cores, $" Kerne · {s.Cpu.LogicalProcessors} Threads"),
            new("GRAFIKKARTE", Clean(s.PrimaryGpu?.Name ?? "unbekannt")),
            new("ARBEITSSPEICHER", "", Math.Round(s.Memory.TotalBytes / GiB),
                s.Memory.SpeedMhz > 0 ? $" GB · {s.Memory.SpeedMhz:0} MHz" : " GB"),
        };
        rows.Add(hz > 0 ? new Row("BILDWIEDERHOLRATE", "", hz, " Hz") : new Row("BILDWIEDERHOLRATE", "unbekannt"));
        rows.Add(new Row("WINDOWS", $"{Clean(s.OperatingSystem.ProductName)} · Build {s.OperatingSystem.Build}"));
        return rows;
    }

    private static string Clean(string text) => string.Join(' ', text.Split(' ', StringSplitOptions.RemoveEmptyEntries));

    // "AMD Ryzen 5 5600 6-Core Processor" -> "AMD Ryzen 5 5600"; "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz"
    // -> "Intel Core i7-9700K". The cue has room for the model, not the marketing.
    private static string CpuName(string raw) =>
        Clean(Regex.Replace(raw, @"\((R|TM)\)|\s+\d+-Core Processor|\s+Processor|\s+CPU @ [\d.]+\s*GHz", "", RegexOptions.IgnoreCase));

    private async Task ShowSystemCheckAsync(Task<(SystemSnapshot? Snapshot, int RefreshHz)> system)
    {
        await Animate(350, p => CheckPanel.Opacity = p);
        AddLog("system-check gestartet");

        var (snapshot, hz) = await system;
        var rows = BuildRows(snapshot, hz);
        for (var i = 0; i < rows.Count; i++)
        {
            var row = rows[i];
            var (value, status, cue) = AddRow(i + 1, row.Label);
            await Animate(400, p => value.Text = row.Number is double n
                ? ((int)Math.Round(n * p)).ToString() + row.Suffix
                : row.Text[..(int)Math.Round(row.Text.Length * p)]);
            value.Text = row.Number is double final ? final.ToString("0") + row.Suffix : row.Text;
            status.Text = "OK";
            AddLog($"{row.Label.ToLower()} erkannt");
            _ = SweepAsync(cue);   // the cue "goes": a light streak runs across the line

            await AdvanceProgress((i + 1) / (double)rows.Count);
            await Pause(60);
        }

        await Pause(200);

        // The brand plate slams across the screen with BEREIT on it ...
        await Animate(380, p => PlateShift.X = -1900 * (1 - p));
        // ... and hits: a white flash and a hard shake at the same moment.
        await Task.WhenAll(Animate(380, p => WhiteFlash.Opacity = 0.8 * (1 - p)), ShakeAsync());
        await Pause(650);
    }

    private async Task ShakeAsync()
    {
        foreach (var x in new[] { 14.0, -12, 9, -6, 3, 0 })
        {
            ShakeTransform.X = x;
            await Task.Delay(32);
        }
    }

    // A bright streak that runs once across a finished cue, like a lighting cue firing.
    private async Task SweepAsync(Grid cue)
    {
        var streak = new Rectangle
        {
            Width = 180,
            HorizontalAlignment = HorizontalAlignment.Left,
            IsHitTestVisible = false,
            Fill = new LinearGradientBrush(
                new GradientStopCollection
                {
                    new GradientStop(Color.FromArgb(0, 255, 255, 255), 0),
                    new GradientStop(Color.FromArgb(70, 255, 255, 255), 0.5),
                    new GradientStop(Color.FromArgb(0, 255, 255, 255), 1),
                }) { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) },
            RenderTransform = new TranslateTransform(-180, 0),
        };
        Grid.SetColumnSpan(streak, 3);
        cue.Children.Add(streak);
        var move = (TranslateTransform)streak.RenderTransform;
        await Animate(520, p => move.X = -180 + (cue.ActualWidth + 180) * p);
        cue.Children.Remove(streak);
    }

    // The bar, the live percentage and the horizon move together: the more is
    // read, the more light comes up.
    private async Task AdvanceProgress(double target)
    {
        var from = _progress;
        await Animate(300, p =>
        {
            var v = from + (target - from) * p;
            ProgressFill.Width = BarWidth * v;
            PercentText.Text = ((int)Math.Round(v * 100)).ToString("000");
            Horizon.Opacity = v;
            Dawn.Opacity = v * 0.9;
            Beams.Opacity = 0.25 + 0.75 * v;       // the lights get stronger ...
            LogoImage.Opacity = 0.35 + 0.65 * v;   // ... and the logo comes up out of the dark
        });
        _progress = target;
    }

    // One line of the boot log; the oldest drop off the top.
    private void AddLog(string text)
    {
        LogHost.Children.Add(new TextBlock
        {
            FontFamily = new FontFamily("Consolas"),
            FontSize = 13,
            Foreground = Muted,
            TextAlignment = TextAlignment.Right,
            Margin = new Thickness(0, 0, 0, 4),
            Inlines = { new Run("[ OK ] ") { Foreground = Accent }, new Run(text) },
        });
        while (LogHost.Children.Count > 12) LogHost.Children.RemoveAt(0);
    }

    private (TextBlock Value, TextBlock Status, Grid Cue) AddRow(int cue, string label)
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(230) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var name = new TextBlock
        {
            Text = $"{cue:00}  {label}",
            FontFamily = new FontFamily(SairaFamily),
            FontWeight = FontWeights.SemiBold,
            FontSize = 15,
            Foreground = Muted,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var value = new TextBlock
        {
            FontFamily = new FontFamily(SairaFamily),
            FontWeight = FontWeights.Black,
            FontSize = 30,
            Foreground = Brushes.White,
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var status = new TextBlock
        {
            Text = "…",
            FontFamily = new FontFamily(SairaFamily),
            FontWeight = FontWeights.Bold,
            FontSize = 15,
            Foreground = Accent,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(14, 0, 0, 0),
        };

        Grid.SetColumn(value, 1);
        Grid.SetColumn(status, 2);
        grid.Children.Add(name);
        grid.Children.Add(value);
        grid.Children.Add(status);

        RowHost.Children.Add(new Border
        {
            BorderBrush = Line,
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(0, 8, 0, 8),
            Margin = new Thickness(0, 0, 0, 6),
            Child = grid,
        });
        return (value, status, grid);
    }

    // A pause that a click or key press cuts short.
    private Task Pause(int ms) => _skip ? Task.CompletedTask : Task.Delay(ms);

    // Calls step with 0..1 over "ms", eased out, on the UI thread. A skip jumps
    // straight to the end state.
    private async Task Animate(double ms, Action<double> step)
    {
        var start = DateTime.UtcNow;
        while (!_skip)
        {
            var t = Math.Min(1, (DateTime.UtcNow - start).TotalMilliseconds / ms);
            step(1 - Math.Pow(1 - t, 3));
            if (t >= 1) return;
            await Task.Delay(16);
        }
        step(1);
    }
}
