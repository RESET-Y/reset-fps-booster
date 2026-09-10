#if RFB_BETA
using System.Diagnostics;
using System.Globalization;
using System.IO;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

/// <summary>
/// Manages the RESET FRAMEBOOST BETA native engine (FrameBoostBeta.exe) — a
/// completely separate process from this app and from any running game.
/// This service only ever launches that engine as an ordinary child process
/// and tails the log file it writes. It never touches a game: no memory
/// access, no injection, nothing but the public screen-capture API the
/// engine itself uses.
/// </summary>
public sealed class FrameBoostBetaService : IFrameBoostBetaService, IDisposable
{
    private Process? _process;

    public bool IsRunning => _process is { HasExited: false };

    public string? Start()
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
                // No arguments at all: the engine boosts the whole primary
                // display, and its hotkeys stay disabled.
                //
                // Both matter. Window capture stalls as soon as the overlay
                // covers the window - Windows stops drawing what it believes
                // is hidden, which starves the very frames FrameBoost needs
                // (measured: 32-35 duplicate frames per second and gaps up to
                // 485 ms, against 0 and ~7 ms capturing the monitor). And the
                // engine's F-key hotkeys, even behind CTRL+ALT, were being
                // triggered from inside the game: one session silently turned
                // off the frame buffer and raised the generation factor, which
                // brought the judder back. Started from here, the tuned
                // configuration is the only one that runs.
                Arguments = string.Empty,
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
                SourceFps = ReadField(lastMatchLine, "Source FPS"),
                NativeFps = ReadField(lastMatchLine, "Native FPS"),
                GeneratedFps = ReadField(lastMatchLine, "Generated FPS"),
                OutputFps = ReadField(lastMatchLine, "Output FPS"),
                PollTimeMs = ReadField(lastMatchLine, "Poll time"),
                CaptureLatencyMs = ReadField(lastMatchLine, "Capture latency (real, avg)"),
                OnScreenAgeMs = ReadField(lastMatchLine, "On-screen age"),
                DisplayHz = ReadField(lastMatchLine, "Display Hz"),
                DuplicateFps = ReadField(lastMatchLine, "Duplicate frames skipped/s"),
                DoublingActive = lastMatchLine.Contains("Doubling: on", StringComparison.Ordinal) ? 1 : 0,
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

}
#endif
