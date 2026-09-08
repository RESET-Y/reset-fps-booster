namespace ResetFpsBooster.Core.Models;

public sealed class OptimizationStatus
{
    /// <summary>The optimization can technically be evaluated/applied on this machine.</summary>
    public bool IsAvailable { get; set; } = true;
    /// <summary>The optimized state is already active.</summary>
    public bool IsApplied { get; set; }
    /// <summary>Human readable detail shown under the module, e.g. "Currently: Balanced".</summary>
    public string DetailText { get; set; } = string.Empty;
    /// <summary>Reason the module is unavailable (missing hardware, unsupported OS, etc.).</summary>
    public string? UnavailableReason { get; set; }
}

public sealed class ChangeLogEntry
{
    public DateTime Timestamp { get; set; } = DateTime.Now;
    public string ModuleName { get; set; } = string.Empty;
    public string SettingName { get; set; } = string.Empty;
    public string OldValue { get; set; } = string.Empty;
    public string NewValue { get; set; } = string.Empty;
    public string? SnapshotId { get; set; }
}

public sealed class OptimizationApplyResult
{
    public bool Success { get; set; }
    public string Message { get; set; } = string.Empty;
    public bool RequiresReboot { get; set; }
    public List<ChangeLogEntry> Changes { get; set; } = new();
    public PowerPlanBackup? PowerPlanBackup { get; set; }

    public static OptimizationApplyResult Ok(string message, List<ChangeLogEntry>? changes = null, bool requiresReboot = false)
        => new() { Success = true, Message = message, Changes = changes ?? new(), RequiresReboot = requiresReboot };

    public static OptimizationApplyResult Fail(string message)
        => new() { Success = false, Message = message };

    public static OptimizationApplyResult Skipped(string message)
        => new() { Success = true, Message = message };
}

public sealed class ScanStep
{
    public string Name { get; set; } = string.Empty;
    public bool Completed { get; set; }
    public bool HasFindings { get; set; }
    public string Summary { get; set; } = string.Empty;
}
