#if RFB_BETA
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
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
                // "monitor": capture the whole display the target sits on,
                // rather than the window itself.
                //
                // Window capture stalls as soon as the overlay covers the
                // window - Windows stops drawing what it believes is hidden,
                // which starves the very frames FrameBoost needs. Measured on
                // the same game and machine: 32-35 duplicate frames per second
                // and gaps up to 485 ms with window capture, against 0 and
                // ~7 ms capturing the monitor. A display is always composited,
                // so it keeps delivering with the overlay on top of it.
                Arguments = string.Create(CultureInfo.InvariantCulture,
                    $"{target.Handle} monitor"),
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

            return new FrameBoostBetaTelemetry
            {
                NativeFps = ReadField(lastMatchLine, "Native FPS"),
                GeneratedFps = ReadField(lastMatchLine, "Generated FPS"),
                OutputFps = ReadField(lastMatchLine, "Output FPS"),
                PollTimeMs = ReadField(lastMatchLine, "Poll time"),
                CaptureLatencyMs = ReadField(lastMatchLine, "Capture latency (real, avg)"),
                OnScreenAgeMs = ReadField(lastMatchLine, "On-screen age"),
                MotionEstimationGpuMs = ReadField(lastMatchLine, "Motion estimation GPU"),
                InterpolationGpuMs = ReadField(lastMatchLine, "Interpolation GPU"),
                LastUpdatedUtc = DateTime.UtcNow,
            };
        }
        catch
        {
            // Log file locked/mid-write/missing - honestly report "no data" rather than guessing.
            return new FrameBoostBetaTelemetry();
        }
    }

    /// Reads one "Name: value" field out of a telemetry line, wherever it sits.
    ///
    /// Deliberately not one big regex over the whole line: the engine's
    /// telemetry gained a dozen fields in a single day of measurement work, and
    /// a pattern that pins the order silently stops matching every time one is
    /// inserted - showing an empty panel rather than an error. Reading each
    /// field by name survives that, and a field that genuinely is not reported
    /// (the engine writes "N/A") stays null rather than becoming a made-up zero.
    private static double? ReadField(string line, string name)
    {
        int keyIndex = line.IndexOf(name + ":", StringComparison.Ordinal);
        if (keyIndex < 0) return null;

        int valueStart = keyIndex + name.Length + 1;
        int valueEnd = line.IndexOf('|', valueStart);
        if (valueEnd < 0) valueEnd = line.Length;

        // The value may carry a unit or further detail ("6.94 ms", "6.6 ms avg,
        // 13.4 ms max") - the leading number is the one meant.
        var span = line.AsSpan(valueStart, valueEnd - valueStart).Trim();
        int length = 0;
        while (length < span.Length && (char.IsDigit(span[length]) || span[length] == '.' || span[length] == '-'))
            length++;
        if (length == 0) return null; // "N/A" and anything else non-numeric

        return double.TryParse(span[..length], NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? value
            : null;
    }

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
