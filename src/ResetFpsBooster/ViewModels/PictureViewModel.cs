using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Picture;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

/// <summary>A preset on the page, and whether the picture currently comes from it.</summary>
public sealed partial class PresetItem : ObservableObject
{
    public PresetItem(PicturePreset preset, bool isActive)
    {
        Preset = preset;
        _isActive = isActive;
    }

    public PicturePreset Preset { get; }

    [ObservableProperty] private bool _isActive;
}

/// <summary>A library program on the picture page: whether it pauses the picture while it runs.</summary>
public sealed partial class PictureGameItem : ObservableObject
{
    private readonly Action<PictureGameItem> _changed;

    public PictureGameItem(string name, bool pauses, Action<PictureGameItem> changed)
    {
        Name = name;
        _pauses = pauses;
        _changed = changed;
    }

    public string Name { get; }

    [ObservableProperty] private bool _pauses;

    partial void OnPausesChanged(bool value) => _changed(this);
}

/// <summary>
/// PICTURE: saturation, brightness and contrast for the screen, with presets. Each slider is applied
/// at once, unless auto-off has paused the picture because a game is running.
/// </summary>
public sealed partial class PictureViewModel : ViewModelBase
{
    private readonly IPictureService _picture;
    private readonly PictureWatcher _watch;
    private readonly IGameLibraryService _library;
    private readonly ISettingsService _settings;
    private bool _loading;

    public ObservableCollection<PresetItem> PresetItems { get; } = new();
    public ObservableCollection<PresetItem> CustomItems { get; } = new();
    public ObservableCollection<PictureGameItem> Games { get; } = new();

    [ObservableProperty] private int _saturation;
    [ObservableProperty] private int _brightness;
    [ObservableProperty] private int _contrast;
    [ObservableProperty] private bool _autoOff;
    [ObservableProperty] private string? _activePresetId;
    [ObservableProperty] private string _newPresetName = "";
    [ObservableProperty] private string _pictureStatus = "";
    [ObservableProperty] private string? _messageText;
    [ObservableProperty] private bool _saturationAvailable;
    [ObservableProperty] private bool _hasGames;
    [ObservableProperty] private bool _gammaAvailable;

    public PictureViewModel(IPictureService picture, PictureWatcher watch, IGameLibraryService library, ISettingsService settings)
    {
        _picture = picture;
        _watch = watch;
        _library = library;
        _settings = settings;

        _loading = true;
        var current = _settings.Current.PictureCurrent;
        _saturation = current.Saturation;
        _brightness = current.Brightness;
        _contrast = current.Contrast;
        _autoOff = _settings.Current.PictureAutoOff;
        _activePresetId = _settings.Current.PictureActivePresetId;
        _loading = false;

        _watch.Changed += (_, _) => RefreshStatus();
        Loc.LanguageChanged += (_, _) =>
        {
            RebuildPresets();
            RefreshStatus();
        };
        Refresh();
    }

    /// <summary>Rebuilds the lists from the settings; run whenever the page is opened.</summary>
    public void Refresh()
    {
        RebuildPresets();
        RebuildGames();
        SaturationAvailable = _picture.SaturationAvailable;
        GammaAvailable = _picture.GammaAvailable;
        RefreshStatus();
    }

    // ---------------------------------------------------------------- presets

    private static PicturePreset Built(string id, string name, int saturation, int brightness, int contrast) => new()
    {
        Id = id,
        Name = name,
        IsBuiltIn = true,
        Saturation = saturation,
        Brightness = brightness,
        Contrast = contrast
    };

    private static List<PicturePreset> BuiltIn() => new()
    {
        Built("builtin-standard", Loc.T("Picture.Preset.Standard"), 0, 0, 0),
        Built("builtin-vivid", Loc.T("Picture.Preset.Vivid"), 30, 0, 10),
        Built("builtin-strong", Loc.T("Picture.Preset.Strong"), 60, 5, 20),
        Built("builtin-max", Loc.T("Picture.Preset.Max"), 100, 10, 25),
        Built("builtin-clear", Loc.T("Picture.Preset.Clear"), 15, 15, 20),
    };

    private void RebuildPresets()
    {
        PresetItems.Clear();
        foreach (var p in BuiltIn()) PresetItems.Add(new PresetItem(p, p.Id == ActivePresetId));

        CustomItems.Clear();
        foreach (var p in _settings.Current.PictureCustomPresets) CustomItems.Add(new PresetItem(p, p.Id == ActivePresetId));
    }

    partial void OnActivePresetIdChanged(string? value)
    {
        foreach (var item in PresetItems.Concat(CustomItems)) item.IsActive = item.Preset.Id == value;
    }

    [RelayCommand]
    private void ApplyPreset(PicturePreset preset)
    {
        _loading = true;
        Saturation = preset.Saturation;
        Brightness = preset.Brightness;
        Contrast = preset.Contrast;
        _loading = false;

        _settings.Current.PictureCurrent = new PicturePreset
        {
            Saturation = preset.Saturation,
            Brightness = preset.Brightness,
            Contrast = preset.Contrast
        };
        _settings.Current.PictureActivePresetId = preset.Id;
        _settings.Save();
        ActivePresetId = preset.Id;
        ApplyNow();
    }

    [RelayCommand]
    private void SavePreset()
    {
        var name = NewPresetName.Trim();
        if (name.Length == 0)
        {
            MessageText = Loc.T("Picture.NameMissing");
            return;
        }

        var preset = new PicturePreset
        {
            Name = name,
            Saturation = Saturation,
            Brightness = Brightness,
            Contrast = Contrast
        };
        _settings.Current.PictureCustomPresets.Add(preset);
        _settings.Current.PictureActivePresetId = preset.Id;
        _settings.Save();

        NewPresetName = "";
        MessageText = null;
        ActivePresetId = preset.Id;
        RebuildPresets();
    }

    [RelayCommand]
    private void DeletePreset(PicturePreset preset)
    {
        if (preset.IsBuiltIn) return;

        _settings.Current.PictureCustomPresets.RemoveAll(p => p.Id == preset.Id);
        if (ActivePresetId == preset.Id)
        {
            _settings.Current.PictureActivePresetId = null;
            ActivePresetId = null;
        }
        _settings.Save();
        RebuildPresets();
    }

    [RelayCommand]
    private void Reset() => ApplyPreset(BuiltIn()[0]);

    // ------------------------------------------------------------- sliders

    partial void OnSaturationChanged(int value) => OnPictureChanged();
    partial void OnBrightnessChanged(int value) => OnPictureChanged();
    partial void OnContrastChanged(int value) => OnPictureChanged();

    private void OnPictureChanged()
    {
        if (_loading) return;

        _settings.Current.PictureCurrent = new PicturePreset
        {
            Saturation = Saturation,
            Brightness = Brightness,
            Contrast = Contrast
        };
        _settings.Current.PictureActivePresetId = null;
        _settings.Save();
        ActivePresetId = null;
        ApplyNow();
    }

    partial void OnAutoOffChanged(bool value)
    {
        if (_loading) return;

        _settings.Current.PictureAutoOff = value;
        _settings.Save();
        ApplyNow();
    }

    // Shows the picture now, unless a game pauses it; the watcher brings it back when the game ends.
    private void ApplyNow()
    {
        MessageText = null;
        if (AutoOff && _watch.PausingGame is not null)
        {
            RefreshStatus();
            return;
        }

        var result = _picture.Apply(_settings.Current.PictureCurrent);
        MessageText = result switch
        {
            PictureResult.WindowsRefused => Loc.T("Picture.Refused"),
            PictureResult.GammaUnavailable => Loc.T("Picture.GammaUnavailable"),
            PictureResult.Failed => Loc.T("Picture.Failed"),
            _ => null
        };
        RefreshStatus();
    }

    // ---------------------------------------------------------------- games

    private void RebuildGames()
    {
        Games.Clear();
        var ignored = _settings.Current.PictureIgnoredGames;
        var names = _library.GetGames()
            .Select(g => g.Name)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(n => n, StringComparer.OrdinalIgnoreCase);
        foreach (var name in names)
            Games.Add(new PictureGameItem(name, !ignored.Contains(name, StringComparer.OrdinalIgnoreCase), OnGameToggled));
        HasGames = Games.Count > 0;
    }

    private void OnGameToggled(PictureGameItem item)
    {
        var ignored = _settings.Current.PictureIgnoredGames;
        ignored.RemoveAll(n => n.Equals(item.Name, StringComparison.OrdinalIgnoreCase));
        if (!item.Pauses) ignored.Add(item.Name);
        _settings.Save();
    }

    // ---------------------------------------------------------------- status

    private void RefreshStatus()
    {
        if (AutoOff && _watch.PausingGame is not null)
            PictureStatus = Loc.F("Picture.Status.Paused", _watch.PausingGame);
        else if (_picture.IsApplied)
            PictureStatus = Loc.T("Picture.Status.Active");
        else
            PictureStatus = Loc.T("Picture.Status.Standard");

        if (!_picture.SaturationAvailable)
            PictureStatus += " " + Loc.T("Picture.Status.NoDriver");
        if (!_picture.GammaAvailable)
            PictureStatus += " " + Loc.T("Picture.GammaUnavailable");
    }
}
