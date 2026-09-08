using System.Diagnostics;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public sealed class SystemRestoreService : ISystemRestoreService
{
    public (bool Success, string Message) CreateRestorePoint(string description)
    {
        if (!AdminHelper.IsRunningAsAdministrator())
            return (false, "Creating a System Restore point requires Administrator rights. Restart RESET FPS BOOSTER as Administrator and try again.");

        try
        {
            // Checkpoint-Computer is the documented PowerShell surface for SystemRestore.CreateRestorePoint —
            // no need to hand-roll the WMI call ourselves.
            var escapedDescription = description.Replace("'", "''");
            var script = $"Checkpoint-Computer -Description '{escapedDescription}' -RestorePointType 'MODIFY_SETTINGS'";

            var psi = new ProcessStartInfo("powershell.exe", $"-NoProfile -NonInteractive -Command \"{script}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            using var process = Process.Start(psi);
            if (process is null)
                return (false, "Could not start PowerShell to create the restore point.");

            var stdErr = process.StandardError.ReadToEnd();
            process.WaitForExit(30_000);

            if (process.ExitCode == 0)
                return (true, "System Restore point created.");

            // Windows throttles restore-point creation to once per 24h by default — the most common failure a user will hit.
            if (stdErr.Contains("frequency", StringComparison.OrdinalIgnoreCase) || stdErr.Contains("24", StringComparison.Ordinal))
                return (false, "Windows only allows one new restore point every 24 hours. A recent one may already cover this.");

            if (stdErr.Contains("System Restore", StringComparison.OrdinalIgnoreCase) && stdErr.Contains("disabled", StringComparison.OrdinalIgnoreCase))
                return (false, "System Restore is disabled for this drive. Enable it in Windows' \"Configure System Restore\" settings first.");

            return (false, string.IsNullOrWhiteSpace(stdErr) ? "Could not create the restore point." : stdErr.Trim());
        }
        catch (Exception ex)
        {
            return (false, $"Could not create the restore point: {ex.Message}");
        }
    }
}
