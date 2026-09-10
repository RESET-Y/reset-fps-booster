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

            // Nothing to add: the game already fills the display, so the engine
            // passes it through untouched. Worth saying plainly - the panel would
            // otherwise show an output number that is simply the game.s own.
            if (Telemetry.DoublingActive is 0 && Telemetry.SourceFps is > 1)
                return Telemetry.NoGpuRoom is 1
                    ? "This game is using all of your graphics card, so generating frames would slow it"
                      + " down instead of helping. FrameBoost is passing it through untouched and starts"
                      + " again by itself when there is room."
                    : "The game already fills your display, so there is nothing to double right now."
                      + " FrameBoost is passing it through untouched and starts doubling by itself"
                      + " as soon as the frame rate drops below half your refresh rate.";

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

    public FrameBoostBetaViewModel(IFrameBoostBetaService service)
    {
        _service = service;
    }

    [RelayCommand]
    public void StartCapture()
    {
        var error = _service.Start();
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
