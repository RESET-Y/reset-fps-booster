using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Sensitivity;
using ResetFpsBooster.Services;
using ResetFpsBooster.Views;

namespace ResetFpsBooster.ViewModels;

public sealed record SensGameChoice(SensGame Game, string Label);

/// <summary>One row of the result table: a round at one sensitivity.</summary>
public sealed class SensRoundRow
{
    public string Sens { get; init; } = "";
    /// <summary>0..1, for the bar.</summary>
    public double ScoreBar { get; init; }
    public bool IsTop { get; init; }
    public string Hits { get; init; } = "";
    public string Time { get; init; } = "";
    public string Track { get; init; } = "";
}

/// <summary>The same turn in another game.</summary>
public sealed class SensGameRow
{
    public string Game { get; init; } = "";
    public string Value { get; init; } = "";
}

/// <summary>
/// SENS-FINDER: choose the game, say what mouse DPI and sensitivity you play
/// with, play the short aim test, get the sensitivity the rounds favour - and
/// the same turn converted for the other games.
///
/// It reads nothing from any game. The only things measured are the mouse
/// counts and clicks inside our own test window; the games' sensitivity
/// scales are published constants (see <see cref="SensMath"/>).
/// </summary>
public sealed partial class SensFinderViewModel : ViewModelBase
{
    // Six scored rounds from a bit more than half to nearly twice the player's own speed.
    private static readonly double[] Multipliers = { 0.52, 0.68, 0.88, 1.14, 1.48, 1.93 };

    // Used only until a value of the player's own is stored for a game.
    private static double DefaultSens(SensGame game) => game switch
    {
        SensGame.ApexLegends => 2.0,
        SensGame.CounterStrike2 => 1.0,
        SensGame.Valorant => 0.4,
        SensGame.Fortnite => 6.0,
        _ => 1.0
    };

    // The field of view the game starts with (or fixes), horizontal, for a 16:9 picture.
    private static double DefaultFov(SensGame game) => game switch
    {
        SensGame.ApexLegends => 90,
        SensGame.CounterStrike2 => 106,
        SensGame.Valorant => 103,
        SensGame.Fortnite => 80,
        _ => 100
    };

    private readonly ISettingsService _settings;
    private bool _loading;

    // What the last test produced, kept so the texts can be written again in another language.
    private SensRecommendation? _rec;
    private IReadOnlyList<AimRoundResult> _results = Array.Empty<AimRoundResult>();
    private SensGame _testGame;
    private double _testDpi;
    private double _testSens;

    public IReadOnlyList<SensGameChoice> Games { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SensUnit))]
    [NotifyPropertyChangedFor(nameof(CurrentCm360Text))]
    [NotifyPropertyChangedFor(nameof(FovHint))]
    private SensGameChoice? _selectedGame;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CurrentCm360Text))]
    private string _dpiText = "800";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CurrentCm360Text))]
    private string _sensText = "1.0";

    [ObservableProperty] private string _fovText = "103";
    [ObservableProperty] private string _resWidthText = "1920";
    [ObservableProperty] private string _resHeightText = "1080";
    [ObservableProperty] private bool _stretch;

    [ObservableProperty] private string? _usedMessage;

    // ---- result ----
    [ObservableProperty] private bool _hasResult;
    [ObservableProperty] private string _resultSens = "";
    [ObservableProperty] private string _resultChange = "";
    [ObservableProperty] private string _resultCm360 = "";
    [ObservableProperty] private string _resultRange = "";
    [ObservableProperty] private string _confidenceText = "";
    [ObservableProperty] private int _confidenceLevel;
    [ObservableProperty] private string? _resultNotice;
    [ObservableProperty] private string? _gameNote;
    public ObservableCollection<SensRoundRow> Rounds { get; } = new();
    public ObservableCollection<SensGameRow> OtherGames { get; } = new();

    /// <summary>What the game does with its field of view, so the player knows which number to type.</summary>
    public string FovHint => SelectedGame is null ? "" : Loc.T("Sens.FovHint." + SelectedGame.Game);

    public string SensUnit => SelectedGame is null ? "" : SensMath.Unit(SelectedGame.Game).Trim();

    /// <summary>"51.9 cm per 360°" for what is typed above, so a wrong DPI or sensitivity shows at once.</summary>
    public string CurrentCm360Text
    {
        get
        {
            if (SelectedGame is null || !TryReadDpi(out var dpi) || !TryReadSens(out var sens)) return "-";
            return Loc.F("Sens.Cm360", SensMath.Cm360(SelectedGame.Game, dpi, sens).ToString("0.0", CultureInfo.CurrentCulture));
        }
    }

    public SensFinderViewModel(ISettingsService settings)
    {
        _settings = settings;
        Games = Enum.GetValues<SensGame>().Select(g => new SensGameChoice(g, SensMath.Name(g))).ToList();

        _loading = true;
        var s = _settings.Current;
        _dpiText = Math.Clamp(s.SensDpi, 100, 32000).ToString(CultureInfo.InvariantCulture);
        _selectedGame = Games.First(g => g.Game == s.SensGame);
        LoadProfile(s.SensGame);
        _loading = false;

        Loc.LanguageChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(CurrentCm360Text));
            OnPropertyChanged(nameof(FovHint));
            RenderResult();
        };
    }

    /// <summary>The player's own values for this game, or sensible ones until there are any:
    /// the game's usual field of view and the resolution of the primary monitor.</summary>
    private void LoadProfile(SensGame game)
    {
        var p = _settings.Current.SensProfiles.GetValueOrDefault(game.ToString());
        var monitor = MonitorList.Read().FirstOrDefault();
        SensText = FormatInput(p is { Sens: > 0 } ? p.Sens : DefaultSens(game));
        FovText = FormatInput(p is { Fov: > 0 } ? p.Fov : DefaultFov(game));
        ResWidthText = (p is { ResWidth: > 0 } ? p.ResWidth : monitor?.Width ?? 1920).ToString(CultureInfo.InvariantCulture);
        ResHeightText = (p is { ResHeight: > 0 } ? p.ResHeight : monitor?.Height ?? 1080).ToString(CultureInfo.InvariantCulture);
        Stretch = p?.Stretch ?? false;
    }

    private static string FormatInput(double value) =>
        value.ToString("0.####", CultureInfo.InvariantCulture);

    partial void OnSelectedGameChanged(SensGameChoice? value)
    {
        if (_loading || value is null) return;
        // Each game has its own scale, view and resolution, so each keeps its own values.
        _loading = true;
        LoadProfile(value.Game);
        _loading = false;
        UsedMessage = null;
        StatusMessage = null;
    }

    // ---------------------------------------------------------------- input

    private bool TryReadDpi(out double dpi)
    {
        dpi = 0;
        if (!TryNumber(DpiText, out var v) || v < 100 || v > 32000) return false;
        dpi = v;
        return true;
    }

    private bool TryReadSens(out double sens)
    {
        sens = 0;
        if (!TryNumber(SensText, out var v) || v <= 0 || v > 1000) return false;
        sens = v;
        return true;
    }

    private bool TryReadFov(out double fov)
    {
        fov = 0;
        if (!TryNumber(FovText, out var v) || v < 40 || v > 160) return false;
        fov = v;
        return true;
    }

    private bool TryReadResolution(out int width, out int height)
    {
        width = height = 0;
        if (!int.TryParse(ResWidthText.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var w) || w < 640 || w > 7680) return false;
        if (!int.TryParse(ResHeightText.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var h) || h < 480 || h > 4320) return false;
        // The shape matters, not the size: anything from 1:1 to 4:1 is a picture a game could have.
        if ((double)w / h is < 1.0 or > 4.0) return false;
        width = w; height = h;
        return true;
    }

    // "1,25" and "1.25" both mean 1.25, whatever the Windows region says.
    private static bool TryNumber(string? text, out double value) =>
        double.TryParse((text ?? "").Trim().Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out value);

    // ----------------------------------------------------------------- test

    [RelayCommand]
    private void StartTest()
    {
        StatusMessage = null;
        UsedMessage = null;
        if (SelectedGame is null) return;
        if (!TryReadDpi(out var dpi)) { StatusMessage = Loc.T("Sens.InvalidDpi"); return; }
        if (!TryReadSens(out var sens)) { StatusMessage = Loc.T("Sens.InvalidSens"); return; }
        if (!TryReadFov(out var fov)) { StatusMessage = Loc.T("Sens.InvalidFov"); return; }
        if (!TryReadResolution(out var resW, out var resH)) { StatusMessage = Loc.T("Sens.InvalidRes"); return; }

        var game = SelectedGame.Game;
        var s = _settings.Current;
        s.SensGame = game;
        s.SensDpi = (int)Math.Round(dpi);
        s.SensProfiles[game.ToString()] = new SensGameProfile
        {
            Sens = sens, Fov = fov, ResWidth = resW, ResHeight = resH, Stretch = Stretch
        };
        _settings.Save();

        // The rounds come in a random order, so neither tiredness nor warming up favours one speed.
        var rng = new Random();
        var order = Multipliers.OrderBy(_ => rng.Next()).ToList();
        var centre = SensMath.Cm360(game, dpi, sens);

        var window = new SensTestWindow(dpi, centre, order, fov, resW, resH, Stretch);
        window.ShowDialog();

        if (!window.Completed) { StatusMessage = Loc.T("Sens.Aborted"); return; }

        var rec = SensAnalyzer.Analyze(window.Session.Results);
        if (rec is null) { StatusMessage = Loc.T("Sens.Aborted"); return; }

        _rec = rec;
        _results = window.Session.Results.ToList();
        _testGame = game;
        _testDpi = dpi;
        _testSens = sens;
        RenderResult();
    }

    [RelayCommand]
    private void UseResult()
    {
        if (_rec is null) return;
        var sens = _testSens * _rec.Multiplier;
        // Typed in the way the game takes it, so what is shown here is what the player enters there.
        SensText = FormatInput(Math.Round(sens, SensMath.Decimals(_testGame)));
        if (_settings.Current.SensProfiles.TryGetValue(_testGame.ToString(), out var profile))
            profile.Sens = double.Parse(SensText, CultureInfo.InvariantCulture);
        _settings.Save();
        UsedMessage = Loc.T("Sens.Result.Used");
    }

    // --------------------------------------------------------------- result

    private void RenderResult()
    {
        Rounds.Clear();
        OtherGames.Clear();
        if (_rec is null) { HasResult = false; return; }

        var rec = _rec;
        var game = _testGame;
        var centre = SensMath.Cm360(game, _testDpi, _testSens);
        var sensNow = _testSens;
        var best = sensNow * rec.Multiplier;
        var cm360 = centre / rec.Multiplier;

        ResultSens = SensMath.Format(game, best);
        ResultCm360 = Loc.F("Sens.Cm360", cm360.ToString("0.0", CultureInfo.CurrentCulture));
        ResultRange = Loc.F("Sens.Result.Range",
            SensMath.Format(game, sensNow * rec.LowMultiplier), SensMath.Format(game, sensNow * rec.HighMultiplier));

        var percent = (int)Math.Round(Math.Abs(rec.Multiplier - 1.0) * 100);
        ResultChange = percent < 3 ? Loc.T("Sens.Result.Same")
            : Loc.F(rec.Multiplier > 1 ? "Sens.Result.Faster" : "Sens.Result.Slower", percent);

        (ConfidenceText, ConfidenceLevel) = rec.NoClearDifference
            ? (Loc.T("Sens.Conf.None"), 0)
            : rec.Confidence switch
            {
                SensConfidence.High => (Loc.T("Sens.Conf.High"), 3),
                SensConfidence.Medium => (Loc.T("Sens.Conf.Medium"), 2),
                _ => (Loc.T("Sens.Conf.Low"), 1)
            };

        ResultNotice =
            rec.NoClearDifference ? Loc.T("Sens.Result.NoDiff") :
            rec.AtEdge ? Loc.T("Sens.Result.Edge") :
            rec.Confidence == SensConfidence.Low ? Loc.T("Sens.Result.Unsure") :
            null;

        GameNote = game switch
        {
            SensGame.ApexLegends => Loc.T("Sens.Note.Apex"),
            SensGame.Fortnite => Loc.T("Sens.Note.Fortnite"),
            _ => null
        };

        // The table: every round, from slowest to fastest, with its score as a bar.
        var scored = _results.Where(r => r.Scored).OrderBy(r => r.Multiplier).ToList();
        var points = rec.Points;
        var lo = points.Count == 0 ? 0 : points.Min(p => p.Score);
        var hi = points.Count == 0 ? 0 : points.Max(p => p.Score);
        for (var i = 0; i < scored.Count; i++)
        {
            var r = scored[i];
            var score = i < points.Count ? points[i].Score : 0;
            var bar = rec.NoClearDifference || hi - lo < 1e-9 ? 0.5 : 0.12 + 0.88 * (score - lo) / (hi - lo);
            Rounds.Add(new SensRoundRow
            {
                Sens = SensMath.Format(game, sensNow * r.Multiplier),
                ScoreBar = bar,
                IsTop = !rec.NoClearDifference && hi - lo >= 1e-9 && score >= hi - 1e-9,
                Hits = Loc.F("Sens.Row.Hits", r.Hits, r.Hits + r.Misses),
                Time = r.Hits == 0 ? "-" : Loc.F("Sens.Row.Time", (int)Math.Round(r.MeanTimeToHitMs)),
                Track = Loc.F("Sens.Row.Track", (int)Math.Round(r.TrackFraction * 100))
            });
        }

        // The same turn in every game (the one being tested is the headline already).
        foreach (var other in Enum.GetValues<SensGame>().Where(g => g != game))
        {
            OtherGames.Add(new SensGameRow
            {
                Game = SensMath.Name(other),
                Value = SensMath.Format(other, SensMath.SensitivityFor(other, _testDpi, cm360))
            });
        }

        HasResult = true;
    }
}
