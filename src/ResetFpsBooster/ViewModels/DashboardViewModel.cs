using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Monitoring;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class DashboardViewModel : ViewModelBase, IDisposable
{
    private readonly IHardwareService _hardwareService;
    private readonly IOptimizationService _optimizationService;
    private readonly IScoreService _scoreService;
    private readonly ISystemScanService _scanService;
    private readonly IChangeLogService _changeLogService;
    private readonly IGameLibraryService _gameLibraryService;

    private IPerformanceMonitorService? _sensorService;
    private DispatcherTimer? _sensorTimer;
    private SystemSnapshot? _snapshot;
    private List<ModuleState> _moduleStates = new();

    [ObservableProperty] private int _score;
    [ObservableProperty] private string _scoreVerdict = "Scanning your system...";
    [ObservableProperty] private string _heroSuffix = "Analyzing.";
    [ObservableProperty] private bool _isAdministrator;
    [ObservableProperty] private string _windowsLabel = string.Empty;

    [ObservableProperty] private HardwareCardViewModel _cpuCard = new() { Title = "CPU" };
    [ObservableProperty] private HardwareCardViewModel _gpuCard = new() { Title = "GPU" };
    [ObservableProperty] private HardwareCardViewModel _ramCard = new() { Title = "RAM" };
    [ObservableProperty] private HardwareCardViewModel _storageCard = new() { Title = "Storage" };

    [ObservableProperty] private ObservableCollection<ChecklistItemViewModel> _scoreChecklist = new();
    [ObservableProperty] private int _availableOptimizationsCount;

    [ObservableProperty] private ObservableCollection<ScanStep> _scanSteps = new();
    [ObservableProperty] private int _scanProgressPercent;
    [ObservableProperty] private bool _isScanning;

    [ObservableProperty] private ObservableCollection<OptimizationCategorySummary> _categorySummaries = new();
    [ObservableProperty] private ObservableCollection<ChangeLogEntry> _recentChanges = new();
    [ObservableProperty] private ObservableCollection<SystemInfoRow> _systemInfoRows = new();
    [ObservableProperty] private ObservableCollection<GameProfile> _quickGames = new();

    [ObservableProperty] private string _gpuDriverVersion = "Unknown";
    [ObservableProperty] private string _powerPlanLabel = "Unknown";
    [ObservableProperty] private bool _powerPlanActive;
    [ObservableProperty] private bool _gameModeActive;

    public Action<NavigationSection>? NavigateRequested { get; set; }

    public DashboardViewModel(
        IHardwareService hardwareService,
        IOptimizationService optimizationService,
        IScoreService scoreService,
        ISystemScanService scanService,
        IChangeLogService changeLogService,
        IGameLibraryService gameLibraryService)
    {
        _hardwareService = hardwareService;
        _optimizationService = optimizationService;
        _scoreService = scoreService;
        _scanService = scanService;
        _changeLogService = changeLogService;
        _gameLibraryService = gameLibraryService;
        IsAdministrator = AdminHelper.IsRunningAsAdministrator();
    }

    [RelayCommand]
    public async Task LoadAsync()
    {
        IsBusy = true;
        try
        {
            _snapshot = await _hardwareService.GetSnapshotAsync();
            _moduleStates = await _optimizationService.RefreshStatusesAsync();

            WindowsLabel = $"{_snapshot.OperatingSystem.ProductName}";
            BuildSystemInfoRows(_snapshot);
            BuildCategorySummaries();
            RecentChanges = new ObservableCollection<ChangeLogEntry>(_changeLogService.GetEntries().Take(5));
            QuickGames = new ObservableCollection<GameProfile>(_gameLibraryService.GetGames().Take(5));

            var gameMode = _moduleStates.FirstOrDefault(m => m.Module.Id == "gaming.game-mode");
            GameModeActive = gameMode?.Status.IsApplied ?? false;

            var powerPlan = _moduleStates.FirstOrDefault(m => m.Module.Id == "windows.power-plan");
            PowerPlanActive = powerPlan?.Status.IsApplied ?? false;
            PowerPlanLabel = powerPlan?.Status.DetailText.Replace("Currently: ", "") ?? "Unknown";

            GpuDriverVersion = _snapshot.PrimaryGpu?.DriverVersion ?? "Unknown";

            AvailableOptimizationsCount = _moduleStates.Count(m => m.Status.IsAvailable && !m.Status.IsApplied);

            var result = await _scoreService.CalculateAsync(_snapshot, _moduleStates);
            Score = result.Score;
            ScoreVerdict = result.Verdict;
            HeroSuffix = Score switch
            {
                >= 85 => "Optimized.",
                >= 60 => "Improving.",
                _ => "Needs Attention."
            };

            BuildChecklist();
            RefreshHardwareCards(_snapshot, null);

            _ = RunQuickScanAsync();
            StartLiveSensors();
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task RunQuickScanAsync()
    {
        IsScanning = true;
        ScanSteps = new ObservableCollection<ScanStep>();
        ScanProgressPercent = 0;
        const int totalSteps = 7;

        var progress = new Progress<ScanStep>(step =>
        {
            var existing = ScanSteps.FirstOrDefault(s => s.Name == step.Name);
            if (existing is null) ScanSteps.Add(step);
            else ScanSteps[ScanSteps.IndexOf(existing)] = step;

            ScanProgressPercent = Math.Min(100, (int)(ScanSteps.Count(s => s.Completed) / (double)totalSteps * 100));
        });

        try { await _scanService.RunScanAsync(progress); }
        finally { IsScanning = false; }
    }

    private void StartLiveSensors()
    {
        _sensorService ??= new PerformanceMonitorService();
        _sensorTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _sensorTimer.Tick -= OnSensorTick;
        _sensorTimer.Tick += OnSensorTick;
        _sensorTimer.Start();
    }

    private void OnSensorTick(object? sender, EventArgs e)
    {
        if (_snapshot is null) return;
        try
        {
            var sample = _sensorService?.ReadSample();
            RefreshHardwareCards(_snapshot, sample);
        }
        catch
        {
            // A missed sensor tick is not worth surfacing to the user.
        }
    }

    private void RefreshHardwareCards(SystemSnapshot snapshot, PerformanceSample? sample)
    {
        var cpuUsage = sample?.CpuUsagePercent ?? snapshot.Cpu.CurrentUsagePercent;
        CpuCard = new HardwareCardViewModel
        {
            Title = "CPU",
            Subtitle = snapshot.Cpu.Name,
            UsagePercent = Math.Clamp(cpuUsage, 0, 100),
            StatLineOne = $"{snapshot.Cpu.MaxClockSpeedGhz:0.0} GHz",
            StatLineTwo = $"{snapshot.Cpu.Cores} Cores / {snapshot.Cpu.LogicalProcessors} Threads"
        };

        var gpu = snapshot.PrimaryGpu;
        GpuCard = new HardwareCardViewModel
        {
            Title = "GPU",
            Subtitle = gpu?.Name ?? "Not detected",
            UsagePercent = Math.Clamp(sample?.GpuUsagePercent ?? 0, 0, 100),
            StatLineOne = sample?.GpuMemoryUsedBytes is { } vram ? FormatBytes(vram) + " VRAM" : FormatBytes(gpu?.AdapterRamBytes ?? 0) + " VRAM",
            StatLineTwo = $"Driver {gpu?.DriverVersion ?? "Unknown"}",
            TemperatureLabel = sample?.GpuTemperatureCelsius is { } gt ? $"{gt:0}°C" : null
        };

        var memUsage = sample?.RamUsedPercent ?? snapshot.Memory.UsedPercent;
        RamCard = new HardwareCardViewModel
        {
            Title = "RAM",
            Subtitle = $"{FormatBytes(snapshot.Memory.TotalBytes)} {(snapshot.Memory.SpeedMhz > 0 ? $"@ {snapshot.Memory.SpeedMhz:0} MHz" : string.Empty)}".Trim(),
            UsagePercent = Math.Clamp(memUsage, 0, 100),
            StatLineOne = $"{FormatBytes(snapshot.Memory.UsedBytes)} Used",
            StatLineTwo = $"{FormatBytes(snapshot.Memory.AvailableBytes)} Free"
        };

        var systemDrive = snapshot.Drives.FirstOrDefault(d => d.DriveLetter.StartsWith(Path.GetPathRoot(Environment.SystemDirectory) ?? "C:", StringComparison.OrdinalIgnoreCase))
            ?? snapshot.Drives.FirstOrDefault();
        StorageCard = new HardwareCardViewModel
        {
            Title = "Storage",
            Subtitle = systemDrive is null ? "No drive detected" : $"{systemDrive.MediaType} ({systemDrive.DriveLetter})",
            UsagePercent = systemDrive?.UsedPercent ?? 0,
            StatLineOne = $"{FormatBytes(systemDrive?.UsedBytes ?? 0)} Used",
            StatLineTwo = $"{FormatBytes(systemDrive?.TotalBytes ?? 0)} Total"
        };
    }

    private void BuildChecklist()
    {
        var gaming = _moduleStates.Where(s => s.Module.Category == OptimizationCategory.Gaming).ToList();
        var gamingGood = gaming.Count > 0 && gaming.All(s => !s.Status.IsAvailable || s.Status.IsApplied);

        var background = _moduleStates.Where(s => s.Module.Id is "windows.background-apps" or "windows.startup-cleanup").ToList();
        var backgroundGood = background.Count > 0 && background.All(s => s.Status.IsApplied);

        var windows = _moduleStates.Where(s => s.Module.Category == OptimizationCategory.Windows).ToList();
        var windowsGood = windows.Count > 0 && windows.Count(s => s.Status.IsApplied) >= windows.Count / 2;

        var systemDrive = _snapshot?.Drives.FirstOrDefault();
        var noCriticalIssues = (systemDrive is null || systemDrive.FreeBytes > 2L * 1024 * 1024 * 1024)
            && (_snapshot is null || _snapshot.Memory.UsedPercent < 95);

        ScoreChecklist = new ObservableCollection<ChecklistItemViewModel>
        {
            new() { Text = "Gaming optimizations active", IsGood = gamingGood },
            new() { Text = "Background processes optimized", IsGood = backgroundGood },
            new() { Text = "System settings optimized", IsGood = windowsGood },
            new() { Text = "No critical issues found", IsGood = noCriticalIssues },
        };
    }

    private void BuildCategorySummaries()
    {
        var glyphs = new Dictionary<OptimizationCategory, string>
        {
            [OptimizationCategory.Gaming] = "",
            [OptimizationCategory.Windows] = "",
            [OptimizationCategory.Cpu] = "",
            [OptimizationCategory.Gpu] = "",
            [OptimizationCategory.Memory] = "",
            [OptimizationCategory.Storage] = "",
            [OptimizationCategory.Network] = "",
        };

        var subtitles = new Dictionary<OptimizationCategory, string>
        {
            [OptimizationCategory.Gaming] = "Game Mode, Game DVR, Background Apps",
            [OptimizationCategory.Windows] = "Startup, Temp Files, Power Plan",
            [OptimizationCategory.Cpu] = "Power Throttling",
            [OptimizationCategory.Gpu] = "Scheduling, Task Priority",
            [OptimizationCategory.Memory] = "Working Set Trim, Standby Cache",
            [OptimizationCategory.Storage] = "Temp File Cleanup",
            [OptimizationCategory.Network] = "Throttling Settings",
        };

        var grouped = _moduleStates
            .GroupBy(s => s.Module.Category)
            .Where(g => g.Any())
            .Select(g => new OptimizationCategorySummary
            {
                Name = $"{g.Key} Optimization",
                Subtitle = subtitles.TryGetValue(g.Key, out var sub) ? sub : string.Empty,
                Count = g.Count(),
                Glyph = glyphs.TryGetValue(g.Key, out var glyph) ? glyph : ""
            })
            .OrderByDescending(s => s.Count);

        CategorySummaries = new ObservableCollection<OptimizationCategorySummary>(grouped);
    }

    private void BuildSystemInfoRows(SystemSnapshot snapshot)
    {
        SystemInfoRows = new ObservableCollection<SystemInfoRow>
        {
            new() { Label = "CPU", Value = snapshot.Cpu.Name },
            new() { Label = "GPU", Value = snapshot.PrimaryGpu?.Name ?? "Not detected" },
            new() { Label = "RAM", Value = FormatBytes(snapshot.Memory.TotalBytes) },
            new() { Label = "Motherboard", Value = $"{snapshot.Motherboard.Manufacturer} {snapshot.Motherboard.Product}".Trim() },
            new() { Label = "Storage", Value = string.Join(", ", snapshot.Drives.Select(d => $"{d.MediaType} {FormatBytes(d.TotalBytes)}")) },
            new() { Label = "OS", Value = snapshot.OperatingSystem.ProductName },
            new() { Label = "Build", Value = snapshot.OperatingSystem.Build },
            new() { Label = "Architecture", Value = snapshot.OperatingSystem.Architecture },
        };
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double size = bytes;
        var unit = 0;
        while (size >= 1024 && unit < units.Length - 1) { size /= 1024; unit++; }
        return $"{size:0.#} {units[unit]}";
    }

    [RelayCommand] public void GoToOptimizer() => NavigateRequested?.Invoke(NavigationSection.Optimizer);
    [RelayCommand] public void GoToLogs() => NavigateRequested?.Invoke(NavigationSection.Logs);
    [RelayCommand] public void GoToGames() => NavigateRequested?.Invoke(NavigationSection.GameProfiles);

    public void StopLiveUpdates()
    {
        _sensorTimer?.Stop();
        _sensorService?.Dispose();
        _sensorService = null;
    }

    public void Dispose() => StopLiveUpdates();
}
