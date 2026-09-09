using System.Linq;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>
/// Votes for CPU_LIMITED only when a CPU thread is saturated *and* the GPU simultaneously has
/// real headroom — high CPU usage alone (e.g. 90% CPU with 98% GPU) is explicitly not enough,
/// per the classifier's exception rules.
/// </summary>
public sealed class CpuAnalyzer : IBottleneckAnalyzer
{
    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        var busiestCore = current.CpuMaxCoreUsagePercent ?? current.CpuTotalUsagePercent;
        var gpuHeadroom = current.GpuUsageHeadroomPercent;

        if (busiestCore is null || gpuHeadroom is null) yield break;
        if (busiestCore < 85 || gpuHeadroom < 12) yield break;

        // Sustain check: require the busiest-core reading to hold up over the recent window too,
        // not just this single tick, so one transient spike doesn't flip the diagnosis.
        var recentBusiest = recentHistory
            .Select(s => s.CpuMaxCoreUsagePercent ?? s.CpuTotalUsagePercent)
            .Where(v => v.HasValue)
            .Select(v => v!.Value)
            .ToList();
        var sustained = recentBusiest.Count == 0 || recentBusiest.Average() >= 78;
        if (!sustained) yield break;

        var weight = Math.Min(1.0, (busiestCore.Value - 85) / 15.0 * 0.6 + gpuHeadroom.Value / 100.0 * 0.4 + 0.35);

        yield return new BottleneckSignal
        {
            Kind = BottleneckKind.CpuLimited,
            Weight = weight,
            Reason = $"CPU thread saturation {busiestCore:0}% while GPU utilization is only {current.GpuUsagePercent:0}% (headroom {gpuHeadroom:0}%)."
        };
    }
}
