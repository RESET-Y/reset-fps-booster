using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>
/// High RAM usage by itself is normal and not a bottleneck — this only fires when usage is high
/// AND the system is actively paging (hard faults/sec meaningfully above idle), which is the real
/// symptom of memory pressure hurting performance.
/// </summary>
public sealed class MemoryAnalyzer : IBottleneckAnalyzer
{
    private const double HighUsageThresholdPercent = 88;
    private const double SignificantHardFaultsPerSec = 40;

    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        if (current.RamUsedPercent is not { } usedPercent || usedPercent < HighUsageThresholdPercent) yield break;

        var hardFaults = current.RamHardFaultsPerSec;
        if (hardFaults is null)
        {
            // Honest, reduced-confidence signal: usage alone is suggestive but not proof of pressure.
            yield return new BottleneckSignal
            {
                Kind = BottleneckKind.MemoryLimited,
                Weight = 0.2,
                Reason = $"RAM usage at {usedPercent:0}%, but hard page fault data was unavailable to confirm active paging."
            };
            yield break;
        }

        if (hardFaults < SignificantHardFaultsPerSec) yield break;

        yield return new BottleneckSignal
        {
            Kind = BottleneckKind.MemoryLimited,
            Weight = Math.Min(1.0, 0.5 + (hardFaults.Value - SignificantHardFaultsPerSec) / 200.0),
            Reason = $"RAM usage at {usedPercent:0}% with {hardFaults:0} hard page faults/sec — the system is actively paging memory to disk."
        };
    }
}
