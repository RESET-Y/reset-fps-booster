#if RFB_BETA
namespace ResetFpsBooster.Core.Models;

public sealed record CaptureTargetWindow(nint Handle, string Title, string ProcessName);

// Real values only — every field stays null until the native engine's own
// log actually reports it. Never fabricated, never a placeholder number.
public sealed class FrameBoostBetaTelemetry
{
    public double? NativeFps { get; init; }
    public double? GeneratedFps { get; init; }
    public double? OutputFps { get; init; }
    public double? PollTimeMs { get; init; }
    public double? CaptureLatencyMs { get; init; }
    public double? MotionEstimationGpuMs { get; init; }
    public double? InterpolationGpuMs { get; init; }
    public DateTime? LastUpdatedUtc { get; init; }
}
#endif
