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

    /// The signed-in session, encrypted with DPAPI for the current Windows user.
    /// Never plain text: it holds a refresh token that can sign in as that user.
    public static string SessionFile { get; } = Path.Combine(RootFolder, "session.bin");
    public static string ChangeLogFile { get; } = Path.Combine(LogsFolder, "changelog.json");

    public static void EnsureFoldersExist()
    {
        Directory.CreateDirectory(RootFolder);
        Directory.CreateDirectory(BackupsFolder);
        Directory.CreateDirectory(LogsFolder);
    }
}
