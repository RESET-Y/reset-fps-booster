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

public sealed class BackupSnapshot
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public DateTime CreatedAt { get; set; } = DateTime.Now;
    public string Description { get; set; } = string.Empty;
    public List<string> ModuleIds { get; set; } = new();
    public List<RegistryValueBackup> RegistryEntries { get; set; } = new();
    public PowerPlanBackup? PowerPlan { get; set; }
    public bool Restored { get; set; }
    public DateTime? RestoredAt { get; set; }
}
