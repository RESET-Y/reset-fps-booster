using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class BackupsViewModel : ViewModelBase
{
    private readonly IBackupService _backupService;
    private readonly ISystemRestoreService _systemRestoreService;

    [ObservableProperty] private ObservableCollection<BackupSnapshot> _snapshots = new();
    [ObservableProperty] private bool _isCreatingRestorePoint;

    public BackupsViewModel(IBackupService backupService, ISystemRestoreService systemRestoreService)
    {
        _backupService = backupService;
        _systemRestoreService = systemRestoreService;
    }

    [RelayCommand]
    public async Task CreateRestorePointAsync()
    {
        IsCreatingRestorePoint = true;
        try
        {
            var (_, message) = await Task.Run(() =>
                _systemRestoreService.CreateRestorePoint($"RESET FPS BOOSTER — manual backup {DateTime.Now:g}"));
            StatusMessage = message;
        }
        finally
        {
            IsCreatingRestorePoint = false;
        }
    }

    [RelayCommand]
    public void Load()
    {
        Snapshots = new ObservableCollection<BackupSnapshot>(_backupService.ListSnapshots());
    }

    [RelayCommand]
    public void Restore(BackupSnapshot snapshot)
    {
        var (success, message) = _backupService.RestoreSnapshot(snapshot.Id);
        StatusMessage = message;
        _ = success;
        Load();
    }
}
