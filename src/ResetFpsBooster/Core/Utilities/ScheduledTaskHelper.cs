using System.Diagnostics;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Creates/removes a logon-triggered Task Scheduler entry with "Run with highest privileges" —
/// the standard, well-documented way for an app to launch already elevated at startup without a
/// UAC prompt on every boot. Windows grants this without requiring the creating process itself
/// to be elevated, since the task belongs to the current user.
/// </summary>
public static class ScheduledTaskHelper
{
    private const string TaskName = "ResetFpsBoosterAutoStart";

    public static (bool Success, string Message) CreateElevatedLogonTask(string exePath)
    {
        var arguments = $"/Create /TN \"{TaskName}\" /TR \"\\\"{exePath}\\\" --minimized\" /SC ONLOGON /RL HIGHEST /F";
        var (success, output) = RunSchtasks(arguments);
        return success
            ? (true, "Elevated auto-start enabled — RESET FPS BOOSTER will launch as Administrator at logon, no UAC prompt.")
            : (false, $"Could not create the startup task: {output}");
    }

    public static (bool Success, string Message) DeleteTask()
    {
        var (success, output) = RunSchtasks($"/Delete /TN \"{TaskName}\" /F");
        // "not found" is not a failure from the caller's point of view — the end state (no task) is achieved either way.
        if (success || output.Contains("cannot find", StringComparison.OrdinalIgnoreCase))
            return (true, "Elevated auto-start disabled.");
        return (false, $"Could not remove the startup task: {output}");
    }

    private static (bool Success, string Output) RunSchtasks(string arguments)
    {
        try
        {
            var psi = new ProcessStartInfo("schtasks.exe", arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            using var process = Process.Start(psi);
            if (process is null) return (false, "Could not start schtasks.exe.");

            var stdOut = process.StandardOutput.ReadToEnd();
            var stdErr = process.StandardError.ReadToEnd();
            process.WaitForExit(10_000);

            return process.ExitCode == 0 ? (true, stdOut) : (false, string.IsNullOrWhiteSpace(stdErr) ? stdOut : stdErr);
        }
        catch (Exception ex)
        {
            return (false, ex.Message);
        }
    }
}
