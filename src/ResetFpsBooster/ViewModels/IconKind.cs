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
#endif
    System,
    Backups,
    Logs,
    Settings
}
