using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IUpdateService
{
    string CurrentVersion { get; }

    /// <summary>The result of the most recent <see cref="CheckForUpdateAsync"/> call, if any — lets a
    /// view model hydrate itself with an already-known result instead of forcing a fresh check
    /// (e.g. after the silent startup check already ran).</summary>
    UpdateCheckResult? LastResult { get; }

    /// <summary>Queries the configured GitHub repository's latest release. Never throws —
    /// any failure (no repository configured, no internet, rate limit, malformed release)
    /// comes back as a normal, honestly-labeled result rather than a crash.</summary>
    Task<UpdateCheckResult> CheckForUpdateAsync(CancellationToken ct = default);

    /// <summary>Downloads the installer asset from the given release to a local temp file and
    /// returns its path. Reports progress as bytes arrive.</summary>
    Task<string> DownloadUpdateAsync(string downloadUrl, IProgress<UpdateDownloadProgress>? progress, CancellationToken ct = default);

    /// <summary>Launches the downloaded installer and exits the current app so it can overwrite
    /// the running executable.</summary>
    void LaunchInstallerAndExit(string installerPath);
}
