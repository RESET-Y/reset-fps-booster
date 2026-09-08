using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Turns on Windows Game Mode, which deprioritizes background work while a game is in focus.</summary>
public sealed class GameModeModule : IOptimizationModule
{
    private const string SubKey = @"Software\Microsoft\GameBar";
    private const string ValueName = "AutoGameModeEnabled";

    public string Id => "gaming.game-mode";
    public string Name => "Windows Game Mode";
    public string Description => "Lets Windows prioritize your game's CPU and GPU scheduling over background tasks while it has focus.";
    public OptimizationCategory Category => OptimizationCategory.Gaming;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var value = Registry.CurrentUser.OpenSubKey(SubKey)?.GetValue(ValueName);
        var enabled = value is int i && i == 1;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = enabled,
            DetailText = enabled ? "Currently: On" : "Currently: Off"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.CurrentUser, SubKey, ValueName, 1, RegistryValueKind.DWord,
            "Windows Game Mode",
            v => v is int i && i == 1 ? "On" : "Off");

        return Task.FromResult(OptimizationApplyResult.Ok("Game Mode enabled.", recorder.ChangeLog));
    }
}
