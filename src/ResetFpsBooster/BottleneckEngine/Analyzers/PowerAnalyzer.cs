using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>Detects the GPU sitting at its power limit — a real, measured value from NVAPI's power
/// topology status, not inferred from usage alone. When power usage is pegged near 100% of the
/// GPU's own limit while under load, the card's own clock is capping itself, not "GPU limited" in
/// the ordinary sense.</summary>
public sealed class PowerAnalyzer : IBottleneckAnalyzer
{
    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        if (current.GpuPowerPercentOfLimit is not { } powerPercent || powerPercent < 97) yield break;
        if (current.GpuUsagePercent is not { } usage || usage < 85) yield break;

        yield return new BottleneckSignal
        {
            Kind = BottleneckKind.PowerLimited,
            Weight = Math.Min(1.0, 0.55 + (powerPercent - 97) / 3.0 * 0.35),
            Reason = $"GPU power draw at {powerPercent:0}% of its configured power limit while at {usage:0}% utilization — the power limit itself is capping performance, not raw workload demand."
        };
    }
}
