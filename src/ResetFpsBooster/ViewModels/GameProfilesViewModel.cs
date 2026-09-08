using System.Collections.ObjectModel;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Services;
using Microsoft.Win32;

namespace ResetFpsBooster.ViewModels;

public sealed partial class GameProfilesViewModel : ViewModelBase
{
    private readonly IGameLibraryService _libraryService;
    private readonly IGameOptimizationService _gameOptimizationService;

    [ObservableProperty] private ObservableCollection<GameProfileItemViewModel> _games = new();
    [ObservableProperty] private bool _isScanning;

    public GameProfilesViewModel(IGameLibraryService libraryService, IGameOptimizationService gameOptimizationService)
    {
        _libraryService = libraryService;
        _gameOptimizationService = gameOptimizationService;
    }

    [RelayCommand]
    public void Load()
    {
        Games = new ObservableCollection<GameProfileItemViewModel>(
            _libraryService.GetGames().Select(g => new GameProfileItemViewModel(g)));
    }

    [RelayCommand]
    public async Task RescanAsync()
    {
        IsScanning = true;
        try
        {
            await _libraryService.RescanAsync();
            Load();
        }
        finally
        {
            IsScanning = false;
        }
    }

    [RelayCommand]
    public async Task OptimizeGameAsync(GameProfileItemViewModel item)
    {
        var (success, message) = await _gameOptimizationService.OptimizeGameAsync(item.Profile);
        item.LastActionMessage = message;
        item.Refresh();
        StatusMessage = message;
        _ = success;
    }

    [RelayCommand]
    public void RestoreGame(GameProfileItemViewModel item)
    {
        var (success, message) = _gameOptimizationService.RestoreGameSettings(item.Profile);
        item.LastActionMessage = message;
        item.Refresh();
        StatusMessage = message;
        _ = success;
    }

    [RelayCommand]
    public void RemoveGame(GameProfileItemViewModel item)
    {
        _libraryService.Remove(item.Profile.Id);
        Load();
    }

    [RelayCommand]
    public void BrowseForExecutable(GameProfileItemViewModel item)
    {
        var dialog = new OpenFileDialog
        {
            Title = $"Locate the executable for {item.Name}",
            Filter = "Executable (*.exe)|*.exe",
            InitialDirectory = Directory.Exists(item.InstallPath) ? item.InstallPath : string.Empty
        };

        if (dialog.ShowDialog() == true)
        {
            item.Profile.ExecutablePath = dialog.FileName;
            _libraryService.Save(item.Profile);
            item.Refresh();
        }
    }
}
