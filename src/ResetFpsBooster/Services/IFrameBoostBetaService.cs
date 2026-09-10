#if RFB_BETA
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IFrameBoostBetaService
{
    bool IsRunning { get; }

    /// Boosts the whole main display. There is no target to choose: the
    /// engine captures the monitor, which is also the only source that keeps
    /// delivering frames while our own output is displayed on top of it.
    /// <returns>Null on success, or a human-readable reason it could not start
    /// (missing engine binary, launch failure, etc.) — never throws.</returns>
    string? Start();

    void Stop();

    /// Reads the native engine's own real-measurement log and returns the
    /// most recent telemetry line, parsed. Never fabricates a value — a
    /// field stays null if the log doesn't report it (e.g. "N/A").
    FrameBoostBetaTelemetry ReadLatestTelemetry();
}
#endif
