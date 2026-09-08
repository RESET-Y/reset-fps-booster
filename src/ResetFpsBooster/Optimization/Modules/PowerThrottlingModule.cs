using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>
/// Disables Windows' global CPU Power Throttling (a.k.a. "Modern Standby power management"),
/// which can down-clock background/EcoQoS-tagged processes. On laptops this trades some
/// battery life for consistency, so it is flagged Medium risk and surfaced clearly.
/// </summary>
public sealed class PowerThrottlingModule : IOptimizationModule
{
    private const string SubKey = @"SYSTEM\CurrentControlSet\Control\Power\PowerThrottling";
    private const string ValueName = "PowerThrottlingOff";

    public string Id => "cpu.power-throttling";
    public string Name => "CPU Power Throttling";
    public string Description => "Disables Windows' automatic power throttling of background processes. On laptops this can reduce battery life on battery power.";
    public OptimizationCategory Category => OptimizationCategory.Cpu;
    public RiskLevel Risk => RiskLevel.Medium;
    public bool RequiresAdmin => true;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var value = Registry.LocalMachine.OpenSubKey(SubKey)?.GetValue(ValueName);
        var disabled = value is int i && i == 1;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = disabled,
            DetailText = disabled ? "Currently: Disabled" : "Currently: Windows default"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.LocalMachine, SubKey, ValueName, 1, RegistryValueKind.DWord,
            "CPU power throttling",
            v => v is int i && i == 1 ? "Disabled" : "Windows default");

        return Task.FromResult(OptimizationApplyResult.Ok("CPU power throttling disabled.", recorder.ChangeLog));
    }
}
