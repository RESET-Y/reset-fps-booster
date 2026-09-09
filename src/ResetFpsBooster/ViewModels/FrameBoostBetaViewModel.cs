#if RFB_BETA
using System.Collections.ObjectModel;
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
    [ObservableProperty] private CaptureTargetWindow? _selectedWindow;
    [ObservableProperty] private FrameBoostBetaTelemetry _telemetry = new();

    public ObservableCollection<CaptureTargetWindow> AvailableWindows { get; } = new();

    public FrameBoostBetaViewModel(IFrameBoostBetaService service)
    {
        _service = service;
    }

    [RelayCommand]
    public void RefreshWindows()
    {
        AvailableWindows.Clear();
        foreach (var window in _service.EnumerateCandidateWindows())
            AvailableWindows.Add(window);
    }

    [RelayCommand]
    public void StartCapture()
    {
        if (SelectedWindow is null)
        {
            StatusMessage = "Select a window to capture first.";
            return;
        }

        var error = _service.Start(SelectedWindow);
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
            if (!_service.IsRunning)
            {
                // The native engine exited on its own (failsafe path, target
                // window closed, or a genuine failure) - reflect that
                // honestly instead of pretending it's still running.
                StopCapture();
                StatusMessage = "FrameBoost engine stopped (target window closed, or it hit a failsafe exit).";
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
