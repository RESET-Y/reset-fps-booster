using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

/// <summary>
/// Checks GitHub Releases for a newer version. GitHub's public releases API needs no
/// authentication for public repos, which keeps this genuinely simple and free to run — no
/// backend of our own to host or maintain. If no repository is configured yet, every call
/// reports that honestly instead of failing silently or pretending to be up to date.
/// </summary>
public sealed class UpdateService : IUpdateService
{
    private readonly ISettingsService _settingsService;
    private static readonly HttpClient Http = CreateHttpClient();

    public string CurrentVersion => AppVersionInfo.Current;

    public UpdateCheckResult? LastResult { get; private set; }

    public UpdateService(ISettingsService settingsService)
    {
        _settingsService = settingsService;
    }

    public async Task<UpdateCheckResult> CheckForUpdateAsync(CancellationToken ct = default)
    {
        var result = await CheckForUpdateCoreAsync(ct);
        LastResult = result;
        return result;
    }

    private async Task<UpdateCheckResult> CheckForUpdateCoreAsync(CancellationToken ct)
    {
        var repository = _settingsService.Current.UpdateRepository?.Trim();

        if (string.IsNullOrEmpty(repository) || !repository.Contains('/'))
        {
            return new UpdateCheckResult
            {
                Success = false,
                CurrentVersion = CurrentVersion,
                ErrorMessage = "No update repository configured yet. Set it in Settings as \"owner/repo\"."
            };
        }

        try
        {
            using var response = await Http.GetAsync($"https://api.github.com/repos/{repository}/releases/latest", ct);

            if (!response.IsSuccessStatusCode)
            {
                var reason = response.StatusCode == System.Net.HttpStatusCode.NotFound
                    ? "Repository or release not found. Check the \"owner/repo\" value and that a release has been published."
                    : $"GitHub returned {(int)response.StatusCode} ({response.ReasonPhrase}).";
                return new UpdateCheckResult { Success = false, CurrentVersion = CurrentVersion, ErrorMessage = reason };
            }

            await using var stream = await response.Content.ReadAsStreamAsync(ct);
            using var doc = await JsonDocument.ParseAsync(stream, cancellationToken: ct);
            var root = doc.RootElement;

            var tagName = root.TryGetProperty("tag_name", out var tag) ? tag.GetString() : null;
            var body = root.TryGetProperty("body", out var b) ? b.GetString() : null;

            if (string.IsNullOrWhiteSpace(tagName))
                return new UpdateCheckResult { Success = false, CurrentVersion = CurrentVersion, ErrorMessage = "The latest release has no version tag." };

            string? downloadUrl = null;
            if (root.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
            {
                foreach (var asset in assets.EnumerateArray())
                {
                    var name = asset.TryGetProperty("name", out var n) ? n.GetString() ?? string.Empty : string.Empty;
                    if (name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                    {
                        downloadUrl = asset.TryGetProperty("browser_download_url", out var u) ? u.GetString() : null;
                        break;
                    }
                }
            }

            var latestVersion = tagName.TrimStart('v', 'V');
            var isNewer = TryCompareVersions(latestVersion, CurrentVersion, out var comparison) && comparison > 0;

            _settingsService.Current.LastUpdateCheckUtc = DateTime.UtcNow;
            _settingsService.Save();

            return new UpdateCheckResult
            {
                Success = true,
                IsUpdateAvailable = isNewer,
                CurrentVersion = CurrentVersion,
                LatestVersion = latestVersion,
                DownloadUrl = downloadUrl,
                ReleaseNotes = body
            };
        }
        catch (TaskCanceledException)
        {
            return new UpdateCheckResult { Success = false, CurrentVersion = CurrentVersion, ErrorMessage = "The update check timed out." };
        }
        catch (HttpRequestException ex)
        {
            return new UpdateCheckResult { Success = false, CurrentVersion = CurrentVersion, ErrorMessage = $"Could not reach GitHub: {ex.Message}" };
        }
        catch (Exception ex)
        {
            return new UpdateCheckResult { Success = false, CurrentVersion = CurrentVersion, ErrorMessage = $"Unexpected error while checking for updates: {ex.Message}" };
        }
    }

    public async Task<string> DownloadUpdateAsync(string downloadUrl, IProgress<UpdateDownloadProgress>? progress, CancellationToken ct = default)
    {
        var destination = Path.Combine(Path.GetTempPath(), $"ResetFpsBooster_Update_{Guid.NewGuid():N}.exe");

        using var response = await Http.GetAsync(downloadUrl, HttpCompletionOption.ResponseHeadersRead, ct);
        response.EnsureSuccessStatusCode();

        var totalBytes = response.Content.Headers.ContentLength;
        await using var contentStream = await response.Content.ReadAsStreamAsync(ct);
        await using var fileStream = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None, 81920, useAsync: true);

        var buffer = new byte[81920];
        long totalRead = 0;
        int read;

        while ((read = await contentStream.ReadAsync(buffer, ct)) > 0)
        {
            await fileStream.WriteAsync(buffer.AsMemory(0, read), ct);
            totalRead += read;
            progress?.Report(new UpdateDownloadProgress { BytesReceived = totalRead, TotalBytes = totalBytes });
        }

        return destination;
    }

    public void LaunchInstallerAndExit(string installerPath)
    {
        Process.Start(new ProcessStartInfo(installerPath) { UseShellExecute = true });
        Environment.Exit(0);
    }

    private static bool TryCompareVersions(string a, string b, out int comparison)
    {
        comparison = 0;
        // Release tags occasionally carry a suffix like "-beta"; keep only the numeric part.
        var aNumeric = new string(a.TakeWhile(c => char.IsDigit(c) || c == '.').ToArray());
        var bNumeric = new string(b.TakeWhile(c => char.IsDigit(c) || c == '.').ToArray());

        if (!Version.TryParse(aNumeric, out var versionA) || !Version.TryParse(bNumeric, out var versionB))
            return false;

        comparison = versionA.CompareTo(versionB);
        return true;
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("ResetFpsBooster", AppVersionInfo.Current));
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return client;
    }
}
