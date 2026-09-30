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
    System,
    Backups,
    Logs,
    Account,
    Manager,
    Settings
}
