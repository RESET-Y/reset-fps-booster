using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public interface IStartupAppsService
{
    List<StartupItem> GetStartupItems();
    void SetEnabled(RegistryChangeRecorder recorder, StartupItem item, bool enabled);
}
