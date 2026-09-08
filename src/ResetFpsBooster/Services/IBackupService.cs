using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public interface IBackupService
{
    IReadOnlyList<BackupSnapshot> ListSnapshots();

    /// <summary>Persists everything a recorder captured as one restorable snapshot and appends it to the changelog.</summary>
    BackupSnapshot CommitSnapshot(RegistryChangeRecorder recorder, string description, PowerPlanBackup? powerPlanBackup = null);

    /// <summary>Reverts every recorded registry value in the snapshot back to its pre-change state.</summary>
    (bool Success, string Message) RestoreSnapshot(string snapshotId);
}
