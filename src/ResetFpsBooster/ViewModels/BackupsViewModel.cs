using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class BackupsViewModel : ViewModelBase
{
    private readonly IBackupService _backupService;

    [ObservableProperty] private ObservableCollection<BackupSnapshot> _snapshots = new();

    public BackupsViewModel(IBackupService backupService)
    {
        _backupService = backupService;
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
