#if RFB_BETA
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

/// SMOOTH MOTION, its own page: an ON/OFF button and a strength slider.
///
/// Motion-aware blur during fast movement, over a whole monitor or, if
/// picked, one game window only. The engine still adapts it every frame to how fast the picture
/// moves, the frame rate and the refresh rate; the slider scales that, and it
/// works live, without restarting anything. The whole screen is treated the
/// same - no still-pixel exception, which left sharp islands that looked odd.
///
/// It runs on the same engine as FrameBoost - the only way to change what is
/// shown without touching a game. On its own it covers the chosen monitor;
/// with FrameBoost on, one engine does both on FrameBoost's game window.
public sealed partial class SmoothMotionViewModel : ViewModelBase
{
    private readonly IFrameBoostBetaService _service;
    private readonly ISettingsService _settings;
    private readonly IAuthService _auth;
    private readonly Action _openAccount;
    private readonly DispatcherTimer _watch;

    [ObservableProperty] private bool _isOn;
    [ObservableProperty] private string? _statusMessage;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLocked))]
    private bool _isPremium;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLocked))]
    private bool _premiumChecked;

    public bool IsLocked => PremiumChecked && !IsPremium;

    public bool FrameBoostRunning => _service.FrameBoostOn;
    public string ModeText => Loc.T(FrameBoostRunning ? "Smooth.WithFrameBoost"
                                    : SelectedScreen?.IsWindow == true ? "Smooth.AloneWindow" : "Smooth.Alone");

    // ---- strength ----
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StrengthText))]
    private int _strength;

    public string StrengthText => $"{Strength} %";

    partial void OnStrengthChanged(int value)
    {
        _settings.Current.SmoothMotionStrength = value;
        _settings.Save();
        // Picked up by a running engine within half a second.
        SmoothMotionScreens.WriteStrength(value);
    }

    // ---- what to cover: a monitor, or one game window ----
    public List<SmoothScreen> Screens { get; private set; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(WindowPicked))]
    [NotifyPropertyChangedFor(nameof(ScreenPicked))]
    private SmoothScreen? _selectedScreen;

    public bool WindowPicked => SelectedScreen?.IsWindow == true;
    public bool ScreenPicked => !WindowPicked;

    partial void OnSelectedScreenChanged(SmoothScreen? value)
    {
        OnPropertyChanged(nameof(ModeText));
        if (value is null || _reading) return;
        // A window handle does not survive a restart; the game's program name
        // does, and picks its window again next time.
        if (value.IsWindow) _settings.Current.SmoothMotionLastProcess = value.Process;
        else { _settings.Current.SmoothMotionScreen = value.Index; _settings.Current.SmoothMotionLastProcess = ""; }
        _settings.Save();
    }

    public SmoothMotionViewModel(IFrameBoostBetaService service, ISettingsService settings,
                                 IAuthService auth, Action openAccount)
    {
        _service = service;
        _settings = settings;
        _auth = auth;
        _openAccount = openAccount;
        _strength = Math.Clamp(settings.Current.SmoothMotionStrength, 0, 100);

        _service.StateChanged += (_, _) => SyncFromService();
        _auth.SignInStateChanged += (_, _) => _ = CheckPremiumAsync();
        Loc.LanguageChanged += (_, _) => { OnPropertyChanged(nameof(ModeText)); ReadScreens(); };

        // The engine can exit on its own. Checked once a second so the button
        // never claims an effect that is not there.
        _watch = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _watch.Tick += (_, _) =>
        {
            if (IsOn && !_service.SmoothMotionOn)
            {
                IsOn = false;
                StatusMessage = Loc.T("Smooth.Stopped") + (_service.LastExitReason is { } why ? " (" + why + ")" : "");
            }
        };
        _watch.Start();

        _ = CheckPremiumAsync();
        ReadScreens();
        SyncFromService();
    }

    private bool _reading;

    // GAME WINDOW FIRST. Measured in Apex on 2026-09-30: the whole screen
    // repeated ~49 frames a second (the desktop runs at 144, the game at ~98)
    // and the blur switched off on every repeat - it read as stutter. The game
    // window had none. So windows head the list and the last game is picked
    // by itself; a whole screen stays available for games that cannot be
    // captured as a window.
    public void ReadScreens()
    {
        var keep = SelectedScreen;
        var list = FrameBoostBetaWindowList.Read()
            .Select(w => new SmoothScreen(0, Loc.F("Smooth.WindowItem", w.Title, w.ProcessName), w.Handle, w.ProcessName))
            .ToList();
        list.AddRange(SmoothMotionScreens.Read((n, primary, w, h) =>
            primary ? Loc.F("Smooth.ScreenPrimary", n, w, h) : Loc.F("Smooth.Screen", n, w, h)));
        Screens = list;
        OnPropertyChanged(nameof(Screens));

        var last = _settings.Current.SmoothMotionLastProcess ?? _settings.Current.FrameBoostLastProcess;
        SmoothScreen? byGame = string.IsNullOrEmpty(last) ? null
            : Screens.FirstOrDefault(s => s.IsWindow && string.Equals(s.Process, last, StringComparison.OrdinalIgnoreCase));
        _reading = true;
        SelectedScreen = (keep is { IsWindow: true } ? Screens.FirstOrDefault(s => s.Window == keep.Window) : null)
                         ?? (keep is { IsWindow: false } ? Screens.FirstOrDefault(s => !s.IsWindow && s.Index == keep.Index) : null)
                         ?? byGame
                         ?? Screens.FirstOrDefault(s => !s.IsWindow && s.Index == _settings.Current.SmoothMotionScreen)
                         ?? Screens.FirstOrDefault(s => !s.IsWindow);
        _reading = false;
    }

    private void SyncFromService()
    {
        IsOn = _service.SmoothMotionOn;
        OnPropertyChanged(nameof(FrameBoostRunning));
        OnPropertyChanged(nameof(ModeText));
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
        if (!IsPremium) { StatusMessage = Loc.T("Smooth.PremiumDot"); return; }

        // A picked game window may have closed since the list was read.
        if (SelectedScreen is { IsWindow: true } picked && !FrameBoostBetaWindowList.StillExists(picked.Window))
        {
            ReadScreens();
            StatusMessage = Loc.T("Smooth.WindowGone");
            return;
        }

        StatusMessage = _service.SetSmoothMotion(true, SelectedScreen?.Index ?? 0, SelectedScreen?.Window ?? 0, Strength);
        SyncFromService();
    }

    [RelayCommand]
    private void TurnOff()
    {
        _service.SetSmoothMotion(false, SelectedScreen?.Index ?? 0, SelectedScreen?.Window ?? 0, Strength);
        StatusMessage = null;
        SyncFromService();
    }
}
#endif
