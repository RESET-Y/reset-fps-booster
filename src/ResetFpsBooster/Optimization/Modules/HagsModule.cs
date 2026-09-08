using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Enables Hardware-accelerated GPU Scheduling (HAGS), letting the GPU manage its own memory queue instead of relying solely on the Windows kernel scheduler. Supported on Windows 10 2004+ with a WDDM 2.7+ driver.</summary>
public sealed class HagsModule : IOptimizationModule
{
    private const string SubKey = @"SYSTEM\CurrentControlSet\Control\GraphicsDrivers";
    private const string ValueName = "HwSchMode";

    public string Id => "gpu.hags";
    public string Name => "Hardware-Accelerated GPU Scheduling";
    public string Description => "Lets a compatible GPU manage its own video memory queue for lower latency. Requires a restart to take effect.";
    public OptimizationCategory Category => OptimizationCategory.Gpu;
    public RiskLevel Risk => RiskLevel.Medium;
    public bool RequiresAdmin => true;
    public bool RequiresReboot => true;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(SubKey);
            if (key is null || !key.GetValueNames().Contains(ValueName))
            {
                return Task.FromResult(new OptimizationStatus
                {
                    IsAvailable = false,
                    UnavailableReason = "Your GPU driver does not expose the HAGS setting."
                });
            }

            var value = Convert.ToInt32(key.GetValue(ValueName));
            return Task.FromResult(new OptimizationStatus
            {
                IsAvailable = true,
                IsApplied = value == 2,
                DetailText = value == 2 ? "Currently: On" : "Currently: Off"
            });
        }
        catch
        {
            return Task.FromResult(new OptimizationStatus { IsAvailable = false, UnavailableReason = "Could not read GPU scheduling state." });
        }
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.LocalMachine, SubKey, ValueName, 2, RegistryValueKind.DWord,
            "Hardware-accelerated GPU scheduling",
            v => v is int i && i == 2 ? "On" : "Off");

        return Task.FromResult(OptimizationApplyResult.Ok(
            "GPU scheduling enabled. Restart your PC for it to take effect.",
            recorder.ChangeLog,
            requiresReboot: true));
    }
}
