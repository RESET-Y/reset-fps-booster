using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IGameLibraryService
{
    List<GameProfile> GetGames();
    Task<List<GameProfile>> RescanAsync(CancellationToken ct = default);
    void Save(GameProfile profile);
    void Remove(string gameId);
}
