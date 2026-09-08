using System.Diagnostics;
using System.Security.Principal;

namespace ResetFpsBooster.Core.Utilities;

public static class AdminHelper
{
    public static bool IsRunningAsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
    }

    /// <summary>
    /// Relaunches the current executable elevated via UAC and exits the current process.
    /// Returns false (and does not exit) if the user cancels the UAC prompt.
    /// </summary>
    public static bool RelaunchElevatedAndExit(string? extraArguments = null)
    {
        try
        {
            var exePath = Process.GetCurrentProcess().MainModule?.FileName;
            if (string.IsNullOrEmpty(exePath))
                return false;

            var startInfo = new ProcessStartInfo(exePath)
            {
                UseShellExecute = true,
                Verb = "runas",
                Arguments = extraArguments ?? string.Empty
            };

            Process.Start(startInfo);
            Environment.Exit(0);
            return true;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // UAC prompt was cancelled by the user.
            return false;
        }
    }
}
