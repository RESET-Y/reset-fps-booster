using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Apex;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

/// <summary>One graphics option the page can switch off, with what the file says about it now.</summary>
public sealed partial class ApexToggleRow : ObservableObject
{
    private readonly string _key;
    public ApexToggleRow(string key)
    {
        _key = key;
        Loc.LanguageChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(Label));
            OnPropertyChanged(nameof(Hint));
            OnPropertyChanged(nameof(AlreadyText));
        };
    }

    public string Label => Loc.T("Apex." + _key);
    public string Hint => Loc.T("Apex." + _key + "Hint");
    public string AlreadyText => Loc.T("Apex.Already");

    /// <summary>Make sure this is off after applying.</summary>
    [ObservableProperty] private bool _isChecked;

    /// <summary>False when the option cannot be set on the chosen target (shadows in autoexec).</summary>
    [ObservableProperty] private bool _isEnabled = true;

    /// <summary>A line of explanation under the option; null when there is none.</summary>
    [ObservableProperty] private string? _note;

    /// <summary>The file has it off already.</summary>
    [ObservableProperty] private bool _already;
}

public sealed record ApexTargetChoice(ApexTarget Target, string Label);

/// <summary>One step of the texture streaming budget; a null value means "leave it as it is".</summary>
public sealed record ApexStreamChoice(int? Value, string Label);

/// <summary>
/// APEX CONFIG: switch off the graphics that cost frames (shadows first) and set
/// the field of view, by editing Apex Legends' own files - the same edit players
/// do by hand - either its settings files (videoconfig.txt, profile.cfg) or as
/// commands in autoexec.cfg. A backup comes first. The settings files need the game
/// closed; autoexec does not. The app never sets the read-only flag on its own: the
/// page starts from what the file has now, so a player who set it keeps it.
/// </summary>
public sealed partial class ApexConfigViewModel : ViewModelBase
{
    /// <summary>The top of the field-of-view slider in the game's own menu.</summary>
    public const double MenuMaxFov = 110;

    private readonly IApexSettingsService _apex;
    private readonly ISettingsService _settings;
    private bool _loading;

    // What the files had when the page last read them. The slider moves in whole degrees, so a
    // value like 108.5 is only kept if the slider is left where it was put.
    private double? _fovInFile;
    private double _fovSliderAtLoad;

    public ApexToggleRow Shadows { get; } = new("Shadows");
    public ApexToggleRow Ssao { get; } = new("Ssao");
    public ApexToggleRow Volumetrics { get; } = new("Volumetrics");
    public ApexToggleRow Ragdolls { get; } = new("Ragdolls");
    public ApexToggleRow Decals { get; } = new("Decals");
    public ObservableCollection<ApexToggleRow> Rows { get; }

    public IReadOnlyList<ApexTargetChoice> Targets { get; private set; } = Array.Empty<ApexTargetChoice>();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsAutoexec))]
    [NotifyPropertyChangedFor(nameof(IsSettingsFiles))]
    [NotifyPropertyChangedFor(nameof(FovHintText))]
    private ApexTargetChoice? _selectedTarget;

    public bool IsAutoexec => SelectedTarget?.Target == ApexTarget.Autoexec;
    public bool IsSettingsFiles => !IsAutoexec;

    public IReadOnlyList<ApexStreamChoice> Streams { get; private set; } = Array.Empty<ApexStreamChoice>();

    [ObservableProperty] private ApexStreamChoice? _selectedStream;
    [ObservableProperty] private string _streamNow = "";

    /// <summary>The budget is below the game's smallest step (textures stay blurry); null when it is fine.</summary>
    [ObservableProperty] private string? _streamWarnLowText;

    /// <summary>An autoexec.cfg sets another budget and wins at every start; null when none does.</summary>
    [ObservableProperty] private string? _streamWarnOverrideText;

    [ObservableProperty] private bool _found;
    [ObservableProperty] private bool _gameRunning;
    [ObservableProperty] private string _autoexecPathText = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FovReadout))]
    [NotifyPropertyChangedFor(nameof(FovAboveMenu))]
    private double _fov = 110;

    [ObservableProperty] private string _fovNow = "";
    [ObservableProperty] private bool _lockReadOnly;
    [ObservableProperty] private string? _okMessage;
    [ObservableProperty] private bool _hasSnapshot;

    public string FovReadout => Loc.F("Apex.FovValue", (int)Math.Round(Fov), ApexConfigText.FormatScale(Math.Round(Fov)));

    /// <summary>Beyond what the game's own menu offers: unofficial, and said so on the page.</summary>
    public bool FovAboveMenu => Math.Round(Fov) > MenuMaxFov;

    /// <summary>Where the field of view ends up, for the line under its heading.</summary>
    public string FovHintText => Loc.T(IsAutoexec ? "Apex.FovHintAutoexec" : "Apex.FovHint");

    /// <summary>What a missing file means on this target, for the notice at the top.</summary>
    public string NotFoundText => Loc.T(IsAutoexec ? "Apex.NotFoundAutoexec" : "Apex.NotFound");

    private ApexTarget Target => SelectedTarget?.Target ?? ApexTarget.SettingsFiles;

    public ApexConfigViewModel(IApexSettingsService apex, ISettingsService settings)
    {
        _apex = apex;
        _settings = settings;
        Rows = new ObservableCollection<ApexToggleRow> { Shadows, Ssao, Volumetrics, Ragdolls, Decals };

        _loading = true;
        ReadTargets();
        ReadStreams();
        _selectedTarget = Targets.First(t => t.Target == settings.Current.ApexTarget);
        _selectedStream = Streams[0];
        _loading = false;

        Loc.LanguageChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(FovReadout));
            OnPropertyChanged(nameof(NotFoundText));
            OnPropertyChanged(nameof(FovHintText));
            _loading = true;
            var current = Target;
            var stream = SelectedStream?.Value;
            ReadTargets();
            ReadStreams();
            SelectedTarget = Targets.First(t => t.Target == current);
            SelectedStream = Streams.FirstOrDefault(x => x.Value == stream) ?? Streams[0];
            _loading = false;
        };
        Refresh();
    }

    private void ReadTargets() => Targets = new[]
    {
        new ApexTargetChoice(ApexTarget.SettingsFiles, Loc.T("Apex.TargetFiles")),
        new ApexTargetChoice(ApexTarget.Autoexec, Loc.T("Apex.TargetAutoexec"))
    };

    private void ReadStreams()
    {
        var list = new List<ApexStreamChoice> { new(null, Loc.T("Apex.StreamKeep")) };
        list.AddRange(ApexStreaming.Tiers.Select(t => new ApexStreamChoice(t.Value, Loc.T("Apex.Stream" + t.Key))));
        Streams = list;
    }

    partial void OnSelectedTargetChanged(ApexTargetChoice? value)
    {
        OnPropertyChanged(nameof(NotFoundText));
        if (_loading || value is null) return;
        _settings.Current.ApexTarget = value.Target;
        _settings.Save();
        StatusMessage = null;
        OkMessage = null;
        Refresh();
    }

    private string? SnapshotId => Target == ApexTarget.Autoexec
        ? _settings.Current.ApexAutoexecSnapshotId
        : _settings.Current.ApexSnapshotId;

    /// <summary>Reads the files again; run whenever the page is opened.</summary>
    public void Refresh()
    {
        var s = _apex.Read(Target);
        Found = s.Found;
        GameRunning = s.GameRunning;
        HasSnapshot = !string.IsNullOrEmpty(SnapshotId);
        AutoexecPathText = s.AutoexecPath is { } p ? Loc.F("Apex.AutoexecPath", p) : "";
        if (!s.Found) return;

        // The settings files have one answer. For autoexec the page manages the app's own block:
        // a switch is on when the block sets it, and "already on" also counts lines of the player's own.
        void Set(ApexToggleRow row, bool already, bool managed) { row.Already = already; row.IsChecked = managed; }
        var m = s.Managed;
        Set(Shadows, s.ShadowsOff, m?.Shadows ?? s.ShadowsOff);
        Set(Ssao, s.AmbientOcclusionOff, m?.AmbientOcclusion ?? s.AmbientOcclusionOff);
        Set(Volumetrics, s.VolumetricsOff, m?.Volumetrics ?? s.VolumetricsOff);
        Set(Ragdolls, s.RagdollsOff, m?.Ragdolls ?? s.RagdollsOff);
        Set(Decals, s.DecalsOff, m?.Decals ?? s.DecalsOff);

        // Apex puts the shadows back after start-up when they come from autoexec, so they are only
        // offered for the settings files.
        Shadows.IsEnabled = !IsAutoexec;
        Shadows.Note = IsAutoexec ? Loc.T("Apex.ShadowsNoAutoexec") : null;
        if (IsAutoexec) { Shadows.IsChecked = false; Shadows.Already = false; }

        LockReadOnly = s.VideoConfigReadOnly;
        FovNow = s.Fov is { } f ? Loc.F("Apex.FovNow", f.ToString("0.#", CultureInfo.CurrentCulture)) : Loc.T("Apex.FovNowNone");
        _fovInFile = s.Fov;
        Fov = s.Fov is { } now ? Math.Clamp(Math.Round(now, MidpointRounding.AwayFromZero), 70, 120) : MenuMaxFov;
        _fovSliderAtLoad = Fov;

        // Texture streaming. The settings files show the step the file has, if it is one of the game's;
        // for autoexec the step shown is the one the app's own block sets (the budget the game ends up
        // with is only named below). Anything else reads "leave as is".
        StreamNow = s.StreamMemory is { } budget ? Loc.F("Apex.StreamNow", budget) : Loc.T("Apex.StreamNowNone");
        StreamWarnLowText = ApexStreaming.IsTooLow(s.StreamMemory) ? Loc.F("Apex.StreamWarnLow", s.StreamMemory!.Value) : null;
        // In the settings-files mode, an autoexec that sets its own budget wins at every start.
        StreamWarnOverrideText = !IsAutoexec && s.AutoexecStreamMemory is { } over && over != s.StreamMemory
            ? Loc.F("Apex.StreamWarnOverride", over)
            : null;
        var shown = IsAutoexec ? s.Managed?.StreamMemory : s.StreamMemory;
        SelectedStream = Streams.FirstOrDefault(x => x.Value is not null && x.Value == shown) ?? Streams[0];
    }

    [RelayCommand]
    private void Apply()
    {
        StatusMessage = null;
        OkMessage = null;

        var moved = Math.Round(Fov) != _fovSliderAtLoad;
        var choice = new ApexSettingsChoice(
            ShadowsOff: !IsAutoexec && Shadows.IsChecked,
            AmbientOcclusionOff: Ssao.IsChecked,
            VolumetricsOff: Volumetrics.IsChecked,
            RagdollsOff: Ragdolls.IsChecked,
            DecalsOff: Decals.IsChecked,
            Fov: _fovInFile is { } kept && !moved ? kept : Math.Round(Fov),
            LockReadOnly: LockReadOnly,
            FovChanged: moved,
            StreamMemory: SelectedStream?.Value);

        var target = Target;
        var result = _apex.Apply(choice, target);
        switch (result.Outcome)
        {
            case ApexApplyOutcome.Applied:
                if (target == ApexTarget.Autoexec) _settings.Current.ApexAutoexecSnapshotId = result.SnapshotId;
                else _settings.Current.ApexSnapshotId = result.SnapshotId;
                _settings.Save();
                Refresh();
                OkMessage = Loc.T(target == ApexTarget.Autoexec ? "Apex.AppliedAutoexec" : "Apex.Applied");
                break;
            case ApexApplyOutcome.Unchanged:
                OkMessage = Loc.T("Apex.Unchanged");
                break;
            case ApexApplyOutcome.NotFound:
                StatusMessage = NotFoundText;
                break;
            case ApexApplyOutcome.GameRunning:
                GameRunning = true;
                StatusMessage = Loc.T("Apex.GameRunning");
                break;
            default:
                StatusMessage = Loc.F("Apex.WriteFailed", result.Detail ?? "");
                break;
        }
    }

    [RelayCommand]
    private void Restore()
    {
        StatusMessage = null;
        OkMessage = null;
        var id = SnapshotId;
        if (string.IsNullOrEmpty(id)) return;

        var target = Target;
        var (success, message) = _apex.Restore(id, target);
        if (!success)
        {
            if (message == "running") { GameRunning = true; StatusMessage = Loc.T("Apex.GameRunning"); }
            else StatusMessage = Loc.F("Apex.RestoreFailed", message);
            return;
        }

        if (target == ApexTarget.Autoexec) _settings.Current.ApexAutoexecSnapshotId = null;
        else _settings.Current.ApexSnapshotId = null;
        _settings.Save();
        Refresh();
        OkMessage = Loc.T("Apex.Restored");
    }
}
