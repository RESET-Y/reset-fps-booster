using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Services;

public sealed class StartupAppsService : IStartupAppsService
{
    // Heuristic list of common non-essential background helpers safe to suggest disabling.
    // Gaming platforms (Steam, Epic, Discord, Riot, Battle.net, NVIDIA/AMD) are deliberately excluded.
    private static readonly string[] RecommendedToDisable =
    {
        "onedrive", "spotify", "skype", "teams", "adobe", "creativecloud",
        "ccleaner", "itunes", "quicktime", "dropbox"
    };

    public List<StartupItem> GetStartupItems()
    {
        var items = new List<StartupItem>();
        ReadRunKey(RegistryHive.CurrentUser, items);
        ReadRunKey(RegistryHive.LocalMachine, items);
        return items.OrderBy(i => i.Name).ToList();
    }

    public void SetEnabled(RegistryChangeRecorder recorder, StartupItem item, bool enabled)
    {
        StartupApprovedHelper.SetEnabled(recorder, item.Hive, item.Name, enabled, item.Name);
        item.IsEnabled = enabled;
    }

    private static void ReadRunKey(RegistryHive hive, List<StartupItem> items)
    {
        try
        {
            using var baseKey = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
            using var runKey = baseKey.OpenSubKey(StartupApprovedHelper.HkcuRunKey);
            if (runKey is null) return;

            foreach (var valueName in runKey.GetValueNames())
            {
                if (string.IsNullOrWhiteSpace(valueName)) continue;

                var command = runKey.GetValue(valueName) as string ?? string.Empty;
                var lowerName = valueName.ToLowerInvariant();

                items.Add(new StartupItem
                {
                    Name = valueName,
                    Command = command,
                    Hive = hive,
                    IsEnabled = StartupApprovedHelper.IsEnabled(hive, valueName),
                    IsRecommendedToDisable = RecommendedToDisable.Any(candidate => lowerName.Contains(candidate))
                });
            }
        }
        catch
        {
            // Leave whatever was already collected — startup enumeration is best-effort.
        }
    }
}
