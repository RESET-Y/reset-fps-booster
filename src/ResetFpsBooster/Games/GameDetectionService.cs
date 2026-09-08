using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;
using Microsoft.Win32;

namespace ResetFpsBooster.Games;

/// <summary>
/// Detects installed games by reading the real Steam and Epic Games Launcher metadata files —
/// no hardcoded "supported games" list. Any title installed through either launcher is found.
/// </summary>
public sealed partial class GameDetectionService : IGameDetectionService
{
    public Task<List<GameProfile>> DetectGamesAsync(CancellationToken ct = default)
        => Task.Run(() =>
        {
            var games = new List<GameProfile>();
            games.AddRange(DetectSteamGames(ct));
            games.AddRange(DetectEpicGames(ct));
            return games;
        }, ct);

    private static IEnumerable<GameProfile> DetectSteamGames(CancellationToken ct)
    {
        var steamPath = ReadSteamInstallPath();
        if (steamPath is null || !Directory.Exists(steamPath))
            yield break;

        var libraryFolders = new List<string> { Path.Combine(steamPath, "steamapps") };
        var seenLibraryFolders = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { NormalizeFolder(libraryFolders[0]) };

        var vdfPath = Path.Combine(steamPath, "steamapps", "libraryfolders.vdf");
        if (File.Exists(vdfPath))
        {
            foreach (Match match in PathRegex().Matches(File.ReadAllText(vdfPath)))
            {
                var libPath = match.Groups[1].Value.Replace(@"\\", @"\");
                var candidate = Path.Combine(libPath, "steamapps");
                if (!Directory.Exists(candidate)) continue;
                if (seenLibraryFolders.Add(NormalizeFolder(candidate)))
                    libraryFolders.Add(candidate);
            }
        }

        foreach (var library in libraryFolders)
        {
            ct.ThrowIfCancellationRequested();
            if (!Directory.Exists(library)) continue;

            foreach (var manifest in Directory.EnumerateFiles(library, "appmanifest_*.acf"))
            {
                string? name = null;
                string? installDir = null;

                foreach (var line in File.ReadLines(manifest))
                {
                    var nameMatch = NameFieldRegex().Match(line);
                    if (nameMatch.Success) name = nameMatch.Groups[1].Value;

                    var dirMatch = InstallDirFieldRegex().Match(line);
                    if (dirMatch.Success) installDir = dirMatch.Groups[1].Value;
                }

                if (name is null || installDir is null) continue;
                if (IsSteamInfrastructureEntry(name)) continue;

                var fullPath = Path.Combine(library, "common", installDir);
                if (!Directory.Exists(fullPath)) continue;

                yield return new GameProfile
                {
                    Name = name,
                    Source = GameSource.Steam,
                    InstallPath = fullPath,
                    ExecutablePath = GuessMainExecutable(fullPath)
                };
            }
        }
    }

    private static string NormalizeFolder(string path)
    {
        try { return Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)); }
        catch { return path.TrimEnd('\\', '/'); }
    }

    private static string? ReadSteamInstallPath()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            return key?.GetValue("SteamPath") as string;
        }
        catch
        {
            return null;
        }
    }

    private static IEnumerable<GameProfile> DetectEpicGames(CancellationToken ct)
    {
        var manifestsFolder = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "Epic", "EpicGamesLauncher", "Data", "Manifests");

        if (!Directory.Exists(manifestsFolder))
            yield break;

        foreach (var file in Directory.EnumerateFiles(manifestsFolder, "*.item"))
        {
            ct.ThrowIfCancellationRequested();

            GameProfile? profile = null;
            try
            {
                var json = File.ReadAllText(file);
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;

                var displayName = root.TryGetProperty("DisplayName", out var dn) ? dn.GetString() : null;
                var installLocation = root.TryGetProperty("InstallLocation", out var il) ? il.GetString() : null;
                var launchExecutable = root.TryGetProperty("LaunchExecutable", out var le) ? le.GetString() : null;

                if (string.IsNullOrWhiteSpace(displayName) || string.IsNullOrWhiteSpace(installLocation))
                    continue;

                var exePath = !string.IsNullOrWhiteSpace(launchExecutable)
                    ? Path.Combine(installLocation, launchExecutable)
                    : GuessMainExecutable(installLocation);

                profile = new GameProfile
                {
                    Name = displayName,
                    Source = GameSource.Epic,
                    InstallPath = installLocation,
                    ExecutablePath = exePath
                };
            }
            catch
            {
                // Skip unreadable/foreign manifest files.
            }

            if (profile is not null)
                yield return profile;
        }
    }

    // Shared runtime/redistributable "apps" Steam installs alongside real games — not games themselves.
    private static readonly string[] SteamInfrastructureNames =
    {
        "steamworks common redistributables", "steam linux runtime", "proton",
        "steamvr", "directx", "microsoft visual c++", ".net"
    };

    private static bool IsSteamInfrastructureEntry(string name)
    {
        var lower = name.ToLowerInvariant();
        return SteamInfrastructureNames.Any(lower.Contains);
    }

    private static string? GuessMainExecutable(string installDir)
    {
        try
        {
            var exeFiles = Directory.EnumerateFiles(installDir, "*.exe", SearchOption.AllDirectories)
                .Where(f => !f.Contains("redist", StringComparison.OrdinalIgnoreCase)
                         && !f.Contains("unins", StringComparison.OrdinalIgnoreCase)
                         && !f.Contains("crashhandler", StringComparison.OrdinalIgnoreCase)
                         && !f.Contains("vcredist", StringComparison.OrdinalIgnoreCase))
                .Select(f => new FileInfo(f))
                .OrderByDescending(f => f.Length)
                .FirstOrDefault();

            return exeFiles?.FullName;
        }
        catch
        {
            return null;
        }
    }

    [GeneratedRegex("\"path\"\\s*\"([^\"]+)\"")]
    private static partial Regex PathRegex();

    [GeneratedRegex("\"name\"\\s*\"([^\"]+)\"")]
    private static partial Regex NameFieldRegex();

    [GeneratedRegex("\"installdir\"\\s*\"([^\"]+)\"")]
    private static partial Regex InstallDirFieldRegex();
}
