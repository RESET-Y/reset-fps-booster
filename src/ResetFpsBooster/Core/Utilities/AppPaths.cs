using System.IO;

namespace ResetFpsBooster.Core.Utilities;

public static class AppPaths
{
    public static string RootFolder { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ResetFpsBooster");

    public static string BackupsFolder { get; } = Path.Combine(RootFolder, "Backups");
    public static string LogsFolder { get; } = Path.Combine(RootFolder, "Logs");
    public static string SettingsFile { get; } = Path.Combine(RootFolder, "settings.json");
    public static string GamesFile { get; } = Path.Combine(RootFolder, "games.json");
    public static string ChangeLogFile { get; } = Path.Combine(LogsFolder, "changelog.json");

    public static void EnsureFoldersExist()
    {
        Directory.CreateDirectory(RootFolder);
        Directory.CreateDirectory(BackupsFolder);
        Directory.CreateDirectory(LogsFolder);
    }
}
