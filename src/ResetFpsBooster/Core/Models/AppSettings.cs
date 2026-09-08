namespace ResetFpsBooster.Core.Models;

public sealed class AppSettings
{
    public AppTheme Theme { get; set; } = AppTheme.Dark;
    public bool StartWithWindows { get; set; }
    public bool ShowToastNotifications { get; set; } = true;
    public bool MinimizeToTray { get; set; } = true;
    public bool AutoScanOnStartup { get; set; }
    public bool ConfirmBeforeApplyingChanges { get; set; } = true;
    public bool EnableExperimentalOptimizations { get; set; }

    /// <summary>GitHub repository to check for updates against, as "owner/repo". Empty until configured.</summary>
    public string UpdateRepository { get; set; } = string.Empty;
    public bool AutoCheckForUpdates { get; set; } = true;
    public DateTime? LastUpdateCheckUtc { get; set; }
}
