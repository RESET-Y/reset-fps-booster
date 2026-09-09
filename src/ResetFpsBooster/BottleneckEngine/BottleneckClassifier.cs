using System.Linq;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine;

/// <summary>
/// Combines every analyzer's signals into a final ranked diagnosis. This is where the "intelligent
/// exceptions" live: a root-cause signal (thermal/power throttling) suppresses the surface-level
/// signal it explains away (GPU/CPU looking "limited" only because it's being throttled), so the
/// report points at the actual cause instead of the symptom.
/// </summary>
public sealed class BottleneckClassifier
{
    private const double MinimumReportableConfidence = 32;

    private readonly IReadOnlyList<IBottleneckAnalyzer> _analyzers;
    private readonly BottleneckExplanationGenerator _explanationGenerator = new();

    public BottleneckClassifier(IReadOnlyList<IBottleneckAnalyzer> analyzers)
    {
        _analyzers = analyzers;
    }

    public BottleneckReport Classify(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        var rawSignals = _analyzers
            .SelectMany(a => SafeAnalyze(a, current, recentHistory))
            .ToList();

        var weights = rawSignals
            .GroupBy(s => s.Kind)
            .ToDictionary(g => g.Key, g => g.Sum(s => s.Weight));

        var reasons = rawSignals
            .GroupBy(s => s.Kind)
            .ToDictionary(g => g.Key, g => g.OrderByDescending(s => s.Weight).Select(s => s.Reason).ToList());

        ApplySuppressionRules(weights);

        var diagnoses = weights
            .Select(kv => new
            {
                kv.Key,
                Confidence = ConfidenceEngine.ToConfidencePercent(kv.Value)
            })
            .Where(x => x.Confidence >= MinimumReportableConfidence)
            .OrderByDescending(x => x.Confidence)
            .Take(3)
            .Select(x => _explanationGenerator.Build(x.Key, x.Confidence, current, reasons.GetValueOrDefault(x.Key) ?? new List<string>()))
            .ToList();

        return new BottleneckReport
        {
            Timestamp = current.Timestamp,
            GameName = current.GameName,
            Diagnoses = diagnoses,
            Snapshot = current
        };
    }

    private static IEnumerable<BottleneckSignal> SafeAnalyze(IBottleneckAnalyzer analyzer, TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> history)
    {
        try { return analyzer.Analyze(current, history).ToList(); }
        catch { return Enumerable.Empty<BottleneckSignal>(); }
    }

    /// <summary>Thermal and power limits are root causes, not just correlated symptoms — when
    /// either fires with meaningful confidence, the GPU/CPU-usage-based signals it would otherwise
    /// explain are reduced so the report leads with the actual cause.</summary>
    private static void ApplySuppressionRules(Dictionary<BottleneckKind, double> weights)
    {
        if (weights.TryGetValue(BottleneckKind.ThermalLimited, out var thermalWeight) && thermalWeight >= 0.5)
        {
            if (weights.TryGetValue(BottleneckKind.GpuLimited, out var gpuWeight))
                weights[BottleneckKind.GpuLimited] = gpuWeight * 0.35;
            if (weights.TryGetValue(BottleneckKind.CpuLimited, out var cpuWeight))
                weights[BottleneckKind.CpuLimited] = cpuWeight * 0.5;
        }

        if (weights.TryGetValue(BottleneckKind.PowerLimited, out var powerWeight) && powerWeight >= 0.5)
        {
            if (weights.TryGetValue(BottleneckKind.GpuLimited, out var gpuWeight))
                weights[BottleneckKind.GpuLimited] = gpuWeight * 0.4;
        }
    }
}
