using System.IO;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public sealed class GameLibraryService : IGameLibraryService
{
    private readonly IGameDetectionService _detectionService;
    private List<GameProfile> _games;

    public GameLibraryService(IGameDetectionService detectionService)
    {
        _detectionService = detectionService;
        AppPaths.EnsureFoldersExist();
        _games = JsonStore.Load(AppPaths.GamesFile, () => new List<GameProfile>());
        DeduplicateExisting();
    }

    public List<GameProfile> GetGames() => _games;

    public async Task<List<GameProfile>> RescanAsync(CancellationToken ct = default)
    {
        var detected = await _detectionService.DetectGamesAsync(ct);

        foreach (var found in detected)
        {
            var existing = _games.FirstOrDefault(g => PathsMatch(g.InstallPath, found.InstallPath));

            if (existing is null)
            {
                _games.Add(found);
            }
            else
            {
                // Preserve optimization state / manual executable overrides for games we already knew about.
                existing.Name = found.Name;
                existing.ExecutablePath ??= found.ExecutablePath;
            }
        }

        Persist();
        return _games;
    }

    public void Save(GameProfile profile)
    {
        var existing = _games.FirstOrDefault(g => g.Id == profile.Id);
        if (existing is null)
            _games.Add(profile);
        else
            _games[_games.IndexOf(existing)] = profile;

        Persist();
    }

    public void Remove(string gameId)
    {
        _games.RemoveAll(g => g.Id == gameId);
        Persist();
    }

    /// <summary>
    /// Collapses entries that point at the same install folder but ended up as separate records
    /// (e.g. from a rescan that matched paths before normalization was added). Keeps whichever
    /// duplicate has the most useful data — a resolved executable and/or an applied optimization.
    /// </summary>
    private void DeduplicateExisting()
    {
        var groups = _games.GroupBy(g => NormalizePath(g.InstallPath)).ToList();
        if (groups.All(g => g.Count() == 1)) return;

        _games = groups
            .Select(group => group
                .OrderByDescending(g => !string.IsNullOrEmpty(g.ExecutablePath))
                .ThenByDescending(g => g.IsOptimized)
                .First())
            .ToList();

        Persist();
    }

    private static bool PathsMatch(string a, string b) => NormalizePath(a) == NormalizePath(b);

    private static string NormalizePath(string path)
    {
        try
        {
            return Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)).ToLowerInvariant();
        }
        catch
        {
            return path.TrimEnd('\\', '/').ToLowerInvariant();
        }
    }

    private void Persist() => JsonStore.Save(AppPaths.GamesFile, _games);
}
