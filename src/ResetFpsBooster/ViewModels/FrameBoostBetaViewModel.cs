#if RFB_BETA
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class FrameBoostBetaViewModel : ViewModelBase, IDisposable
{
    private readonly IFrameBoostBetaService _service;
    private DispatcherTimer? _telemetryTimer;

    [ObservableProperty] private bool _isRunning;
    [ObservableProperty] private string? _statusMessage;
    [ObservableProperty] private FrameBoostBetaTelemetry _telemetry = new();

    /// Picked BEFORE starting, because the engine reads it once at launch.
    /// Changing it while FrameBoost runs does nothing until the next start,
    /// which is why the toggle is disabled while it is running rather than
    /// silently ignored.
    [ObservableProperty] private bool _lowLatency;

    /// THE CAP TO RECOMMEND, from the refresh rate the user picks.
    ///
    /// FrameBoost doubles the game's frame rate, so the game should run at no
    /// more than half the refresh rate for the doubled output to fit the panel:
    /// 72 on 144 Hz, 60 on 120 Hz. It is also a GPU budget: generating one
    /// frame costs several milliseconds at 1440p and peaks near 10 ms under
    /// fast motion, and a game already holding the GPU at 100% leaves none.
    ///
    /// Picked from a list rather than read from Windows. Reading the primary
    /// display was wrong for anyone playing on the other of two monitors, and
    /// a recommendation built on the wrong number is worse than none.
    private readonly ISettingsService _settings;

    public IReadOnlyList<int> RefreshRates { get; } =
        new[] { 60, 75, 100, 120, 144, 165, 170, 180, 200, 240, 280, 360 };

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CapAdvice))]
    private int _displayHz;

    partial void OnDisplayHzChanged(int value)
    {
        _settings.Current.FrameBoostDisplayHz = value;
        _settings.Save();
    }

    public string CapAdvice =>
        $"Cap the game at {DisplayHz / 2} FPS. FrameBoost doubles it to {DisplayHz}, which is what a {DisplayHz} Hz monitor can show.";

    /// What the engine is doing right now, in the user.s terms. Three states
    /// read as "Native FPS: 0" on their own and mean completely different
    /// things: a still picture, a source that is too fast to double, and a
    /// capture that has stopped delivering. Only the last one is a fault.
    private int _noticeHoldTicks;
    private string? _shownNotice;

    /// The notice only appears after the condition has held for about a second,
    /// and disappears the same way. Without that it flickered on every second
    /// that happened to contain no new frames - a pause between rounds, a
    /// loading screen, a moment of standing still - which reads as a fault
    /// light blinking rather than as information.
    public string? StatusNotice
    {
        get
        {
            string? live = LiveNotice;
            if (live == _shownNotice) { _noticeHoldTicks = 0; return _shownNotice; }
            if (++_noticeHoldTicks < 3) return _shownNotice;
            _noticeHoldTicks = 0;
            _shownNotice = live;
            return _shownNotice;
        }
    }

    private string? LiveNotice
    {
        get
        {
            if (!IsRunning) return null;

            // Only one reason is worth interrupting for: the game needs the whole
            // graphics card, so generating would cost it frames. The other case -
            // a source too fast to double on this display - is no longer reported,
            // and no longer switched on and off, because a source hovering near
            // that threshold produced a notice that came and went constantly.
            if (Telemetry.NoGpuRoom is 1 && Telemetry.SourceFps is > 1)
                return "This game is using all of your graphics card, so generating frames would slow it"
                     + " down instead of helping. FrameBoost is passing it through untouched and starts"
                     + " again by itself when there is room.";

            bool noNewContent = (Telemetry.SourceFps ?? Telemetry.NativeFps) is null or < 1;
            if (!noNewContent) return null;

            // Frames still arrive, they are just identical - the game is
            // running, the picture is standing still. Nothing to double, and
            // nothing wrong.
            if (Telemetry.DuplicateFps is > 5)
                return "The picture is not changing right now, so there is nothing to double."
                     + " The game keeps running at its own frame rate; generation resumes by itself"
                     + " as soon as something moves.";

            return "No frames are arriving from the display. If this stays, turn FrameBoost off and on again.";
        }
    }

    /// FRAMEBOOST IS PREMIUM, and the answer comes from the server.
    ///
    /// IsPremium starts false and is filled in by asking Supabase, never read
    /// from anything local the user could edit. It is asked again whenever the
    /// sign-in state changes - the stored session is restored in the background
    /// at start-up, so a premium user who opens this page in the first second
    /// sees it unlock a moment later rather than stay locked.
    ///
    /// What this does NOT protect against, stated so nobody mistakes it for
    /// more: a check inside a desktop app can be patched out, and the engine
    /// executable can be started by hand. It keeps honest users honest. Real
    /// enforcement would need the engine itself to verify a signed token.
    private readonly IAuthService _auth;
    private readonly Action _openAccount;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLocked))]
    private bool _isPremium;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLocked))]
    private bool _premiumChecked;

    /// Locked only once the server has answered no. While the question is
    /// still out, neither the lock nor the buttons are shown - a paying user
    /// should not see a lock flash up for a second.
    public bool IsLocked => PremiumChecked && !IsPremium;
    public bool IsSignedIn => _auth.IsSignedIn;

    public FrameBoostBetaViewModel(IFrameBoostBetaService service, ISettingsService settings,
                                   IAuthService auth, Action openAccount)
    {
        _service = service;
        _settings = settings;
        _auth = auth;
        _openAccount = openAccount;
        _auth.SignInStateChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(IsSignedIn));
            _ = CheckPremiumAsync();
        };
        _ = CheckPremiumAsync();
        _displayHz = settings.Current.FrameBoostDisplayHz > 0 ? settings.Current.FrameBoostDisplayHz : 144;
    }

    private async Task CheckPremiumAsync()
    {
        IsPremium = await _auth.IsPremiumAsync();
        PremiumChecked = true;
    }

    [RelayCommand]
    private void OpenAccount() => _openAccount();

    [RelayCommand]
    public async Task StartCapture()
    {
        // Asked again at the moment it matters, not trusted from when the page
        // opened: premium can lapse, or be revoked, while the page sits open.
        await CheckPremiumAsync();
        if (!IsPremium)
        {
            StatusMessage = "FrameBoost is a Premium feature.";
            return;
        }

        var error = _service.Start(LowLatency);
        if (error is not null)
        {
            StatusMessage = error;
            IsRunning = false;
            return;
        }

        StatusMessage = null;
        IsRunning = true;

        _telemetryTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _telemetryTimer.Tick += (_, _) =>
        {
            Telemetry = _service.ReadLatestTelemetry();
            OnPropertyChanged(nameof(StatusNotice));
            if (!_service.IsRunning)
            {
                // The native engine exited on its own (failsafe path or a
                // genuine failure) - reflect that honestly instead of
                // pretending it's still running.
                StopCapture();
                StatusMessage = "FrameBoost stopped on its own (it hit a failsafe exit). Nothing was left running.";
            }
        };
        _telemetryTimer.Start();
    }

    [RelayCommand]
    public void StopCapture()
    {
        _telemetryTimer?.Stop();
        _telemetryTimer = null;
        _service.Stop();
        IsRunning = false;
        Telemetry = new FrameBoostBetaTelemetry();
    }

    public void Dispose() => StopCapture();
}
#endif
