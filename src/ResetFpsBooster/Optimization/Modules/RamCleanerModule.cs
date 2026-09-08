using System.Diagnostics;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Hardware;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>
/// A genuine RAM reclaim, not a "RAM BOOST!" gimmick: it (1) trims the working set of idle
/// background processes so they give back memory pages they aren't actively using, and
/// (2) purges Windows' Standby List — the reclaimable file-cache Task Manager still counts as
/// "in use" — using the same mechanism Sysinternals RAMMap exposes. Nothing is closed, no data
/// is lost (standby pages are just cache; trimmed processes simply re-fault pages back in on
/// next use), and the foreground process plus this app itself are never touched.
/// </summary>
public sealed class RamCleanerModule : IOptimizationModule
{
    // Never trim these — critical OS processes, the visible desktop/shell, or anything that
    // would cause a visible hitch for no real benefit.
    private static readonly string[] ExcludedProcessNames =
    {
        "system", "idle", "registry", "smss", "csrss", "wininit", "services", "lsass",
        "winlogon", "memcompression", "dwm", "explorer", "resetfpsbooster", "secure system"
    };

    private const long MinWorkingSetToTrimBytes = 20L * 1024 * 1024; // skip tiny processes — nothing meaningful to reclaim

    public string Id => "memory.ram-cleaner";
    public string Name => "RAM Cleaner";
    public string Description => "Trims memory held by idle background processes and clears Windows' reclaimable file cache (Standby List). Nothing is closed and no data is lost — this only asks the OS to give back pages it isn't actively using.";
    public OptimizationCategory Category => OptimizationCategory.Memory;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => true;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        NativeMemoryStatus.GlobalMemoryStatusEx(out var status);
        var freePercent = status.ullTotalPhys == 0 ? 0 : (double)status.ullAvailPhys / status.ullTotalPhys * 100;

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = freePercent > 25,
            DetailText = $"Available: {FormatBytes((long)status.ullAvailPhys)} ({freePercent:0}% free)"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        PrivilegeHelper.EnablePrivilege("SeProfileSingleProcessPrivilege");
        PrivilegeHelper.EnablePrivilege("SeIncreaseQuotaPrivilege");

        NativeMemoryStatus.GlobalMemoryStatusEx(out var before);

        var foregroundProcessId = GetForegroundProcessId();
        var currentProcessId = Environment.ProcessId;

        var trimmedCount = 0;
        long workingSetFreed = 0;

        foreach (var process in Process.GetProcesses())
        {
            ct.ThrowIfCancellationRequested();

            using (process)
            {
                try
                {
                    if (process.Id == currentProcessId || process.Id == foregroundProcessId) continue;
                    if (ExcludedProcessNames.Contains(process.ProcessName, StringComparer.OrdinalIgnoreCase)) continue;

                    var before64 = process.WorkingSet64;
                    if (before64 < MinWorkingSetToTrimBytes) continue;

                    if (!NativeMethods.EmptyWorkingSet(process.Handle)) continue;

                    process.Refresh();
                    var after64 = process.WorkingSet64;

                    trimmedCount++;
                    workingSetFreed += Math.Max(0, before64 - after64);
                }
                catch
                {
                    // Protected/system-owned process or it exited mid-scan — skip and keep going.
                }
            }
        }

        var standbyListPurged = TryPurgeStandbyList();

        NativeMemoryStatus.GlobalMemoryStatusEx(out var after);

        recorder.ChangeLog.Add(new ChangeLogEntry
        {
            ModuleName = Name,
            SettingName = "Background process working sets",
            OldValue = $"{trimmedCount} process(es) holding {FormatBytes(workingSetFreed)}",
            NewValue = "Trimmed"
        });

        recorder.ChangeLog.Add(new ChangeLogEntry
        {
            ModuleName = Name,
            SettingName = "System available memory",
            OldValue = FormatBytes((long)before.ullAvailPhys),
            NewValue = FormatBytes((long)after.ullAvailPhys)
        });

        var message = standbyListPurged
            ? $"Trimmed {trimmedCount} background process(es) ({FormatBytes(workingSetFreed)}) and cleared the standby cache. Available memory: {FormatBytes((long)before.ullAvailPhys)} → {FormatBytes((long)after.ullAvailPhys)}."
            : $"Trimmed {trimmedCount} background process(es) ({FormatBytes(workingSetFreed)}). Standby cache could not be cleared (needs administrator). Available memory: {FormatBytes((long)before.ullAvailPhys)} → {FormatBytes((long)after.ullAvailPhys)}.";

        return Task.FromResult(OptimizationApplyResult.Ok(message, recorder.ChangeLog));
    }

    private static bool TryPurgeStandbyList()
    {
        try
        {
            var command = NativeMethods.MemoryPurgeStandbyList;
            var result = NativeMethods.NtSetSystemInformation(NativeMethods.SystemMemoryListInformation, ref command, sizeof(int));
            return result == 0; // STATUS_SUCCESS
        }
        catch
        {
            return false;
        }
    }

    private static int GetForegroundProcessId()
    {
        try
        {
            var hwnd = NativeMethods.GetForegroundWindow();
            if (hwnd == IntPtr.Zero) return -1;
            NativeMethods.GetWindowThreadProcessId(hwnd, out var pid);
            return (int)pid;
        }
        catch
        {
            return -1;
        }
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = { "B", "KB", "MB", "GB" };
        double size = bytes;
        var unit = 0;
        while (size >= 1024 && unit < units.Length - 1)
        {
            size /= 1024;
            unit++;
        }
        return $"{size:0.#} {units[unit]}";
    }
}
