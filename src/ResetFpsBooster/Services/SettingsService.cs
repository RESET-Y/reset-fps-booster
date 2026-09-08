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

    public (bool Success, string Message) ApplyStartWithWindowsAsAdmin(bool enabled)
    {
        if (!enabled)
        {
            var (deleted, deleteMessage) = ScheduledTaskHelper.DeleteTask();
            if (deleted)
            {
                Current.StartWithWindowsAsAdmin = false;
                Save();
            }
            return (deleted, deleteMessage);
        }

        var exePath = Process.GetCurrentProcess().MainModule?.FileName
            ?? Assembly.GetExecutingAssembly().Location;

        var (created, createMessage) = ScheduledTaskHelper.CreateElevatedLogonTask(exePath);
        if (!created)
            return (false, createMessage);

        // The scheduled task now owns startup — remove the plain Run key so the app doesn't launch twice.
        if (Current.StartWithWindows)
        {
            try
            {
                using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
                if (key?.GetValueNames().Contains(RunValueName) == true)
                    key.DeleteValue(RunValueName);
            }
            catch
            {
                // Best-effort cleanup — not worth failing the whole operation over.
            }
            Current.StartWithWindows = false;
        }

        Current.StartWithWindowsAsAdmin = true;
        Save();
        return (true, createMessage);
    }
}
