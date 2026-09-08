using System.Diagnostics;
using System.Text.RegularExpressions;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>Switches the active Windows power plan to High Performance, which prevents the CPU from aggressively downclocking to save power.</summary>
public sealed partial class PowerPlanModule : IOptimizationModule
{
    private static readonly Guid HighPerformanceGuid = new("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c");
    private static readonly Guid BalancedGuid = new("381b4222-f694-41f0-9685-ff5bb260df2e");

    public string Id => "windows.power-plan";
    public string Name => "High Performance Power Plan";
    public string Description => "Switches Windows' active power plan from Balanced to High Performance so the CPU doesn't downclock during gaming to save power.";
    public OptimizationCategory Category => OptimizationCategory.Windows;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public async Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        var (guid, name) = await GetActiveSchemeAsync(ct);
        var isHighOrBetter = guid == HighPerformanceGuid || IsUltimateOrHigherByName(name);

        return new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = isHighOrBetter,
            DetailText = $"Currently: {name}"
        };
    }

    public async Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        var (currentGuid, currentName) = await GetActiveSchemeAsync(ct);

        if (currentGuid == HighPerformanceGuid || IsUltimateOrHigherByName(currentName))
            return OptimizationApplyResult.Skipped($"{currentName} is already active — that's at least as fast as High Performance.");

        var success = await RunPowerCfgAsync($"/setactive {HighPerformanceGuid:D}", ct);
        if (!success)
            return OptimizationApplyResult.Fail("Could not switch the power plan. No changes were made.");

        recorder.ChangeLog.Add(new ChangeLogEntry
        {
            ModuleName = Name,
            SettingName = "Active power plan",
            OldValue = currentName,
            NewValue = "High performance"
        });

        var result = OptimizationApplyResult.Ok("Switched to the High Performance power plan.", recorder.ChangeLog);
        result.PowerPlanBackup = new PowerPlanBackup { PreviousActiveSchemeGuid = currentGuid == Guid.Empty ? BalancedGuid : currentGuid };
        return result;
    }

    private static bool IsUltimateOrHigherByName(string name)
    {
        var lower = name.ToLowerInvariant();
        return lower.Contains("ultimate") || lower.Contains("ultimativ") || lower.Contains("höchstleistung");
    }

    private static async Task<(Guid Guid, string Name)> GetActiveSchemeAsync(CancellationToken ct)
    {
        var output = await RunPowerCfgCaptureAsync("/getactivescheme", ct);
        var match = GuidRegex().Match(output);
        if (!match.Success || !Guid.TryParse(match.Value, out var guid))
            return (Guid.Empty, "Unknown");

        var nameMatch = NameRegex().Match(output);
        var name = nameMatch.Success ? nameMatch.Groups[1].Value.Trim() : "Unknown";
        return (guid, name);
    }

    private static async Task<bool> RunPowerCfgAsync(string arguments, CancellationToken ct)
    {
        try
        {
            var psi = new ProcessStartInfo("powercfg.exe", arguments)
            {
                CreateNoWindow = true,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using var process = Process.Start(psi);
            if (process is null) return false;
            await process.WaitForExitAsync(ct);
            return process.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    private static async Task<string> RunPowerCfgCaptureAsync(string arguments, CancellationToken ct)
    {
        try
        {
            var psi = new ProcessStartInfo("powercfg.exe", arguments)
            {
                CreateNoWindow = true,
                UseShellExecute = false,
                RedirectStandardOutput = true
            };
            using var process = Process.Start(psi);
            if (process is null) return string.Empty;
            var output = await process.StandardOutput.ReadToEndAsync(ct);
            await process.WaitForExitAsync(ct);
            return output;
        }
        catch
        {
            return string.Empty;
        }
    }

    [GeneratedRegex(@"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")]
    private static partial Regex GuidRegex();

    [GeneratedRegex(@"\(([^)]+)\)")]
    private static partial Regex NameRegex();
}
