using System.Linq;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>Flags meaningful background CPU consumption while a game is running — strengthened
/// when disk activity is elevated at the same time (e.g. an updater or indexer actively
/// working), since that's a stronger, more specific signature of background workload.</summary>
public sealed class ProcessAnalyzer : IBottleneckAnalyzer
{
    private const double SignificantBackgroundCpuPercent = 15;

    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        var backgroundCpu = current.BackgroundCpuPercent;
        if (backgroundCpu < SignificantBackgroundCpuPercent) yield break;

        var topProcess = current.TopBackgroundProcesses.OrderByDescending(p => p.CpuPercent).FirstOrDefault();
        var diskBoost = current.DiskActivePercent is > 50 ? 0.15 : 0;

        yield return new BottleneckSignal
        {
            Kind = BottleneckKind.BackgroundWorkload,
            Weight = Math.Min(1.0, 0.4 + (backgroundCpu - SignificantBackgroundCpuPercent) / 40.0 + diskBoost),
            Reason = topProcess is null
                ? $"Background processes are consuming {backgroundCpu:0}% combined CPU."
                : $"\"{topProcess.ProcessName}\" and other background processes are consuming {backgroundCpu:0}% combined CPU."
        };
    }
}
