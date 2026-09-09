namespace ResetFpsBooster.ViewModels;

public enum NavigationSection
{
    Dashboard,
    Optimizer,
    GameProfiles,
    Performance,
    BottleneckEngine,
#if RFB_BETA
    FrameBoostBeta,
#endif
    System,
    Backups,
    Logs,
    Settings
}
