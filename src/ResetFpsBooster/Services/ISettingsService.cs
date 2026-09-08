using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface ISettingsService
{
    AppSettings Current { get; }
    void Save();
    void ApplyStartWithWindows(bool enabled);

    /// <summary>Switches auto-start between the plain (non-admin) Run key and an elevated
    /// Task Scheduler entry. Returns a user-facing success/failure message.</summary>
    (bool Success, string Message) ApplyStartWithWindowsAsAdmin(bool enabled);
}
