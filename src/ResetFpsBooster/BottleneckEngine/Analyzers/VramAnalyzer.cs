using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>VRAM near full is only meaningful when it's actually causing frame delivery problems.
/// Without real frametime data this can't be confirmed, so the signal is deliberately capped at
/// moderate confidence and says so explicitly.</summary>
public sealed class VramAnalyzer : IBottleneckAnalyzer
{
    private const double HighUsageThresholdPercent = 94;

    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        if (current.VramUsedPercent is not { } usedPercent || usedPercent < HighUsageThresholdPercent) yield break;

        var confirmed = current.IsFrametimeAvailable;
        yield return new BottleneckSignal
        {
            Kind = BottleneckKind.VramLimited,
            Weight = confirmed ? 0.65 : 0.35,
            Reason = confirmed
                ? $"VRAM usage at {usedPercent:0}%, correlating with frame time spikes."
                : $"VRAM usage at {usedPercent:0}% — near its limit, though frame-time data to confirm actual stutter from it is unavailable."
        };
    }
}
