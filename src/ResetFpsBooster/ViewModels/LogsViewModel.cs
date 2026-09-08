using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class LogsViewModel : ViewModelBase
{
    private readonly IChangeLogService _changeLogService;

    [ObservableProperty] private ObservableCollection<ChangeLogEntry> _entries = new();

    public LogsViewModel(IChangeLogService changeLogService)
    {
        _changeLogService = changeLogService;
    }

    [RelayCommand]
    public void Load()
    {
        Entries = new ObservableCollection<ChangeLogEntry>(_changeLogService.GetEntries());
    }

    [RelayCommand]
    public void Clear()
    {
        _changeLogService.Clear();
        Load();
    }
}
