#if RFB_BETA
namespace ResetFpsBooster.Core.Models;


// Real values only — every field stays null until the native engine's own
// log actually reports it. Never fabricated, never a placeholder number.
public sealed class FrameBoostBetaTelemetry
{
    public double? NativeFps { get; init; }
    public double? GeneratedFps { get; init; }
    public double? OutputFps { get; init; }
    public double? PollTimeMs { get; init; }
    public double? CaptureLatencyMs { get; init; }

    // How old the newest real frame is by the time it is actually on screen -
    // capture latency plus everything the engine adds after it. This is the
    // number a player feels, so it is reported separately rather than folded
    // into CaptureLatencyMs.
    public double? OnScreenAgeMs { get; init; }

    // The display the boost is running on. The panel needs it to say whether
    // doubling can still reach the screen: above half the refresh rate the
    // generated frames exist but the monitor has no window left to show them.
    public double? DisplayHz { get; init; }
    public double? MotionEstimationGpuMs { get; init; }
    public double? InterpolationGpuMs { get; init; }
    public DateTime? LastUpdatedUtc { get; init; }
}
#endif
