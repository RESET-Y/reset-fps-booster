using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Disables the multimedia network throttling index, which by default caps non-multimedia network traffic to ~10,000 packets/sec while an application uses the Multimedia Class Scheduler Service.</summary>
public sealed class NetworkThrottlingModule : IOptimizationModule
{
    private const string SubKey = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile";
    private const string ValueName = "NetworkThrottlingIndex";
    private const int Disabled = unchecked((int)0xFFFFFFFF);

    public string Id => "network.throttling-index";
    public string Name => "Network Throttling Index";
    public string Description => "Removes the ~10,000 packets/sec cap Windows applies to network traffic while a multimedia/gaming task is active.";
    public OptimizationCategory Category => OptimizationCategory.Network;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => true;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var value = Registry.LocalMachine.OpenSubKey(SubKey)?.GetValue(ValueName);
        var disabled = value is int i && i == Disabled;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = disabled,
            DetailText = disabled ? "Currently: Unrestricted" : "Currently: Default (throttled)"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.LocalMachine, SubKey, ValueName, Disabled, RegistryValueKind.DWord,
            "Network throttling index",
            v => v is int i && i == Disabled ? "Unrestricted" : "Default");

        return Task.FromResult(OptimizationApplyResult.Ok("Network throttling for multimedia tasks disabled.", recorder.ChangeLog));
    }
}
