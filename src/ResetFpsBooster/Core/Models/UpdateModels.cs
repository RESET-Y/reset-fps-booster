namespace ResetFpsBooster.Core.Models;

public sealed class UpdateCheckResult
{
    public bool Success { get; set; }
    public bool IsUpdateAvailable { get; set; }
    public string CurrentVersion { get; set; } = string.Empty;
    public string? LatestVersion { get; set; }
    public string? DownloadUrl { get; set; }
    public string? ReleaseNotes { get; set; }
    public string? ErrorMessage { get; set; }
}

public sealed class UpdateDownloadProgress
{
    public long BytesReceived { get; set; }
    public long? TotalBytes { get; set; }
    public double? PercentComplete => TotalBytes is > 0 ? (double)BytesReceived / TotalBytes.Value * 100 : null;
}
