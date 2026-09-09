using System.Collections.ObjectModel;
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class BottleneckEngineViewModel : ViewModelBase, IDisposable
{
    private readonly IGameLibraryService _gameLibraryService;
    private IBottleneckEngineService? _engine;
    private DispatcherTimer? _timer;

    [ObservableProperty] private bool _isMonitoring;
    [ObservableProperty] private BottleneckReport? _currentReport;

    public ObservableCollection<BottleneckHistoryEntry> History { get; } = new();

    public bool IsBalanced => CurrentReport is { IsBalanced: true };
    public BottleneckDiagnosis? Primary => CurrentReport?.Diagnoses.ElementAtOrDefault(0);
    public BottleneckDiagnosis? Secondary => CurrentReport?.Diagnoses.ElementAtOrDefault(1);
    public BottleneckDiagnosis? Tertiary => CurrentReport?.Diagnoses.ElementAtOrDefault(2);

    public BottleneckEngineViewModel(IGameLibraryService gameLibraryService)
    {
        _gameLibraryService = gameLibraryService;
    }

    partial void OnCurrentReportChanged(BottleneckReport? value)
    {
        OnPropertyChanged(nameof(IsBalanced));
        OnPropertyChanged(nameof(Primary));
        OnPropertyChanged(nameof(Secondary));
        OnPropertyChanged(nameof(Tertiary));
    }

    [RelayCommand]
    public void StartMonitoring()
    {
        if (IsMonitoring) return;

        _engine = new BottleneckEngine.BottleneckEngineService(_gameLibraryService);
        History.Clear();

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _timer.Tick += (_, _) => Sample();
        _timer.Start();

        Sample();
        IsMonitoring = true;
    }

    [RelayCommand]
    public void StopMonitoring()
    {
        _timer?.Stop();
        _timer = null;
        _engine?.Dispose();
        _engine = null;
        IsMonitoring = false;
        CurrentReport = null;
        History.Clear();
    }

    private void Sample()
    {
        try
        {
            var report = _engine?.Sample();
            if (report is null) return;

            CurrentReport = report;

            History.Clear();
            foreach (var entry in _engine!.History)
                History.Add(entry);
        }
        catch
        {
            // A single failed sample should not stop monitoring — the next tick tries again.
        }
    }

    public void Dispose() => StopMonitoring();
}
