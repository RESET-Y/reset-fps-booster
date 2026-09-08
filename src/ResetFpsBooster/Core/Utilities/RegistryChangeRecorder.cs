using ResetFpsBooster.Core.Models;
using Microsoft.Win32;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Applies registry writes while transparently recording the previous value so the
/// change can be reverted later, and produces a human readable change-log entry for each write.
/// This is the single mechanism every optimization module uses to touch the registry,
/// which guarantees every applied change is backed up and reversible.
/// </summary>
public sealed class RegistryChangeRecorder
{
    private const string MultiStringSeparator = "␟";

    public List<RegistryValueBackup> BackupEntries { get; } = new();
    public List<ChangeLogEntry> ChangeLog { get; } = new();
    public string ModuleName { get; }

    public RegistryChangeRecorder(string moduleName)
    {
        ModuleName = moduleName;
    }

    private static RegistryKey OpenBaseKey(RegistryHive hive) =>
        RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);

    public object? ReadCurrentValue(RegistryHive hive, string subKey, string valueName)
    {
        using var baseKey = OpenBaseKey(hive);
        using var key = baseKey.OpenSubKey(subKey, writable: false);
        return key?.GetValue(valueName);
    }

    /// <summary>
    /// Writes a value, recording the old value/kind for restore and appending a friendly
    /// change-log line. Pass null current/new formatters to fall back to raw ToString().
    /// </summary>
    public void SetValue(
        RegistryHive hive,
        string subKey,
        string valueName,
        object newValue,
        RegistryValueKind kind,
        string friendlySettingName,
        Func<object?, string>? formatValue = null)
    {
        using var baseKey = OpenBaseKey(hive);
        using var key = baseKey.CreateSubKey(subKey, writable: true)
            ?? throw new InvalidOperationException("Could not open or create registry key '" + subKey + "'.");

        var existed = key.GetValueNames().Contains(valueName, StringComparer.OrdinalIgnoreCase);
        var oldValue = existed ? key.GetValue(valueName) : null;
        var oldKind = existed ? key.GetValueKind(valueName) : RegistryValueKind.Unknown;

        BackupEntries.Add(new RegistryValueBackup
        {
            Hive = hive,
            SubKey = subKey,
            ValueName = valueName,
            ValueExisted = existed,
            SerializedOldValue = SerializeValue(oldValue, oldKind),
            OldKind = oldKind
        });

        var format = formatValue ?? (v => v?.ToString() ?? "(not set)");

        ChangeLog.Add(new ChangeLogEntry
        {
            ModuleName = ModuleName,
            SettingName = friendlySettingName,
            OldValue = format(oldValue),
            NewValue = format(newValue)
        });

        key.SetValue(valueName, newValue, kind);
    }

    public void DeleteValue(RegistryHive hive, string subKey, string valueName, string friendlySettingName)
    {
        using var baseKey = OpenBaseKey(hive);
        using var key = baseKey.OpenSubKey(subKey, writable: true);
        if (key is null) return;

        var existed = key.GetValueNames().Contains(valueName, StringComparer.OrdinalIgnoreCase);
        if (!existed) return;

        var oldValue = key.GetValue(valueName);
        var oldKind = key.GetValueKind(valueName);

        BackupEntries.Add(new RegistryValueBackup
        {
            Hive = hive,
            SubKey = subKey,
            ValueName = valueName,
            ValueExisted = true,
            SerializedOldValue = SerializeValue(oldValue, oldKind),
            OldKind = oldKind
        });

        ChangeLog.Add(new ChangeLogEntry
        {
            ModuleName = ModuleName,
            SettingName = friendlySettingName,
            OldValue = oldValue?.ToString() ?? string.Empty,
            NewValue = "(removed)"
        });

        key.DeleteValue(valueName);
    }

    internal static string? SerializeValue(object? value, RegistryValueKind kind)
    {
        if (value is null) return null;
        return kind switch
        {
            RegistryValueKind.MultiString => string.Join(MultiStringSeparator, (string[])value),
            RegistryValueKind.Binary => Convert.ToBase64String((byte[])value),
            _ => value.ToString()
        };
    }

    internal static object? DeserializeValue(string? serialized, RegistryValueKind kind)
    {
        if (serialized is null) return null;
        return kind switch
        {
            RegistryValueKind.DWord => int.Parse(serialized),
            RegistryValueKind.QWord => long.Parse(serialized),
            RegistryValueKind.MultiString => serialized.Split(MultiStringSeparator),
            RegistryValueKind.Binary => Convert.FromBase64String(serialized),
            _ => serialized
        };
    }
}
