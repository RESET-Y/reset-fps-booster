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

    /// <summary>Accent color as "#RRGGBB". Applied on the next app start (see ThemeColorHelper).</summary>
    public string AccentColorHex { get; set; } = "#E8121F";

    /// <summary>When true, the app launches at logon already elevated (via a Task Scheduler
    /// entry with "Run with highest privileges") instead of the plain, non-admin Run key —
    /// so there's no UAC prompt to click through every boot.</summary>
    public bool StartWithWindowsAsAdmin { get; set; }

    /// <summary>When true, a background watcher deprioritizes other processes while a detected
    /// game is running and restores them the moment it exits. Streaming/broadcast and voice-chat
    /// software is always left untouched. Off by default — this runs continuously in the
    /// background, so it's opt-in rather than a one-time tweak.</summary>
    public bool EnableGameBoost { get; set; }
}
