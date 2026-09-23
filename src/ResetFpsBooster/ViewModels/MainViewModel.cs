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
            new(NavigationSection.Dashboard, "Nav.Dashboard", IconKind.Dashboard),
            new(NavigationSection.Optimizer, "Nav.Optimizer", IconKind.Optimizer),
            new(NavigationSection.GameProfiles, "Nav.GameProfiles", IconKind.Games),
            new(NavigationSection.Performance, "Nav.Performance", IconKind.Performance),
            new(NavigationSection.BottleneckEngine, "Nav.BottleneckEngine", IconKind.BottleneckEngine),
#if RFB_BETA
            new(NavigationSection.FrameBoostBeta, "Nav.FrameBoost", IconKind.FrameBoostBeta),
#endif
            new(NavigationSection.System, "Nav.System", IconKind.System),
            new(NavigationSection.Backups, "Nav.Backups", IconKind.Backups),
            new(NavigationSection.Logs, "Nav.Logs", IconKind.Logs),
            new(NavigationSection.Account, "Nav.Account", IconKind.Account),
            new(NavigationSection.Settings, "Nav.Settings", IconKind.Settings),
        };

        NavigateTo(NavigationSection.Dashboard);

        // A stored sign-in is restored quietly in the background. Nothing waits
        // on it: the app works signed out, and premium is only asked for once
        // the session is back.
        _ = _services.Auth.RestoreAsync();

        // THE MANAGER ENTRY APPEARS ONLY FOR A MANAGER, decided by the server.
        // Asked again on every sign-in change, so it comes and goes with the
        // account. Hiding it is courtesy, not protection - every action on
        // that page is refused by the database for anyone who is not one.
        _services.Auth.SignInStateChanged += async (_, _) => await UpdateManagerEntryAsync();

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

    private async Task UpdateManagerEntryAsync()
    {
        var isManager = await _services.Auth.IsManagerAsync();
        var existing = NavigationItems.FirstOrDefault(i => i.Section == NavigationSection.Manager);

        // Back on the UI thread for the collection change: the sign-in event
        // can arrive from the background restore at start-up.
        System.Windows.Application.Current.Dispatcher.Invoke(() =>
        {
            if (isManager && existing is null)
            {
                var account = NavigationItems.FirstOrDefault(i => i.Section == NavigationSection.Account);
                var index = account is null ? NavigationItems.Count : NavigationItems.IndexOf(account) + 1;
                NavigationItems.Insert(index, new NavigationItem(NavigationSection.Manager, "Nav.Manager", IconKind.Manager));
            }
            else if (!isManager && existing is not null)
            {
                NavigationItems.Remove(existing);
                if (CurrentSection == NavigationSection.Manager) NavigateTo(NavigationSection.Dashboard);
            }
        });
    }

    // [RelayCommand] generates NavigateToCommand, which every sidebar button
    // binds to. Keep it directly on this method: once, a method inserted
    // between the attribute and this line took the attribute over, the command
    // vanished, and every sidebar click silently did nothing.
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
            NavigationSection.BottleneckEngine => new BottleneckEngineViewModel(_services.GameLibrary),
#if RFB_BETA
            NavigationSection.FrameBoostBeta => new FrameBoostBetaViewModel(_services.FrameBoostBeta, _services.Settings, _services.Auth,
                () => NavigateTo(NavigationSection.Account)),
#endif
            NavigationSection.System => new SystemViewModel(_services.Hardware, _services.SystemScan),
            NavigationSection.Backups => new BackupsViewModel(_services.Backup, _services.SystemRestore),
            NavigationSection.Logs => new LogsViewModel(_services.ChangeLog),
            NavigationSection.Account => new AccountViewModel(_services.Auth),
            NavigationSection.Manager => new ManagerViewModel(_services.Auth),
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
