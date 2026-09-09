using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using NvAPIWrapper.DRS;
using NvAPIWrapper.DRS.SettingValues;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>
/// Sets NVIDIA's global 3D driver settings the same way the NVIDIA Control Panel's "Manage 3D
/// Settings" page does, via NVIDIA's own public NVAPI — nothing here is a registry hack or an
/// undocumented trick. Applies to the base/global profile, so it affects every game unless that
/// game already has its own per-application override in the driver.
/// </summary>
public sealed class NvidiaMaxPerformanceModule : IOptimizationModule
{
    public string Id => "gpu.nvidia-max-performance";
    public string Name => "NVIDIA Max Performance";
    public string Description => "Sets NVIDIA's global driver profile to Power Management: Prefer Maximum Performance, Threaded Optimization: On, Texture Filtering: High Performance, and V-Sync: Off — the same settings the NVIDIA Control Panel exposes under \"Manage 3D Settings\".";
    public OptimizationCategory Category => OptimizationCategory.Gpu;
    // Medium: this is a global (not per-game) driver profile change, and forcing V-Sync off can
    // introduce screen tearing — a deliberate performance-over-image-quality trade-off, not a bug.
    public RiskLevel Risk => RiskLevel.Medium;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        return Task.Run(() =>
        {
            try
            {
                using var session = DriverSettingsSession.CreateAndLoad();
                var profile = session.BaseProfile;
                if (profile is null)
                    return new OptimizationStatus { IsAvailable = false, UnavailableReason = "Could not access the NVIDIA driver's global profile." };

                var perf = profile.GetSetting(KnownSettingId.PreferredPerformanceState);
                var applied = perf is { IsCurrentValuePredefined: false } &&
                    Convert.ToUInt32(perf.CurrentValue) == (uint)PreferredPerformanceState.PreferMaximum;

                return new OptimizationStatus
                {
                    IsAvailable = true,
                    IsApplied = applied,
                    DetailText = applied ? "Currently: Prefer Maximum Performance" : "Currently: Driver default"
                };
            }
            catch (Exception ex)
            {
                return new OptimizationStatus { IsAvailable = false, UnavailableReason = $"No NVIDIA GPU/driver detected, or NVAPI is unavailable: {ex.Message}" };
            }
        }, ct);
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        return Task.Run(() =>
        {
            try
            {
                using var session = DriverSettingsSession.CreateAndLoad();
                var profile = session.BaseProfile;
                if (profile is null)
                    return OptimizationApplyResult.Fail("Could not access the NVIDIA driver's global profile.");

                RecordAndSet(recorder, profile, KnownSettingId.PreferredPerformanceState, "Power management mode",
                    (uint)PreferredPerformanceState.PreferMaximum, "Prefer Maximum Performance");

                RecordAndSet(recorder, profile, KnownSettingId.OpenGLThreadControl, "Threaded optimization",
                    (uint)OpenGLThreadControl.Enable, "On");

                RecordAndSet(recorder, profile, KnownSettingId.QualityEnhancements, "Texture filtering quality",
                    (uint)NvAPIWrapper.DRS.SettingValues.QualityEnhancements.HighPerformance, "High Performance");

                RecordAndSet(recorder, profile, KnownSettingId.VSyncMode, "Vertical sync",
                    (uint)VSyncMode.ForceOff, "Off");

                session.Save();

                return OptimizationApplyResult.Ok(
                    "NVIDIA global 3D settings set to Max Performance (Power Management, Threaded Optimization, Texture Filtering, V-Sync).",
                    recorder.ChangeLog);
            }
            catch (Exception ex)
            {
                return OptimizationApplyResult.Fail($"Could not change NVIDIA driver settings: {ex.Message}");
            }
        }, ct);
    }

    private static void RecordAndSet(
        RegistryChangeRecorder recorder,
        DriverSettingsProfile profile,
        KnownSettingId settingId,
        string friendlyName,
        uint newValue,
        string newValueDisplay)
    {
        var existing = profile.GetSetting(settingId);
        var wasCustom = existing is { IsCurrentValuePredefined: false };
        uint? oldValue = wasCustom ? Convert.ToUInt32(existing!.CurrentValue) : null;

        recorder.RecordNvidiaSetting(
            (uint)settingId,
            friendlyName,
            wasCustom,
            oldValue,
            wasCustom ? oldValue.ToString()! : "Driver default",
            newValueDisplay);

        profile.SetSetting(settingId, newValue);
    }
}
