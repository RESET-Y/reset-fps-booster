using System.Diagnostics;
using System.IO;
using System.Linq;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Services;

public sealed class BackupService : IBackupService
{
    private readonly IChangeLogService _changeLog;

    public BackupService(IChangeLogService changeLog)
    {
        _changeLog = changeLog;
        AppPaths.EnsureFoldersExist();
    }

    public IReadOnlyList<BackupSnapshot> ListSnapshots()
    {
        if (!Directory.Exists(AppPaths.BackupsFolder))
            return Array.Empty<BackupSnapshot>();

        var snapshots = new List<BackupSnapshot>();
        foreach (var file in Directory.GetFiles(AppPaths.BackupsFolder, "*.json"))
        {
            var snapshot = JsonStore.Load<BackupSnapshot?>(file, () => null);
            if (snapshot is not null)
                snapshots.Add(snapshot);
        }

        return snapshots.OrderByDescending(s => s.CreatedAt).ToList();
    }

    public BackupSnapshot CommitSnapshot(RegistryChangeRecorder recorder, string description, PowerPlanBackup? powerPlanBackup = null)
    {
        var snapshot = new BackupSnapshot
        {
            Description = description,
            ModuleIds = new List<string> { recorder.ModuleName },
            RegistryEntries = recorder.BackupEntries,
            PowerPlan = powerPlanBackup
        };

        var path = Path.Combine(AppPaths.BackupsFolder, $"{snapshot.CreatedAt:yyyyMMdd_HHmmss}_{snapshot.Id}.json");
        JsonStore.Save(path, snapshot);

        foreach (var entry in recorder.ChangeLog)
            entry.SnapshotId = snapshot.Id;

        _changeLog.Append(recorder.ChangeLog);

        return snapshot;
    }

    public (bool Success, string Message) RestoreSnapshot(string snapshotId)
    {
        var file = Directory.GetFiles(AppPaths.BackupsFolder, "*.json")
            .FirstOrDefault(f => f.Contains(snapshotId, StringComparison.OrdinalIgnoreCase));

        if (file is null)
            return (false, "Backup snapshot not found.");

        var snapshot = JsonStore.Load<BackupSnapshot?>(file, () => null);
        if (snapshot is null)
            return (false, "Backup snapshot could not be read.");

        var restoredEntries = new List<ChangeLogEntry>();
        var failures = 0;

        foreach (var entry in snapshot.RegistryEntries)
        {
            try
            {
                using var baseKey = RegistryKey.OpenBaseKey(entry.Hive, RegistryView.Registry64);
                using var key = baseKey.OpenSubKey(entry.SubKey, writable: true);
                if (key is null) continue;

                if (!entry.ValueExisted)
                {
                    if (key.GetValueNames().Contains(entry.ValueName, StringComparer.OrdinalIgnoreCase))
                        key.DeleteValue(entry.ValueName);
                }
                else
                {
                    var value = RegistryChangeRecorder.DeserializeValue(entry.SerializedOldValue, entry.OldKind);
                    if (value is not null)
                        key.SetValue(entry.ValueName, value, entry.OldKind);
                }

                restoredEntries.Add(new ChangeLogEntry
                {
                    ModuleName = "Restore",
                    SettingName = entry.ValueName,
                    OldValue = "(optimized)",
                    NewValue = "(restored to previous value)",
                    SnapshotId = snapshot.Id
                });
            }
            catch (Exception ex)
            {
                failures++;
                Debug.WriteLine($"Failed to restore {entry.SubKey}\\{entry.ValueName}: {ex.Message}");
            }
        }

        if (snapshot.PowerPlan is not null)
        {
            TryRestorePowerPlan(snapshot.PowerPlan.PreviousActiveSchemeGuid);
        }

        snapshot.Restored = true;
        snapshot.RestoredAt = DateTime.Now;
        JsonStore.Save(file, snapshot);

        _changeLog.Append(restoredEntries);

        if (failures == 0)
            return (true, $"Restored {restoredEntries.Count} setting(s) from backup created on {snapshot.CreatedAt:g}.");

        return (true, $"Restored {restoredEntries.Count} setting(s), but {failures} could not be reverted (they may already be at their default value).");
    }

    private static void TryRestorePowerPlan(Guid schemeGuid)
    {
        try
        {
            var psi = new ProcessStartInfo("powercfg.exe", $"/setactive {schemeGuid:D}")
            {
                CreateNoWindow = true,
                UseShellExecute = false
            };
            using var process = Process.Start(psi);
            process?.WaitForExit(5000);
        }
        catch
        {
            // Best-effort — power plan restore is not critical enough to fail the whole restore operation.
        }
    }
}
