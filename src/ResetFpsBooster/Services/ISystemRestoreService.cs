namespace ResetFpsBooster.Services;

public interface ISystemRestoreService
{
    /// <summary>Creates a real Windows System Restore point — covers the whole system (registry,
    /// drivers, system files), not just what RESET FPS BOOSTER itself has touched. Requires
    /// Administrator and System Restore to be enabled on the system drive. Windows limits how
    /// often a new restore point can be created (once per 24h by default), which is reported
    /// back honestly rather than silently failing.</summary>
    (bool Success, string Message) CreateRestorePoint(string description);
}
