#if RFB_BETA
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IFrameBoostBetaService
{
    bool IsRunning { get; }

    IReadOnlyList<CaptureTargetWindow> EnumerateCandidateWindows();

    /// <returns>Null on success, or a human-readable reason it could not start
    /// (missing engine binary, launch failure, etc.) — never throws.</returns>
    string? Start(CaptureTargetWindow target);

    void Stop();

    /// Reads the native engine's own real-measurement log and returns the
    /// most recent telemetry line, parsed. Never fabricates a value — a
    /// field stays null if the log doesn't report it (e.g. "N/A").
    FrameBoostBetaTelemetry ReadLatestTelemetry();
}
#endif
