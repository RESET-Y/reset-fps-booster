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
                // WINDOW capture, and the display-slot wait.
                //
                // This was "no arguments at all" - monitor capture - because
                // window capture used to stall the moment the overlay covered
                // the game: Windows stops drawing what it believes is hidden,
                // which starved the very frames FrameBoost needs. Measured back
                // then: 32-35 duplicate frames a second and gaps up to 485 ms.
                //
                // That failure is fixed at its source. The overlay is created
                // one step below opaque (alpha 254), which Windows does not
                // count as an occluder, so the game keeps rendering underneath.
                // Window capture measured 0 duplicates a second through a full
                // day of testing on 2026-09-14, against 45-96 with the monitor
                // path, and it is the single largest quality win the engine
                // has had.
                //
                // "slotwait" holds each present until the display can actually
                // show it, which is what every measurement today was taken
                // with: 72 -> 144 fps at 0.3 ms jitter and 0% missed slots.
                //
                // Hotkeys stay off deliberately. Even behind CTRL+ALT they were
                // being triggered from inside the game - one session silently
                // turned off the frame buffer and raised the generation factor,
                // and the judder came back. Started from here, the tuned
                // configuration is the only one that runs.
                Arguments = "window slotwait",
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

            // READ THE TAIL, NOT THE FILE.
            //
            // This used to scan from the first byte on every call, and it is
            // called from a DispatcherTimer every 500 ms - on the UI thread. The
            // engine writes a 2.5 KB telemetry line every second, so the log
            // grows about 9 MB an hour and never stopped: it reached 122 MB in
            // one evening of testing.
            //
            // A quarter of a gigabyte of text parsing per second, on the thread
            // that draws the window, beside the game the whole product exists to
            // help. The timer cannot possibly keep its interval, so the number
            // shown is from whenever the last scan happened to finish - which is
            // why the panel disagreed with the log it was reading.
            //
            // 256 KB is about a hundred telemetry lines, far more than the one
            // needed, and it does not grow.
            const int kTailBytes = 256 * 1024;
            using var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            if (stream.Length > kTailBytes)
            {
                stream.Seek(-kTailBytes, SeekOrigin.End);
            }
            using var reader = new StreamReader(stream);
            // The first line after a seek is very likely cut in half; skipping it
            // costs nothing, because the line wanted is the last one.
            if (stream.Position > 0) reader.ReadLine();
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
                NoGpuRoom = lastMatchLine.Contains("no GPU room", StringComparison.Ordinal) ? 1 : 0,
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
