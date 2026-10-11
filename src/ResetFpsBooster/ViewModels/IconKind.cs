namespace ResetFpsBooster.ViewModels;

public enum IconKind
{
    Dashboard,
    Optimizer,
    Games,
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
