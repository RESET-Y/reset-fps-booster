using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IGameDetectionService
{
    Task<List<GameProfile>> DetectGamesAsync(CancellationToken ct = default);
}
