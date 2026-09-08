using ResetFpsBooster.Games;
using ResetFpsBooster.Hardware;
using ResetFpsBooster.Optimization;
using ResetFpsBooster.Optimization.Modules;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.Core;

/// <summary>
/// Composition root. A small app like this doesn't need a DI container — services are
/// singletons wired up once at startup and handed to view models directly.
/// </summary>
public sealed class AppServices
{
    public IHardwareService Hardware { get; }
    public IChangeLogService ChangeLog { get; }
    public IBackupService Backup { get; }
    public ISettingsService Settings { get; }
    public IStartupAppsService StartupApps { get; }
    public IOptimizationService Optimization { get; }
    public ISystemScanService SystemScan { get; }
    public IScoreService Score { get; }
    public IGameDetectionService GameDetection { get; }
    public IGameLibraryService GameLibrary { get; }
    public IGameOptimizationService GameOptimization { get; }
    public IGameAutoexecService GameAutoexec { get; }
    public IUpdateService Update { get; }

    public AppServices()
    {
        Hardware = new HardwareService();
        ChangeLog = new ChangeLogService();
        Backup = new BackupService(ChangeLog);
        Settings = new SettingsService();
        StartupApps = new StartupAppsService();

        var modules = new List<IOptimizationModule>
        {
            new GameModeModule(),
            new GameDvrModule(),
            new PowerPlanModule(),
            new HagsModule(),
            new GpuTaskPriorityModule(),
            new NetworkThrottlingModule(),
            new PowerThrottlingModule(),
            new VisualEffectsModule(),
            new BackgroundAppsModule(),
            new StartupCleanupModule(StartupApps),
            new TempFileCleanupModule(),
            new RamCleanerModule(),
        };

        Optimization = new OptimizationService(modules, Backup);
        SystemScan = new SystemScanService(Optimization, StartupApps, Hardware);
        Score = new ScoreService();
        GameDetection = new GameDetectionService();
        GameLibrary = new GameLibraryService(GameDetection);
        GameOptimization = new GameOptimizationService(Backup, GameLibrary);
        GameAutoexec = new GameAutoexecService(Backup, GameLibrary);
        Update = new UpdateService(Settings);
    }
}
