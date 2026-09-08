using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Optimization;

namespace ResetFpsBooster.Services;

public sealed class OptimizationService : IOptimizationService
{
    private readonly IBackupService _backupService;

    public IReadOnlyList<IOptimizationModule> Modules { get; }

    public OptimizationService(IEnumerable<IOptimizationModule> modules, IBackupService backupService)
    {
        Modules = modules.ToList();
        _backupService = backupService;
    }

    public async Task<List<ModuleState>> RefreshStatusesAsync(CancellationToken ct = default)
    {
        // Every module's check is I/O-bound (registry, disk, or a spawned process) and independent
        // of the others, so running them concurrently instead of one-by-one is what keeps opening
        // the Optimizer page fast instead of paying the sum of every module's latency in sequence.
        var checks = Modules.Select(async module =>
        {
            OptimizationStatus status;
            try
            {
                status = await Task.Run(() => module.CheckStatusAsync(ct), ct);
            }
            catch (Exception ex)
            {
                status = new OptimizationStatus { IsAvailable = false, UnavailableReason = $"Check failed: {ex.Message}" };
            }

            return new ModuleState { Module = module, Status = status };
        });

        var results = await Task.WhenAll(checks);
        return results.ToList();
    }

    public async Task<OptimizationApplyResult> ApplyModuleAsync(IOptimizationModule module, CancellationToken ct = default)
    {
        if (module.RequiresAdmin && !AdminHelper.IsRunningAsAdministrator())
            return OptimizationApplyResult.Fail("This optimization requires administrator privileges.");

        var recorder = new RegistryChangeRecorder(module.Name);
        OptimizationApplyResult result;

        try
        {
            result = await module.ApplyAsync(recorder, ct);
        }
        catch (UnauthorizedAccessException)
        {
            return OptimizationApplyResult.Fail("Access denied. No changes were made.");
        }
        catch (Exception ex)
        {
            return OptimizationApplyResult.Fail($"Could not apply this optimization: {ex.Message}. No changes were made.");
        }

        if (result.Success && recorder.BackupEntries.Count > 0)
        {
            _backupService.CommitSnapshot(recorder, $"{module.Name}", result.PowerPlanBackup);
        }
        else if (result.Success && recorder.ChangeLog.Count > 0)
        {
            // Non-registry effects (e.g. temp cleanup) still get logged even without a backup snapshot.
            _backupService.CommitSnapshot(recorder, $"{module.Name}", result.PowerPlanBackup);
        }

        return result;
    }

    public async Task<List<OptimizationApplyResult>> ApplyModulesAsync(
        IEnumerable<IOptimizationModule> modules,
        IProgress<(IOptimizationModule Module, OptimizationApplyResult Result)>? progress = null,
        CancellationToken ct = default)
    {
        var results = new List<OptimizationApplyResult>();
        foreach (var module in modules)
        {
            ct.ThrowIfCancellationRequested();
            var result = await ApplyModuleAsync(module, ct);
            results.Add(result);
            progress?.Report((module, result));
        }

        return results;
    }
}
