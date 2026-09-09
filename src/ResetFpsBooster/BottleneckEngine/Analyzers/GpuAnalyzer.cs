using System.Linq;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>
/// Votes for GPU_LIMITED when GPU usage is consistently near its ceiling and the CPU is not
/// simultaneously the dominant factor. Deliberately does not fire when GPU usage is high *because*
/// of thermal or power throttling — that's ThermalAnalyzer/PowerAnalyzer's job, and the classifier
/// gives their signals priority since they describe the actual root cause.
/// </summary>
public sealed class GpuAnalyzer : IBottleneckAnalyzer
{
    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        var gpuUsage = current.GpuUsagePercent;
        if (gpuUsage is null || gpuUsage < 92) yield break;

        var busiestCore = current.CpuMaxCoreUsagePercent ?? current.CpuTotalUsagePercent;
        // If a CPU thread is equally or more saturated, CPU is at least as likely the cause —
        // let CpuAnalyzer's own signal (and the classifier's comparison) speak for that case.
        if (busiestCore is not null && busiestCore >= 90 && current.GpuUsageHeadroomPercent is > 10)
            yield break;

        var recentUsage = recentHistory
            .Select(s => s.GpuUsagePercent)
            .Where(v => v.HasValue)
            .Select(v => v!.Value)
            .ToList();
        var sustained = recentUsage.Count == 0 || recentUsage.Average() >= 85;
        if (!sustained) yield break;

        var weight = Math.Min(1.0, (gpuUsage.Value - 92) / 8.0 * 0.5 + 0.4);

        yield return new BottleneckSignal
        {
            Kind = BottleneckKind.GpuLimited,
            Weight = weight,
            Reason = $"GPU utilization sustained at {gpuUsage:0}% with no other dominant limiting factor observed."
        };
    }
}
