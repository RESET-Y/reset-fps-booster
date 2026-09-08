using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Switches Windows visual effects to "best performance", turning off window/menu animations and transparency that cost GPU compositing time. Purely cosmetic — does not touch gameplay settings.</summary>
public sealed class VisualEffectsModule : IOptimizationModule
{
    private const string SubKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects";
    private const string ValueName = "VisualFXSetting";

    public string Id => "windows.visual-effects";
    public string Name => "Visual Effects";
    public string Description => "Turns off window animations, transparency and shadows so the desktop compositor uses less GPU time. Optional — purely cosmetic, does not affect games.";
    public OptimizationCategory Category => OptimizationCategory.Windows;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var value = Registry.CurrentUser.OpenSubKey(SubKey)?.GetValue(ValueName);
        var applied = value is int i && i == 2;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = applied,
            DetailText = applied ? "Currently: Best performance" : "Currently: Let Windows choose / best appearance"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.CurrentUser, SubKey, ValueName, 2, RegistryValueKind.DWord,
            "Visual effects preset",
            v => v is int i && i == 2 ? "Best performance" : "Default");

        ApplyLiveAnimationSettings();

        return Task.FromResult(OptimizationApplyResult.Ok("Visual effects tuned for performance.", recorder.ChangeLog));
    }

    private static void ApplyLiveAnimationSettings()
    {
        // Best-effort: these calls change the running session immediately; failures are non-fatal.
        try
        {
            NativeAnimationSwitch.Disable(NativeAnimationSwitch.Effect.MenuAnimation);
            NativeAnimationSwitch.Disable(NativeAnimationSwitch.Effect.ComboBoxAnimation);
            NativeAnimationSwitch.Disable(NativeAnimationSwitch.Effect.TooltipAnimation);
            NativeAnimationSwitch.Disable(NativeAnimationSwitch.Effect.ListBoxSmoothScrolling);
        }
        catch
        {
            // Cosmetic only.
        }
    }
}
