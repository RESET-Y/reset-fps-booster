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

    /// <summary>GitHub repository to check for updates against, as "owner/repo". Defaults to the
    /// official RESET FPS BOOSTER repo so update checks work out of the box with no setup —
    /// still overridable in Settings for anyone running a fork.</summary>
    public string UpdateRepository { get; set; } = "RESET-Y/reset-fps-booster";
    public bool AutoCheckForUpdates { get; set; } = true;
    public DateTime? LastUpdateCheckUtc { get; set; }
}
