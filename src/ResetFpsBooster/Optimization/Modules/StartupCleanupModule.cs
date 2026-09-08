using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>
/// Disables only the startup entries that match a curated, conservative list of common
/// non-essential helpers (OneDrive, Spotify, Adobe helpers, etc.). Gaming platforms and
/// drivers are never touched. Every entry that will be disabled is listed in the change log.
/// </summary>
public sealed class StartupCleanupModule : IOptimizationModule
{
    private readonly IStartupAppsService _startupApps;

    public StartupCleanupModule(IStartupAppsService startupApps)
    {
        _startupApps = startupApps;
    }

    public string Id => "windows.startup-cleanup";
    public string Name => "Startup Optimization";
    public string Description => "Disables common non-essential startup helpers (e.g. OneDrive, Spotify, Adobe updaters). Gaming platforms and drivers are never touched. Fully reversible.";
    public OptimizationCategory Category => OptimizationCategory.Windows;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var candidates = _startupApps.GetStartupItems().Where(i => i.IsRecommendedToDisable).ToList();
        var stillEnabled = candidates.Count(i => i.IsEnabled);

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = candidates.Count > 0 && stillEnabled == 0,
            DetailText = candidates.Count == 0
                ? "No non-essential startup helpers found"
                : $"{stillEnabled} of {candidates.Count} non-essential helper(s) still enabled"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        var candidates = _startupApps.GetStartupItems().Where(i => i.IsRecommendedToDisable && i.IsEnabled).ToList();

        if (candidates.Count == 0)
            return Task.FromResult(OptimizationApplyResult.Skipped("No non-essential startup helpers to disable."));

        foreach (var item in candidates)
            _startupApps.SetEnabled(recorder, item, enabled: false);

        return Task.FromResult(OptimizationApplyResult.Ok(
            $"Disabled {candidates.Count} startup item(s): {string.Join(", ", candidates.Select(c => c.Name))}.",
            recorder.ChangeLog));
    }
}
