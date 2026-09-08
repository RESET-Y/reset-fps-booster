using Microsoft.Win32;

namespace ResetFpsBooster.Core.Models;

public sealed class StartupItem
{
    public string Name { get; set; } = string.Empty;
    public string Command { get; set; } = string.Empty;
    public RegistryHive Hive { get; set; }
    public bool IsEnabled { get; set; }
    public bool IsRecommendedToDisable { get; set; }
}
