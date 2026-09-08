using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class SystemViewModel : ViewModelBase
{
    private readonly IHardwareService _hardwareService;
    private readonly ISystemScanService _scanService;

    [ObservableProperty] private SystemSnapshot? _snapshot;
    [ObservableProperty] private ObservableCollection<ScanStep> _scanSteps = new();
    [ObservableProperty] private bool _isScanning;
    [ObservableProperty] private int _scanProgressPercent;

    public SystemViewModel(IHardwareService hardwareService, ISystemScanService scanService)
    {
        _hardwareService = hardwareService;
        _scanService = scanService;
    }

    [RelayCommand]
    public async Task LoadAsync()
    {
        IsBusy = true;
        try { Snapshot = await _hardwareService.GetSnapshotAsync(); }
        finally { IsBusy = false; }
    }

    [RelayCommand]
    public async Task RunDeepScanAsync()
    {
        IsScanning = true;
        ScanSteps = new ObservableCollection<ScanStep>();
        ScanProgressPercent = 0;

        const int totalSteps = 7;
        var progress = new Progress<ScanStep>(step =>
        {
            var existing = ScanSteps.FirstOrDefault(s => s.Name == step.Name);
            if (existing is null)
            {
                ScanSteps.Add(step);
            }
            else
            {
                var index = ScanSteps.IndexOf(existing);
                ScanSteps[index] = step;
            }

            ScanProgressPercent = Math.Min(100, (int)(ScanSteps.Count(s => s.Completed) / (double)totalSteps * 100));
        });

        try
        {
            await _scanService.RunScanAsync(progress);
        }
        finally
        {
            IsScanning = false;
        }
    }
}
