using Microsoft.Win32;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Reads/writes the same "StartupApproved" flag Task Manager's Startup tab uses to enable or
/// disable a startup entry without deleting it — fully reversible.
/// </summary>
public static class StartupApprovedHelper
{
    public const string HkcuRunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    public const string HkcuApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    public const string HklmRunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    public const string HklmApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";

    public static bool IsEnabled(RegistryHive hive, string valueName)
    {
        var approvedKey = hive == RegistryHive.CurrentUser ? HkcuApprovedKey : HklmApprovedKey;
        using var baseKey = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
        using var key = baseKey.OpenSubKey(approvedKey);
        var data = key?.GetValue(valueName) as byte[];

        // No approved-state entry at all means Windows treats it as enabled by default.
        if (data is null || data.Length == 0) return true;
        return data[0] != 0x03;
    }

    public static void SetEnabled(RegistryChangeRecorder recorder, RegistryHive hive, string valueName, bool enabled, string friendlyName)
    {
        var approvedKey = hive == RegistryHive.CurrentUser ? HkcuApprovedKey : HklmApprovedKey;

        var data = new byte[12];
        data[0] = enabled ? (byte)0x02 : (byte)0x03; // 0x02 = enabled, 0x03 = user-disabled (same flag Task Manager's Startup tab uses)

        static string Format(object? v) => v is byte[] b && b.Length > 0 && b[0] == 0x03 ? "Disabled" : "Enabled";

        recorder.SetValue(
            hive, approvedKey, valueName, data, RegistryValueKind.Binary,
            $"Startup: {friendlyName}",
            Format);
    }
}
