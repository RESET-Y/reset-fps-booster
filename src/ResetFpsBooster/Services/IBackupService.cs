using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public interface IBackupService
{
    IReadOnlyList<BackupSnapshot> ListSnapshots();

    /// <summary>Persists everything a recorder captured as one restorable snapshot and appends it to the changelog.</summary>
    BackupSnapshot CommitSnapshot(RegistryChangeRecorder recorder, string description, PowerPlanBackup? powerPlanBackup = null, IReadOnlyList<FileContentBackup>? fileEntries = null);

    /// <summary>Persists a snapshot made up only of file backups (no registry changes), e.g. for config-file tweaks.</summary>
    BackupSnapshot CommitFileSnapshot(string moduleName, string description, IReadOnlyList<FileContentBackup> fileEntries);

    /// <summary>Reverts every recorded registry value and file in the snapshot back to its pre-change state.</summary>
    (bool Success, string Message) RestoreSnapshot(string snapshotId);
}
