using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.ViewModels;

public sealed partial class MainViewModel : ObservableObject
{
    private readonly AppServices _services;
    private readonly Dictionary<NavigationSection, ViewModelBase> _cache = new();

    public ObservableCollection<NavigationItem> NavigationItems { get; }

    [ObservableProperty] private ViewModelBase? _currentViewModel;
    [ObservableProperty] private NavigationSection _currentSection = NavigationSection.Dashboard;
    public bool IsAdministrator => AdminHelper.IsRunningAsAdministrator();

    public MainViewModel(AppServices services)
    {
        _services = services;

        NavigationItems = new ObservableCollection<NavigationItem>
        {
            new(NavigationSection.Dashboard, "Dashboard", IconKind.Dashboard),
            new(NavigationSection.Optimizer, "Optimizer", IconKind.Optimizer),
            new(NavigationSection.GameProfiles, "Game Profiles", IconKind.Games),
            new(NavigationSection.Performance, "Performance", IconKind.Performance),
            new(NavigationSection.System, "System", IconKind.System),
            new(NavigationSection.Backups, "Backups", IconKind.Backups),
            new(NavigationSection.Logs, "Logs", IconKind.Logs),
            new(NavigationSection.Settings, "Settings", IconKind.Settings),
        };

        NavigateTo(NavigationSection.Dashboard);

        if (_services.Settings.Current.AutoCheckForUpdates && !string.IsNullOrWhiteSpace(_services.Settings.Current.UpdateRepository))
            _ = CheckForUpdatesSilentlyAsync();

        if (_services.Settings.Current.EnableGameBoost)
            _services.GameBoost.Start();
    }

    // Runs once at startup so an update is already known by the time the user opens Settings —
    // no manual "Check for Updates" click needed. Never surfaces errors (offline, rate-limited,
    // etc.) since this is a background, best-effort check, not a user-initiated action.
    private async Task CheckForUpdatesSilentlyAsync()
    {
        try
        {
            await _services.Update.CheckForUpdateAsync();
        }
        catch
        {
            // Best-effort — the user can always retry manually from Settings.
        }
    }

    [RelayCommand]
    public void NavigateTo(NavigationSection section)
    {
        if (CurrentViewModel is DashboardViewModel leavingDashboard && CurrentSection != section)
            leavingDashboard.StopLiveUpdates();

        foreach (var item in NavigationItems)
            item.IsSelected = item.Section == section;

        CurrentSection = section;
        CurrentViewModel = GetOrCreate(section);

        _ = RunEntryCommandAsync(section);
    }

    private ViewModelBase GetOrCreate(NavigationSection section)
    {
        if (_cache.TryGetValue(section, out var existing))
            return existing;

        ViewModelBase vm = section switch
        {
            NavigationSection.Dashboard => new DashboardViewModel(
                _services.Hardware, _services.Optimization, _services.Score,
                _services.SystemScan, _services.ChangeLog, _services.GameLibrary)
            {
                NavigateRequested = NavigateTo
            },
            NavigationSection.Optimizer => new OptimizerViewModel(_services.Optimization),
            NavigationSection.GameProfiles => new GameProfilesViewModel(_services.GameLibrary, _services.GameOptimization, _services.GameAutoexec),
            NavigationSection.Performance => new PerformanceViewModel(),
            NavigationSection.System => new SystemViewModel(_services.Hardware, _services.SystemScan),
            NavigationSection.Backups => new BackupsViewModel(_services.Backup, _services.SystemRestore),
            NavigationSection.Logs => new LogsViewModel(_services.ChangeLog),
            NavigationSection.Settings => new SettingsViewModel(_services.Settings, _services.Update, _services.GameBoost),
            _ => throw new ArgumentOutOfRangeException(nameof(section))
        };

        _cache[section] = vm;
        return vm;
    }

    private async Task RunEntryCommandAsync(NavigationSection section)
    {
        switch (GetOrCreate(section))
        {
            case DashboardViewModel dashboard:
                await dashboard.LoadCommand.ExecuteAsync(null);
                break;
            case OptimizerViewModel optimizer:
                await optimizer.LoadCommand.ExecuteAsync(null);
                break;
            case GameProfilesViewModel games:
                games.LoadCommand.Execute(null);
                break;
            case SystemViewModel system:
                await system.LoadCommand.ExecuteAsync(null);
                break;
            case BackupsViewModel backups:
                backups.LoadCommand.Execute(null);
                break;
            case LogsViewModel logs:
                logs.LoadCommand.Execute(null);
                break;
        }
    }
}
