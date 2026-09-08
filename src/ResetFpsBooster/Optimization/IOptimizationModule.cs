using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Optimization;

/// <summary>
/// A single, independently applicable optimization. Every module is responsible for
/// reporting whether it is already applied and for recording every registry change it
/// makes through the shared <see cref="RegistryChangeRecorder"/> so it can be undone.
/// </summary>
public interface IOptimizationModule
{
    string Id { get; }
    string Name { get; }
    string Description { get; }
    OptimizationCategory Category { get; }
    RiskLevel Risk { get; }
    bool RequiresAdmin { get; }
    bool RequiresReboot { get; }

    Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default);

    Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default);
}
