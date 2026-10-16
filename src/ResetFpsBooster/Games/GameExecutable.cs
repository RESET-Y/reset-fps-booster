using System.IO;

namespace ResetFpsBooster.Games;

/// <summary>
/// WHICH PROGRAM IS THE GAME, and does a running process belong to it.
///
/// A launcher's manifest or a folder only says where a game is installed; the
/// program that actually draws the game is another matter. Unreal Engine games
/// (ARC Raiders, Delta Force, ...) ship a small starter next to a large
/// "Game-Win64-Shipping.exe" under Binaries\Win64, and it is the second that
/// runs, owns the window and the frames. Everything per program - the
/// fullscreen-optimization flag in the registry, the "is the game running"
/// checks of Game Boost and the Bottleneck Engine - must name that one.
/// </summary>
public static class GameExecutable
{
    // Titles whose game program is known by name. The folder is searched for these names, in this
    // order, before any guess. (ARC Raiders: the Unreal project is called "PioneerGame".)
    private static readonly (string Title, string[] Programs)[] KnownTitles =
    {
        ("ARC Raiders", new[] { "PioneerGame-Win64-Shipping.exe", "PioneerGame.exe" }),
    };

    /// <summary>
    /// The game program inside an install folder: a known title's own program first, then an Unreal
    /// "-Win64-Shipping" program, then the launcher's own choice (<paramref name="launcherChoice"/>,
    /// if it exists), and last the largest program that is not an installer or crash tool.
    /// </summary>
    public static string? PickMain(string installDir, string? gameName = null, string? launcherChoice = null)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(installDir) || !Directory.Exists(installDir)) return launcherChoice;

            // A folder the game's anti-cheat or the system keeps closed must not stop the search.
            var options = new EnumerationOptions
            {
                RecurseSubdirectories = true,
                IgnoreInaccessible = true,
                AttributesToSkip = FileAttributes.Hidden | FileAttributes.System | FileAttributes.ReparsePoint
            };
            var programs = Directory.EnumerateFiles(installDir, "*.exe", options)
                .Select(f => new FileInfo(f))
                .ToList();

            // 1. a title we know by name
            if (!string.IsNullOrWhiteSpace(gameName))
            {
                foreach (var (title, names) in KnownTitles)
                {
                    if (!gameName.Contains(title, StringComparison.OrdinalIgnoreCase)) continue;
                    foreach (var name in names)
                    {
                        var hit = programs
                            .Where(f => f.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
                            .OrderByDescending(f => f.Length)
                            .FirstOrDefault();
                        if (hit is not null) return hit.FullName;
                    }
                }
            }

            // 2. an Unreal game: its shipping program, not the starter beside it
            var shipping = programs
                .Where(f => IsUnrealShipping(f.FullName))
                .OrderByDescending(f => f.Length)
                .FirstOrDefault();
            if (shipping is not null) return shipping.FullName;

            // 3. what the launcher itself launches, if it is really there
            if (!string.IsNullOrWhiteSpace(launcherChoice) && File.Exists(launcherChoice)) return launcherChoice;

            // 4. the largest program that is not an installer, an anti-cheat tool or a crash tool
            return programs
                .Where(f => !f.FullName.Contains("redist", StringComparison.OrdinalIgnoreCase)
                         && !f.FullName.Contains("unins", StringComparison.OrdinalIgnoreCase)
                         && !f.FullName.Contains("crashhandler", StringComparison.OrdinalIgnoreCase)
                         && !f.FullName.Contains("vcredist", StringComparison.OrdinalIgnoreCase)
                         && !IsSupportTool(f))
                .OrderByDescending(f => f.Length)
                .FirstOrDefault()?.FullName
                ?? launcherChoice;
        }
        catch
        {
            return launcherChoice;
        }
    }

    /// <summary>True for a path that is evidently not a game: an anti-cheat tool, an installer, a setup or crash program.</summary>
    public static bool LooksLikeSupportTool(string? path) =>
        !string.IsNullOrWhiteSpace(path) && IsSupportTool(new FileInfo(path));

    // Programs that sit in a game's folder and are often the largest of all: Apex ships a 278 MB
    // "EAAntiCheat.Installer.exe" next to its 69 MB game program. Judged by the file's own name, and
    // for anti-cheat also by the folder it sits directly in (BattlEye's service is "BEService.exe"
    // inside a folder called BattlEye).
    private static bool IsSupportTool(FileInfo file) =>
        SupportToolMarks.Any(m => file.Name.Contains(m, StringComparison.OrdinalIgnoreCase))
        || AntiCheatFolderMarks.Any(m => (file.Directory?.Name ?? "").Contains(m, StringComparison.OrdinalIgnoreCase));

    private static readonly string[] SupportToolMarks =
    {
        "anticheat", "battleye", "beservice", "installer", "setup", "crashreport"
    };

    private static readonly string[] AntiCheatFolderMarks = { "anticheat", "battleye" };

    // "...\<Project>\Binaries\Win64\<Project>-Win64-Shipping.exe", but not one of the engine's own tools.
    private static bool IsUnrealShipping(string path)
    {
        if (!path.EndsWith("-Win64-Shipping.exe", StringComparison.OrdinalIgnoreCase)) return false;
        var p = path.Replace('/', '\\');
        return p.Contains("\\Binaries\\Win64\\", StringComparison.OrdinalIgnoreCase)
            && !p.Contains("\\Engine\\", StringComparison.OrdinalIgnoreCase)
            && !p.Contains("crashreport", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Does this running process belong to that library entry?
    ///
    /// Not an exact name comparison, because games ship variants of the same
    /// program: the library entry says r5apex.exe while Apex Legends in DX12 mode
    /// runs as r5apex_dx12.exe, and an Unreal game's starter "PioneerGame.exe" is
    /// followed by the process "PioneerGame-Win64-Shipping". An exact match misses
    /// those, and the miss is expensive: Game Boost then treats the game as just
    /// another background process and drops it to BelowNormal (seen on a live
    /// machine: Apex at BelowNormal while the GPU sat at 69 percent).
    /// </summary>
    public static bool IsSameProcess(string? executablePath, string? processName)
    {
        var exeName = Path.GetFileNameWithoutExtension(executablePath);
        if (string.IsNullOrEmpty(exeName) || string.IsNullOrEmpty(processName)) return false;
        if (string.Equals(exeName, processName, StringComparison.OrdinalIgnoreCase)) return true;

        // One is the other plus a build suffix. Only a suffix that starts with a separator
        // counts, so "portal" never matches "portal2".
        var (shorter, longer) = exeName.Length <= processName.Length ? (exeName, processName) : (processName, exeName);
        return longer.StartsWith(shorter, StringComparison.OrdinalIgnoreCase)
            && longer.Length > shorter.Length
            && (longer[shorter.Length] == '_' || longer[shorter.Length] == '-');
    }
}
