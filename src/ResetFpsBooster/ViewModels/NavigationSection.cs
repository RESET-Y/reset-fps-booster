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
    Crosshair,
    SensFinder,
    ApexConfig,
    System,
    Backups,
    Logs,
    Account,
    Manager,
    Settings
}
