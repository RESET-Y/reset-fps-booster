using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Win32;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed record CrosshairShapeChoice(CrosshairShape Shape, string Label);
public sealed record CrosshairScreenChoice(int Index, string Label);

/// CROSSHAIR, a premium page: pick a shape or an own image, tune it, switch
/// it on. Every change is saved at once and redrawn live while it is shown.
/// The overlay itself is CrosshairService - an ordinary click-through window,
/// nothing that touches a game.
public sealed partial class CrosshairViewModel : ViewModelBase
{
    private readonly ICrosshairService _crosshair;
    private readonly ISettingsService _settings;
    private readonly IAuthService _auth;
    private readonly Action _openAccount;
    private bool _loading;

    private CrosshairStyle Style => _settings.Current.Crosshair;

    [ObservableProperty] private bool _isOn;
    [ObservableProperty] private string? _statusMessage;
    [ObservableProperty] private ImageSource? _preview;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLocked))]
    private bool _isPremium;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLocked))]
    private bool _premiumChecked;

    public bool IsLocked => PremiumChecked && !IsPremium;

    // ---- look ----
    public List<CrosshairShapeChoice> Shapes { get; private set; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsImage))]
    [NotifyPropertyChangedFor(nameof(IsDrawn))]
    [NotifyPropertyChangedFor(nameof(HasArms))]
    private CrosshairShapeChoice? _selectedShape;

    public bool IsImage => SelectedShape?.Shape == CrosshairShape.Image;
    public bool IsDrawn => !IsImage;
    public bool HasArms => SelectedShape?.Shape is not (CrosshairShape.Dot or CrosshairShape.Image);

    [ObservableProperty] private string _color = "#00FF66";
    [ObservableProperty] private int _length;
    [ObservableProperty] private int _thickness;
    [ObservableProperty] private int _gap;
    [ObservableProperty] private bool _outline;
    [ObservableProperty] private int _outlineThickness;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PreviewOpacity))]
    private int _opacity;

    public double PreviewOpacity => Math.Clamp(Opacity, 10, 100) / 100.0;
    [ObservableProperty] private int _imageScale;
    [ObservableProperty] private string? _imageName;

    public IReadOnlyList<string> Swatches { get; } = new[]
    {
        "#00FF66", "#00FFFF", "#FFFF00", "#FF00FF", "#FF2D2D", "#FFFFFF"
    };

    // ---- where ----
    public List<CrosshairScreenChoice> Screens { get; private set; } = new();
    public bool HasSeveralScreens => Screens.Count > 1;
    [ObservableProperty] private CrosshairScreenChoice? _selectedScreen;

    public CrosshairViewModel(ICrosshairService crosshair, ISettingsService settings, IAuthService auth, Action openAccount)
    {
        _crosshair = crosshair;
        _settings = settings;
        _auth = auth;
        _openAccount = openAccount;

        _loading = true;
        var s = Style;
        _color = s.Color;
        _length = s.Length;
        _thickness = s.Thickness;
        _gap = s.Gap;
        _outline = s.Outline;
        _outlineThickness = s.OutlineThickness;
        _opacity = s.Opacity;
        _imageScale = s.ImageScale;
        _imageName = string.IsNullOrEmpty(s.ImagePath) ? null : Path.GetFileName(s.ImagePath);
        ReadLists();
        _loading = false;

        _crosshair.StateChanged += (_, _) => IsOn = _crosshair.IsShown;
        _auth.SignInStateChanged += (_, _) => _ = CheckPremiumAsync();
        Loc.LanguageChanged += (_, _) => { _loading = true; ReadLists(); _loading = false; };

        IsOn = _crosshair.IsShown;
        Redraw();
        _ = CheckPremiumAsync();
    }

    public void ReadLists()
    {
        Shapes = new List<CrosshairShapeChoice>
        {
            new(CrosshairShape.Cross, Loc.T("Cross.ShapeCross")),
            new(CrosshairShape.CrossDot, Loc.T("Cross.ShapeCrossDot")),
            new(CrosshairShape.Dot, Loc.T("Cross.ShapeDot")),
            new(CrosshairShape.Circle, Loc.T("Cross.ShapeCircle")),
            new(CrosshairShape.CircleDot, Loc.T("Cross.ShapeCircleDot")),
            new(CrosshairShape.TShape, Loc.T("Cross.ShapeT")),
            new(CrosshairShape.Image, Loc.T("Cross.ShapeImage")),
        };
        OnPropertyChanged(nameof(Shapes));
        SelectedShape = Shapes.FirstOrDefault(x => x.Shape == Style.Shape) ?? Shapes[0];

        Screens = MonitorList.Read().Select((m, i) => new CrosshairScreenChoice(i,
            m.Primary ? Loc.F("Smooth.ScreenPrimary", i + 1, m.Width, m.Height) : Loc.F("Smooth.Screen", i + 1, m.Width, m.Height))).ToList();
        OnPropertyChanged(nameof(Screens));
        OnPropertyChanged(nameof(HasSeveralScreens));
        SelectedScreen = Screens.FirstOrDefault(x => x.Index == Style.Screen) ?? Screens.FirstOrDefault();
    }

    // Every setting lands here: store it, save, redraw the preview, and the
    // overlay too if it is up.
    private void Apply(Action<CrosshairStyle> change)
    {
        if (_loading) return;
        change(Style);
        _settings.Save();
        Redraw();
        if (_crosshair.IsShown && !_crosshair.Show(Style))
            StatusMessage = Loc.T("Cross.ImageUnreadable");
    }

    private void Redraw()
    {
        Preview = CrosshairRenderer.Render(Style);
        if (Preview is null && IsImage)
            StatusMessage = Style.ImagePath is null ? Loc.T("Cross.PickImage") : Loc.T("Cross.ImageUnreadable");
        else
            StatusMessage = null;
    }

    partial void OnSelectedShapeChanged(CrosshairShapeChoice? value) { if (value is not null) Apply(s => s.Shape = value.Shape); }
    partial void OnSelectedScreenChanged(CrosshairScreenChoice? value) { if (value is not null) Apply(s => s.Screen = value.Index); }
    partial void OnColorChanged(string value)
    {
        // Typed by hand: only take it once it is a real colour.
        if (value.Length is 7 && value.StartsWith('#') && int.TryParse(value[1..], System.Globalization.NumberStyles.HexNumber, null, out _))
            Apply(s => s.Color = value.ToUpperInvariant());
    }
    partial void OnLengthChanged(int value) => Apply(s => s.Length = value);
    partial void OnThicknessChanged(int value) => Apply(s => s.Thickness = value);
    partial void OnGapChanged(int value) => Apply(s => s.Gap = value);
    partial void OnOutlineChanged(bool value) => Apply(s => s.Outline = value);
    partial void OnOutlineThicknessChanged(int value) => Apply(s => s.OutlineThickness = value);
    partial void OnOpacityChanged(int value) => Apply(s => s.Opacity = value);
    partial void OnImageScaleChanged(int value) => Apply(s => s.ImageScale = value);

    [RelayCommand]
    private void PickColor(string hex) => Color = hex;

    [RelayCommand]
    private void PickImage()
    {
        var dialog = new OpenFileDialog
        {
            Title = Loc.T("Cross.PickImageTitle"),
            Filter = "PNG (*.png)|*.png",
        };
        if (dialog.ShowDialog() != true) return;

        // Copied next to the settings, so moving or deleting the original
        // does not take the crosshair with it.
        string target;
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ResetFpsBooster", "Crosshairs");
            Directory.CreateDirectory(dir);
            target = Path.Combine(dir, Path.GetFileName(dialog.FileName));
            if (!string.Equals(Path.GetFullPath(dialog.FileName), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase))
                File.Copy(dialog.FileName, target, overwrite: true);
        }
        catch
        {
            target = dialog.FileName;
        }

        ImageName = Path.GetFileName(target);
        Apply(s => { s.ImagePath = target; s.Shape = CrosshairShape.Image; });
        _loading = true;
        SelectedShape = Shapes.First(x => x.Shape == CrosshairShape.Image);
        _loading = false;
    }

    private async Task CheckPremiumAsync()
    {
        IsPremium = await _auth.IsPremiumAsync();
        PremiumChecked = true;
    }

    [RelayCommand]
    private void OpenAccount() => _openAccount();

    [RelayCommand]
    private async Task TurnOnAsync()
    {
        await CheckPremiumAsync();
        if (!IsPremium) { StatusMessage = Loc.T("Cross.PremiumDot"); return; }

        if (!_crosshair.Show(Style))
        {
            StatusMessage = IsImage ? Loc.T("Cross.PickImage") : Loc.T("Cross.ImageUnreadable");
            return;
        }
        _settings.Current.CrosshairEnabled = true;
        _settings.Save();
        IsOn = true;
    }

    [RelayCommand]
    private void TurnOff()
    {
        _crosshair.Hide();
        _settings.Current.CrosshairEnabled = false;
        _settings.Save();
        IsOn = false;
    }
}
