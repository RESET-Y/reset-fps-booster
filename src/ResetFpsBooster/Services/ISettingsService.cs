using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface ISettingsService
{
    AppSettings Current { get; }
    void Save();
    void ApplyStartWithWindows(bool enabled);
}
