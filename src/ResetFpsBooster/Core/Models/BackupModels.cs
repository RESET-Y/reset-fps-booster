using Microsoft.Win32;

namespace ResetFpsBooster.Core.Models;

public sealed class RegistryValueBackup
{
    public RegistryHive Hive { get; set; }
    public string SubKey { get; set; } = string.Empty;
    public string ValueName { get; set; } = string.Empty;
    public bool ValueExisted { get; set; }
    public string? SerializedOldValue { get; set; }
    public RegistryValueKind OldKind { get; set; } = RegistryValueKind.Unknown;
}

public sealed class PowerPlanBackup
{
    public Guid PreviousActiveSchemeGuid { get; set; }
}

/// <summary>Captures a file's previous state so it can be restored or removed again.
/// <see cref="FileExisted"/> false means the file didn't exist before — restoring deletes it.</summary>
public sealed class FileContentBackup
{
    public string TargetPath { get; set; } = string.Empty;
    public bool FileExisted { get; set; }
    public string? PreviousContentBase64 { get; set; }
}

/// <summary>Captures an NVIDIA driver (DRS) global setting's previous state. Unlike the registry,
/// NVIDIA's driver settings database has no generic "old value" readback for arbitrary types, so
/// <see cref="WasCustomValue"/> distinguishes "restore to this exact value" from "this had no
/// custom override before — restore to the driver's own default".</summary>
public sealed class NvidiaSettingBackup
{
    public uint SettingId { get; set; }
    public string SettingName { get; set; } = string.Empty;
    public bool WasCustomValue { get; set; }
    public uint? OldValue { get; set; }
}

public sealed class BackupSnapshot
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public DateTime CreatedAt { get; set; } = DateTime.Now;
    public string Description { get; set; } = string.Empty;
    public List<string> ModuleIds { get; set; } = new();
    public List<RegistryValueBackup> RegistryEntries { get; set; } = new();
    public List<FileContentBackup> FileEntries { get; set; } = new();
    public List<NvidiaSettingBackup> NvidiaEntries { get; set; } = new();
    public PowerPlanBackup? PowerPlan { get; set; }
    public bool Restored { get; set; }
    public DateTime? RestoredAt { get; set; }
}
