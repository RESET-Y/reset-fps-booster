using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Optimization;

namespace ResetFpsBooster.Services;

public sealed class ModuleState
{
    public required IOptimizationModule Module { get; init; }
    public OptimizationStatus Status { get; set; } = new();
}

public interface IOptimizationService
{
    IReadOnlyList<IOptimizationModule> Modules { get; }
    Task<List<ModuleState>> RefreshStatusesAsync(CancellationToken ct = default);
    Task<OptimizationApplyResult> ApplyModuleAsync(IOptimizationModule module, CancellationToken ct = default);
    Task<List<OptimizationApplyResult>> ApplyModulesAsync(IEnumerable<IOptimizationModule> modules, IProgress<(IOptimizationModule Module, OptimizationApplyResult Result)>? progress = null, CancellationToken ct = default);
}
