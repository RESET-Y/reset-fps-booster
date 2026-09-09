#if RFB_BETA
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

/// <summary>
/// Manages the RESET FRAMEBOOST BETA native engine (FrameBoostBeta.exe) — a
/// completely separate process from this app and from any target game.
/// This service only ever: enumerates OTHER processes' top-level window
/// handles (a public, read-only Win32 API — no memory access, no
/// injection), launches the native engine as an ordinary child process with
/// that handle as a command-line argument, and tails a log file the engine
/// writes. It never touches a target application beyond that.
/// </summary>
public sealed class FrameBoostBetaService : IFrameBoostBetaService, IDisposable
{
    private static readonly Regex TelemetryLineRegex = new(
        @"Native FPS: (?<native>[\d.]+) \| Generated FPS: (?<generated>[\d.]+) \| Output FPS: (?<output>[\d.]+) \| Poll time: (?<poll>[\d.]+) ms \| Capture latency \(real, avg\): (?:(?<latency>[\d.]+) ms|N/A) \| Motion estimation GPU: (?<me>[\d.]+) ms \| Interpolation GPU: (?<interp>[\d.]+) ms",
        RegexOptions.Compiled);

    private Process? _process;

    public bool IsRunning => _process is { HasExited: false };

    public IReadOnlyList<CaptureTargetWindow> EnumerateCandidateWindows()
    {
        var results = new List<CaptureTargetWindow>();
        int ownProcessId = Environment.ProcessId;

        NativeMethods.EnumWindows((hWnd, _) =>
        {
            if (!NativeMethods.IsWindowVisible(hWnd)) return true;

            int titleLength = NativeMethods.GetWindowTextLength(hWnd);
            if (titleLength == 0) return true;

            var titleBuffer = new System.Text.StringBuilder(titleLength + 1);
            NativeMethods.GetWindowText(hWnd, titleBuffer, titleBuffer.Capacity);
            string title = titleBuffer.ToString();
            if (string.IsNullOrWhiteSpace(title)) return true;

            NativeMethods.GetWindowThreadProcessId(hWnd, out uint pid);
            if (pid == ownProcessId) return true;

            string processName = "unknown";
            try
            {
                using var proc = Process.GetProcessById((int)pid);
                processName = proc.ProcessName;
            }
            catch { /* process may have exited between enumeration and lookup */ }

            results.Add(new CaptureTargetWindow(hWnd, title, processName));
            return true;
        }, IntPtr.Zero);

        return results;
    }

    public string? Start(CaptureTargetWindow target)
    {
        Stop();

        string exePath = Path.Combine(AppContext.BaseDirectory, "FrameBoostBeta", "FrameBoostBeta.exe");
        if (!File.Exists(exePath))
            return $"FrameBoost engine not found at {exePath}.";

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = target.Handle.ToString(CultureInfo.InvariantCulture),
                UseShellExecute = false,
                CreateNoWindow = false,
            };
            _process = Process.Start(psi);
            return _process is null ? "Failed to start the FrameBoost engine process." : null;
        }
        catch (Exception ex)
        {
            return $"Failed to start FrameBoost: {ex.Message}";
        }
    }

    public void Stop()
    {
        if (_process is { HasExited: false })
        {
            try { _process.CloseMainWindow(); if (!_process.WaitForExit(2000)) _process.Kill(); }
            catch { /* best-effort - never let a shutdown failure here surface as a crash */ }
        }
        _process?.Dispose();
        _process = null;
    }

    public FrameBoostBetaTelemetry ReadLatestTelemetry()
    {
        try
        {
            string logPath = Path.Combine(AppPaths.LogsFolder, "framebooost_beta.log");
            if (!File.Exists(logPath)) return new FrameBoostBetaTelemetry();

            using var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var reader = new StreamReader(stream);
            string? lastMatchLine = null;
            string? line;
            while ((line = reader.ReadLine()) != null)
            {
                if (line.Contains("Native FPS:")) lastMatchLine = line;
            }

            if (lastMatchLine is null) return new FrameBoostBetaTelemetry();

            var match = TelemetryLineRegex.Match(lastMatchLine);
            if (!match.Success) return new FrameBoostBetaTelemetry();

            return new FrameBoostBetaTelemetry
            {
                NativeFps = ParseDouble(match.Groups["native"]),
                GeneratedFps = ParseDouble(match.Groups["generated"]),
                OutputFps = ParseDouble(match.Groups["output"]),
                PollTimeMs = ParseDouble(match.Groups["poll"]),
                CaptureLatencyMs = match.Groups["latency"].Success ? ParseDouble(match.Groups["latency"]) : null,
                MotionEstimationGpuMs = ParseDouble(match.Groups["me"]),
                InterpolationGpuMs = ParseDouble(match.Groups["interp"]),
                LastUpdatedUtc = DateTime.UtcNow,
            };
        }
        catch
        {
            // Log file locked/mid-write/missing - honestly report "no data" rather than guessing.
            return new FrameBoostBetaTelemetry();
        }
    }

    private static double? ParseDouble(System.Text.RegularExpressions.Group group) =>
        group.Success && double.TryParse(group.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : null;

    public void Dispose() => Stop();

    private static class NativeMethods
    {
        public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern int GetWindowTextLength(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    }
}
#endif
