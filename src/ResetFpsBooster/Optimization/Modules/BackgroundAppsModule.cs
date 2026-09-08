using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Globally disables Store (UWP) apps from running in the background, freeing CPU/RAM/network they'd otherwise use while not in focus.</summary>
public sealed class BackgroundAppsModule : IOptimizationModule
{
    private const string SubKey = @"Software\Microsoft\Windows\CurrentVersion\BackgroundAccessApplications";
    private const string ValueName = "GlobalUserDisabled";

    public string Id => "windows.background-apps";
    public string Name => "Background Store Apps";
    public string Description => "Prevents Microsoft Store (UWP) apps from running and syncing in the background when you're not using them.";
    public OptimizationCategory Category => OptimizationCategory.Windows;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var value = Registry.CurrentUser.OpenSubKey(SubKey)?.GetValue(ValueName);
        var disabled = value is int i && i == 1;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = disabled,
            DetailText = disabled ? "Currently: Disabled" : "Currently: Allowed"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.CurrentUser, SubKey, ValueName, 1, RegistryValueKind.DWord,
            "Background Store apps",
            v => v is int i && i == 1 ? "Disabled" : "Allowed");

        return Task.FromResult(OptimizationApplyResult.Ok("Background Store apps disabled.", recorder.ChangeLog));
    }
}
