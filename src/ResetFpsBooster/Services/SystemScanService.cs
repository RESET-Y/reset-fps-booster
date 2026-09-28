using ResetFpsBooster.Core.Localization;
using System.Diagnostics;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

/// <summary>
/// Runs the same checks the optimization modules use, step by step, so the user sees exactly
/// what is being inspected. No step is simulated — each one performs a real read of system state.
/// </summary>
public sealed class SystemScanService : ISystemScanService
{
    private readonly IOptimizationService _optimizationService;
    private readonly IStartupAppsService _startupApps;
    private readonly IHardwareService _hardwareService;

    public SystemScanService(IOptimizationService optimizationService, IStartupAppsService startupApps, IHardwareService hardwareService)
    {
        _optimizationService = optimizationService;
        _startupApps = startupApps;
        _hardwareService = hardwareService;
    }

    public async Task<List<ScanStep>> RunScanAsync(IProgress<ScanStep>? progress, CancellationToken ct = default)
    {
        var results = new List<ScanStep>();

        async Task RunStep(string name, Func<Task<(bool HasFindings, string Summary)>> work)
        {
            ct.ThrowIfCancellationRequested();
            var step = new ScanStep { Name = name };
            progress?.Report(step);

            try
            {
                var (hasFindings, summary) = await work();
                step.Completed = true;
                step.HasFindings = hasFindings;
                step.Summary = summary;
            }
            catch (Exception ex)
            {
                step.Completed = true;
                step.HasFindings = true;
                step.Summary = Loc.F("Scan.Failed", ex.Message);
            }

            results.Add(step);
            progress?.Report(step);
        }

        await RunStep(Loc.T("Scan.Cpu"), async () =>
        {
            var snapshot = await _hardwareService.GetSnapshotAsync(ct);
            return (false, Loc.F("Scan.CpuResult", snapshot.Cpu.Name, snapshot.Cpu.LogicalProcessors));
        });

        await RunStep(Loc.T("Scan.Startup"), () =>
        {
            var items = _startupApps.GetStartupItems();
            var enabled = items.Count(i => i.IsEnabled);
            var nonEssential = items.Count(i => i.IsRecommendedToDisable && i.IsEnabled);
            return Task.FromResult((nonEssential > 0, Loc.F("Scan.StartupResult", enabled, nonEssential)));
        });

        await RunStep(Loc.T("Scan.Gaming"), async () =>
        {
            var states = await _optimizationService.RefreshStatusesAsync(ct);
            var gaming = states.Where(s => s.Module.Category == OptimizationCategory.Gaming).ToList();
            var notApplied = gaming.Count(s => s.Status.IsAvailable && !s.Status.IsApplied);
            return (notApplied > 0, Loc.F("Scan.GamingResult", gaming.Count(s => s.Status.IsApplied), gaming.Count));
        });

        await RunStep(Loc.T("Scan.Gpu"), async () =>
        {
            var snapshot = await _hardwareService.GetSnapshotAsync(ct);
            var gpu = snapshot.PrimaryGpu;
            return (false, gpu is null ? Loc.T("Scan.NoGpu") : Loc.F("Scan.GpuResult", gpu.Name, gpu.DriverVersion));
        });

        await RunStep(Loc.T("Scan.Processes"), () =>
        {
            var count = Process.GetProcesses().Length;
            return Task.FromResult((false, Loc.F("Scan.ProcessesResult", count)));
        });

        await RunStep(Loc.T("Scan.PowerNet"), async () =>
        {
            var states = await _optimizationService.RefreshStatusesAsync(ct);
            var relevant = states.Where(s => s.Module.Category is OptimizationCategory.Windows or OptimizationCategory.Network).ToList();
            var notApplied = relevant.Count(s => s.Status.IsAvailable && !s.Status.IsApplied);
            return (notApplied > 0, Loc.F("Scan.PowerNetResult", relevant.Count(s => s.Status.IsApplied), relevant.Count));
        });

        await RunStep(Loc.T("Scan.Final"), () => Task.FromResult((false, Loc.T("Scan.Done"))));

        return results;
    }
}
