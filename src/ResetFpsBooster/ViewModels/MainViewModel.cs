using System.Collections.ObjectModel;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class MainViewModel : ObservableObject
{
    private readonly AppServices _services;
    private readonly Dictionary<NavigationSection, ViewModelBase> _cache = new();

    /// <summary>The sidebar: the Dashboard alone, then the pages in foldable groups.</summary>
    public ObservableCollection<NavigationGroup> NavigationGroups { get; }

    /// <summary>Account, the manager page and Settings: always in reach at the bottom, never folded.</summary>
    public ObservableCollection<NavigationItem> PinnedItems { get; }

    private IEnumerable<NavigationItem> AllItems => NavigationGroups.SelectMany(g => g.Items).Concat(PinnedItems);

    [ObservableProperty] private ViewModelBase? _currentViewModel;
    [ObservableProperty] private NavigationSection _currentSection = NavigationSection.Dashboard;
    public bool IsAdministrator => AdminHelper.IsRunningAsAdministrator();

    public MainViewModel(AppServices services)
    {
        _services = services;

        // What belongs together sits together: tuning the PC, looking at how it runs, and the
        // tools for playing. Analysis starts folded (see AppSettings.NavCollapsedGroups).
        var gaming = new List<NavigationItem>();
#if RFB_BETA
        gaming.Add(new(NavigationSection.FrameBoostBeta, "Nav.FrameBoost", IconKind.FrameBoostBeta));
        gaming.Add(new(NavigationSection.SmoothMotion, "Nav.SmoothMotion", IconKind.SmoothMotion));
#endif
        gaming.Add(new(NavigationSection.Crosshair, "Nav.Crosshair", IconKind.Crosshair));
        gaming.Add(new(NavigationSection.SensFinder, "Nav.SensFinder", IconKind.SensFinder));
        gaming.Add(new(NavigationSection.ApexConfig, "Nav.Apex", IconKind.ApexConfig));

        NavigationGroups = new ObservableCollection<NavigationGroup>
        {
            new("main", null, new NavigationItem[]
            {
                new(NavigationSection.Dashboard, "Nav.Dashboard", IconKind.Dashboard),
            }),
            new("tuning", "Nav.Group.Tuning", new NavigationItem[]
            {
                new(NavigationSection.Optimizer, "Nav.Optimizer", IconKind.Optimizer),
                new(NavigationSection.GameProfiles, "Nav.GameProfiles", IconKind.Games),
                new(NavigationSection.Backups, "Nav.Backups", IconKind.Backups),
            }),
            new("analysis", "Nav.Group.Analysis", new NavigationItem[]
            {
                new(NavigationSection.Performance, "Nav.Performance", IconKind.Performance),
                new(NavigationSection.BottleneckEngine, "Nav.BottleneckEngine", IconKind.BottleneckEngine),
                new(NavigationSection.System, "Nav.System", IconKind.System),
                new(NavigationSection.Logs, "Nav.Logs", IconKind.Logs),
            }),
            new("gaming", "Nav.Group.Gaming", gaming),
        };

        var folded = _services.Settings.Current.NavCollapsedGroups;
        foreach (var group in NavigationGroups)
            group.IsExpanded = !folded.Contains(group.Id);

        PinnedItems = new ObservableCollection<NavigationItem>
        {
            new(NavigationSection.Account, "Nav.Account", IconKind.Account),
            new(NavigationSection.Settings, "Nav.Settings", IconKind.Settings),
        };

        NavigateTo(NavigationSection.Dashboard);

        // A stored sign-in is restored quietly in the background. Nothing waits
        // on it: the app works signed out, and premium is only asked for once
        // the session is back.
        // The crosshair comes back on by itself if it was on last time - but
        // only once the restored session confirms premium.
        _ = RestoreSessionAsync();

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

    private async Task RestoreSessionAsync()
    {
        try { await _services.Auth.RestoreAsync(); } catch { /* signed out is fine */ }
        await _services.Crosshair.RestoreAsync(_services.Settings, _services.Auth);
    }

    // Runs once at startup. Errors (offline, rate-limited, etc.) stay silent - this is a
    // background, best-effort check. A newer release with an installer attached pops up
    // once; "Later" leaves it waiting in Settings.
    private async Task CheckForUpdatesSilentlyAsync()
    {
        try
        {
            var result = await _services.Update.CheckForUpdateAsync();
            if (!result.Success || !result.IsUpdateAvailable || string.IsNullOrEmpty(result.DownloadUrl)) return;

            // The check usually beats the main window's fade-in; give the window a moment
            // so the pop-up has an owner to centre on.
            await Task.Delay(1200);
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                var owner = System.Windows.Application.Current.MainWindow;
                var popup = new UpdateWindow(_services.Update, result);
                if (owner is { IsLoaded: true }) popup.Owner = owner;
                else popup.WindowStartupLocation = System.Windows.WindowStartupLocation.CenterScreen;
                popup.ShowDialog();
            });
        }
        catch
        {
            // Best-effort — the user can always retry manually from Settings.
        }
    }

    private async Task UpdateManagerEntryAsync()
    {
        var isManager = await _services.Auth.IsManagerAsync();
        var existing = PinnedItems.FirstOrDefault(i => i.Section == NavigationSection.Manager);

        // Back on the UI thread for the collection change: the sign-in event
        // can arrive from the background restore at start-up.
        System.Windows.Application.Current.Dispatcher.Invoke(() =>
        {
            if (isManager && existing is null)
            {
                var account = PinnedItems.FirstOrDefault(i => i.Section == NavigationSection.Account);
                var index = account is null ? PinnedItems.Count : PinnedItems.IndexOf(account) + 1;
                PinnedItems.Insert(index, new NavigationItem(NavigationSection.Manager, "Nav.Manager", IconKind.Manager));
            }
            else if (!isManager && existing is not null)
            {
                PinnedItems.Remove(existing);
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

        foreach (var item in AllItems)
            item.IsSelected = item.Section == section;

        // A page reached by any road (a Dashboard button, a link) opens its group, so it is seen in the list.
        foreach (var group in NavigationGroups)
        {
            group.RefreshSelection();
            if (group.HasHeader && group.HasSelection && !group.IsExpanded) group.IsExpanded = true;
        }

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
                _services.SystemScan, _services.ChangeLog, _services.GameLibrary, _services.Settings)
            {
                NavigateRequested = NavigateTo
            },
            NavigationSection.Optimizer => new OptimizerViewModel(_services.Optimization),
            NavigationSection.GameProfiles => new GameProfilesViewModel(_services.GameLibrary, _services.GameOptimization, _services.GameAutoexec),
            NavigationSection.Performance => new PerformanceViewModel(),
            NavigationSection.BottleneckEngine => new BottleneckEngineViewModel(_services.GameLibrary),
#if RFB_BETA
            NavigationSection.SmoothMotion => new SmoothMotionViewModel(_services.FrameBoostBeta, _services.Settings, _services.Auth,
                () => NavigateTo(NavigationSection.Account)),
            NavigationSection.FrameBoostBeta => new FrameBoostBetaViewModel(_services.FrameBoostBeta, _services.Settings, _services.Auth,
                () => NavigateTo(NavigationSection.Account)),
#endif
            NavigationSection.Crosshair => new CrosshairViewModel(_services.Crosshair, _services.Settings, _services.Auth,
                () => NavigateTo(NavigationSection.Account)),
            NavigationSection.SensFinder => new SensFinderViewModel(_services.Settings),
            NavigationSection.ApexConfig => new ApexConfigViewModel(
                new ApexSettingsService(_services.Backup, ApexInstallFolder), _services.Settings),
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

    /// <summary>Folds or opens a group by its heading, and remembers it for the next start.</summary>
    [RelayCommand]
    private void ToggleGroup(NavigationGroup group)
    {
        group.IsExpanded = !group.IsExpanded;
        var folded = _services.Settings.Current.NavCollapsedGroups;
        folded.Remove(group.Id);
        if (!group.IsExpanded) folded.Add(group.Id);
        _services.Settings.Save();
    }

    // Where the game library found Apex Legends (for its autoexec.cfg); null if it has not.
    private string? ApexInstallFolder() => _services.GameLibrary.GetGames()
        .FirstOrDefault(g => g.Name.Contains("Apex Legends", StringComparison.OrdinalIgnoreCase) ||
                             Path.GetFileName(g.ExecutablePath ?? "").StartsWith("r5apex", StringComparison.OrdinalIgnoreCase))
        ?.InstallPath;

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
            case ApexConfigViewModel apex:
                apex.Refresh();
                break;
        }
    }
}
