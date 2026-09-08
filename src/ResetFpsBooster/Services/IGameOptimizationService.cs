using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IGameOptimizationService
{
    Task<(bool Success, string Message)> OptimizeGameAsync(GameProfile profile, CancellationToken ct = default);
    (bool Success, string Message) RestoreGameSettings(GameProfile profile);
}
