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
    SmoothMotion,
#endif
    System,
    Backups,
    Logs,
    Account,
    Manager,
    Settings
}
