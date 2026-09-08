namespace ResetFpsBooster.Core.Models;

public enum GameSource
{
    Steam,
    Epic,
    Manual
}

public sealed class GameProfile
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get; set; } = string.Empty;
    public GameSource Source { get; set; }
    public string InstallPath { get; set; } = string.Empty;
    public string? ExecutablePath { get; set; }
    public bool IsOptimized { get; set; }
    public DateTime? LastOptimizedAt { get; set; }
    public string? LastBackupSnapshotId { get; set; }

    public bool IsAutoexecApplied { get; set; }
    public DateTime? AutoexecAppliedAt { get; set; }
    public string? AutoexecSnapshotId { get; set; }
}
