using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Disables Xbox Game Bar's background recording (Game DVR), which continuously captures gameplay in a buffer and costs CPU/GPU/disk overhead.</summary>
public sealed class GameDvrModule : IOptimizationModule
{
    private const string GameBarKey = @"Software\Microsoft\GameBar";
    private const string GameConfigKey = @"System\GameConfigStore";

    public string Id => "gaming.game-dvr";
    public string Name => "Background Game Recording (Game DVR)";
    public string Description => "Disables the Xbox Game Bar background capture buffer that continuously records gameplay, freeing up CPU, GPU and disk I/O.";
    public OptimizationCategory Category => OptimizationCategory.Gaming;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var dvrValue = Registry.CurrentUser.OpenSubKey(GameConfigKey)?.GetValue("GameDVR_Enabled");
        var disabled = dvrValue is int i && i == 0;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = disabled,
            DetailText = disabled ? "Currently: Disabled" : "Currently: Enabled"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        recorder.SetValue(
            RegistryHive.CurrentUser, GameConfigKey, "GameDVR_Enabled", 0, RegistryValueKind.DWord,
            "Game DVR background recording",
            v => v is int i && i == 1 ? "Enabled" : "Disabled");

        recorder.SetValue(
            RegistryHive.CurrentUser, GameBarKey, "AllowAutoGameMode", 1, RegistryValueKind.DWord,
            "Game Bar auto-detection");

        recorder.SetValue(
            RegistryHive.CurrentUser, GameBarKey, "UseNexusForGameBarEnabled", 0, RegistryValueKind.DWord,
            "Game Bar overlay on Win+G");

        return Task.FromResult(OptimizationApplyResult.Ok("Background game recording disabled.", recorder.ChangeLog));
    }
}
