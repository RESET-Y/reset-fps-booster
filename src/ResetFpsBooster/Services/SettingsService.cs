using System.Diagnostics;
using System.Reflection;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Services;

public sealed class SettingsService : ISettingsService
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValueName = "ResetFpsBooster";

    public AppSettings Current { get; }

    public SettingsService()
    {
        AppPaths.EnsureFoldersExist();
        Current = JsonStore.Load(AppPaths.SettingsFile, () => new AppSettings());
    }

    public void Save() => JsonStore.Save(AppPaths.SettingsFile, Current);

    public void ApplyStartWithWindows(bool enabled)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true) ?? Registry.CurrentUser.CreateSubKey(RunKey);
            if (key is null) return;

            if (enabled)
            {
                var exePath = Process.GetCurrentProcess().MainModule?.FileName
                    ?? Assembly.GetExecutingAssembly().Location;
                key.SetValue(RunValueName, $"\"{exePath}\" --minimized");
            }
            else
            {
                if (key.GetValueNames().Contains(RunValueName))
                    key.DeleteValue(RunValueName);
            }

            Current.StartWithWindows = enabled;
            Save();
        }
        catch
        {
            // Non-critical convenience setting — ignore failures (e.g. locked-down policy).
        }
    }
}
