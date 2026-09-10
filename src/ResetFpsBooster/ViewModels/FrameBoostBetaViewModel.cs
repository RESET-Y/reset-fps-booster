#if RFB_BETA
using System.Globalization;
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

    /// Set when the game runs faster than half the refresh rate. The generated
    /// frames are real, but the monitor has no window left to show them - and
    /// saying nothing would leave an output number on screen that the display
    /// never actually reaches.
    public string? DisplayLimitNotice
    {
        get
        {
            if (Telemetry.NativeFps is not > 1 || Telemetry.DisplayHz is not > 1) return null;
            double doubled = Telemetry.NativeFps.Value * 2;
            if (doubled <= Telemetry.DisplayHz.Value) return null;

            return string.Create(CultureInfo.InvariantCulture,
                $"The game runs at {Telemetry.NativeFps:0} FPS. Doubled that is {doubled:0}, "
                + $"but this display shows {Telemetry.DisplayHz:0} per second - so generation is off. "
                + $"Cap the game at {Telemetry.DisplayHz.Value / 2:0} FPS or below and every generated frame reaches the screen.");
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
            OnPropertyChanged(nameof(DisplayLimitNotice));
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
