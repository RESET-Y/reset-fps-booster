#if RFB_BETA
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IFrameBoostBetaService
{
    bool IsRunning { get; }

    /// Captures one game window and shows the doubled output over it.
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
    /// <param name="gameWindow">The window handle picked in the list, or 0 for
    /// the engine's own five-second countdown.</param>
    /// <param name="displayHz">The refresh rate picked in the app.</param>
    string? StartFrameBoost(bool lowLatency, long gameWindow, int displayHz);

    /// Turns frame generation off. The engine keeps running if Smooth Motion
    /// is still on, otherwise it stops.
    void StopFrameBoost();

    /// SMOOTH MOTION, its own feature on the same engine: motion-aware blur
    /// during fast movement, strength chosen by the engine. On its own the
    /// engine runs without generated frames; together with FrameBoost one
    /// engine does both. On its own it covers the whole of monitor `screen`
    /// (0 = primary); with FrameBoost it follows FrameBoost's game window.
    /// `strength` is the slider, 0..100, 50 = the automatic strength.
    string? SetSmoothMotion(bool on, int screen, long window, int strength);

    bool FrameBoostOn { get; }
    bool SmoothMotionOn { get; }

    /// Raised whenever either feature is switched, so both pages can follow.
    event EventHandler? StateChanged;

    /// Stops the engine and both features, e.g. when the app closes.
    void Stop();

    /// Reads the native engine's own real-measurement log and returns the
    /// most recent telemetry line, parsed. Never fabricates a value — a
    /// field stays null if the log doesn't report it (e.g. "N/A").
    FrameBoostBetaTelemetry ReadLatestTelemetry();
}
#endif
