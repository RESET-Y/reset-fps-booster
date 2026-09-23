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
    /// <param name="lowLatency">Trades picture quality on the GENERATED
    /// frames for input lag. Measured in CS2 over 49 s, with the engine
    /// already not holding frames back: of 10.79 ms total, 4.73 ms is
    /// Windows delivering the captured frame and cannot be touched from
    /// here, and 4.13 ms is GPU work - nearly all of it the motion search,
    /// which peaks to 9.59 ms exactly when the picture moves fastest. Low
    /// latency generates at half resolution with a cheaper filter, so it cuts
    /// into that part and into those peaks. It cannot halve the number.</param>
    string? Start(bool lowLatency = false);

    void Stop();

    /// Reads the native engine's own real-measurement log and returns the
    /// most recent telemetry line, parsed. Never fabricates a value — a
    /// field stays null if the log doesn't report it (e.g. "N/A").
    FrameBoostBetaTelemetry ReadLatestTelemetry();
}
#endif
