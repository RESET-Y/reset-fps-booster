using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine;

/// <summary>
/// One independent analyzer that looks at a single dimension of the system (CPU, GPU, thermals,
/// power, memory, VRAM, storage, background load) and votes on which bottleneck kinds the
/// evidence supports. Analyzers never decide the final diagnosis themselves — that's the
/// <see cref="BottleneckClassifier"/>'s job, after seeing every analyzer's votes together.
/// </summary>
public interface IBottleneckAnalyzer
{
    IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory);
}
