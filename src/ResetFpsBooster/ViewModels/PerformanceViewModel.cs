using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Monitoring;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class PerformanceViewModel : ViewModelBase, IDisposable
{
    private IPerformanceMonitorService? _monitor;
    private DispatcherTimer? _timer;

    [ObservableProperty] private bool _isMonitoring;
    [ObservableProperty] private PerformanceSample? _currentSample;
    [ObservableProperty] private bool _isGpuUsageAvailable;
    [ObservableProperty] private bool _isGpuTemperatureAvailable;

    public string FpsNote => "In-game FPS requires an external overlay (e.g. RTSS/MSI Afterburner) — RESET FPS BOOSTER does not hook into game render loops and will not show a fabricated number.";

    [RelayCommand]
    public void StartMonitoring()
    {
        if (IsMonitoring) return;

        _monitor = new PerformanceMonitorService();
        IsGpuUsageAvailable = _monitor.IsGpuMonitoringAvailable;
        IsGpuTemperatureAvailable = _monitor.IsGpuTemperatureAvailable;

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) =>
        {
            try { CurrentSample = _monitor?.ReadSample(); }
            catch { /* a single failed sample should not stop monitoring */ }
        };
        _timer.Start();
        IsMonitoring = true;
    }

    [RelayCommand]
    public void StopMonitoring()
    {
        _timer?.Stop();
        _timer = null;
        _monitor?.Dispose();
        _monitor = null;
        IsMonitoring = false;
        CurrentSample = null;
    }

    public void Dispose() => StopMonitoring();
}
