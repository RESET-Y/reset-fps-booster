using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>
/// Tunes the "Games" multimedia task profile that the Windows scheduler (MMCSS) already
/// applies to processes which register themselves as games. Raises their GPU/CPU scheduling
/// priority. This does not target a specific executable — it configures the shared profile.
/// </summary>
public sealed class GpuTaskPriorityModule : IOptimizationModule
{
    private const string SubKey = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks\Games";

    public string Id => "gpu.task-priority";
    public string Name => "Gaming Task Scheduler Priority";
    public string Description => "Raises the CPU/GPU scheduling priority Windows grants to the built-in \"Games\" multimedia task category used by MMCSS.";
    public OptimizationCategory Category => OptimizationCategory.Gpu;
    // Medium, not Low: this rewrites a shared, system-wide scheduler profile (not per-game), and
    // elevated CPU/GPU priority for the "Games" task category has been reported to contribute to
    // instability in some anti-cheat-protected titles when combined with other scheduling changes
    // (e.g. HAGS). Requires deliberate opt-in rather than being pre-selected by default.
    public RiskLevel Risk => RiskLevel.Medium;
    public bool RequiresAdmin => true;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(SubKey);
            var gpuPriority = key?.GetValue("GPU Priority");
            var applied = gpuPriority is int i && i == 8;

            return Task.FromResult(new OptimizationStatus
            {
                IsAvailable = true,
                IsApplied = applied,
                DetailText = applied ? "Currently: High priority" : "Currently: Default"
            });
        }
        catch
        {
            return Task.FromResult(new OptimizationStatus { IsAvailable = false, UnavailableReason = "Could not read the task scheduler profile." });
        }
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(RegistryHive.LocalMachine, SubKey, "GPU Priority", 8, RegistryValueKind.DWord, "GPU priority");
        recorder.SetValue(RegistryHive.LocalMachine, SubKey, "Priority", 6, RegistryValueKind.DWord, "CPU priority");
        recorder.SetValue(RegistryHive.LocalMachine, SubKey, "Scheduling Category", "High", RegistryValueKind.String, "Scheduling category");
        recorder.SetValue(RegistryHive.LocalMachine, SubKey, "SFIO Priority", "High", RegistryValueKind.String, "Storage I/O priority");

        return Task.FromResult(OptimizationApplyResult.Ok("Gaming task scheduler priority raised.", recorder.ChangeLog));
    }
}
