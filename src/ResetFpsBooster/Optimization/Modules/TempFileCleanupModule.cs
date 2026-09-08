using System.IO;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Optimization.Modules;

/// <summary>
/// Deletes leftover files from the user and system temp folders. This is the one module whose
/// effect cannot be undone (deleted files are gone), so it is always presented to the user with
/// an explicit "this cannot be undone" notice before running.
/// </summary>
public sealed class TempFileCleanupModule : IOptimizationModule
{
    private const long CleanThresholdBytes = 200 * 1024 * 1024; // 200 MB
    private static readonly TimeSpan CacheLifetime = TimeSpan.FromSeconds(45);

    private long? _cachedSize;
    private DateTime _cachedAt;

    public string Id => "storage.temp-cleanup";
    public string Name => "Temporary File Cleanup";
    public string Description => "Deletes leftover files from your user and system temp folders. This action cannot be undone.";
    public OptimizationCategory Category => OptimizationCategory.Storage;
    public RiskLevel Risk => RiskLevel.Low;
    public bool RequiresAdmin => false;
    public bool RequiresReboot => false;

    public Task<OptimizationStatus> CheckStatusAsync(CancellationToken ct = default)
    {
        // Walking every file under %TEMP% can take a while on machines with a large browser
        // cache — cache the result briefly so re-opening the Optimizer page doesn't re-scan
        // the whole folder tree every single time.
        long size;
        if (_cachedSize is { } cached && DateTime.UtcNow - _cachedAt < CacheLifetime)
        {
            size = cached;
        }
        else
        {
            size = CalculateTempSize();
            _cachedSize = size;
            _cachedAt = DateTime.UtcNow;
        }

        return Task.FromResult(new OptimizationStatus
        {
            IsAvailable = true,
            IsApplied = size < CleanThresholdBytes,
            DetailText = $"Reclaimable: {FormatBytes(size)}"
        });
    }

    public Task<OptimizationApplyResult> ApplyAsync(RegistryChangeRecorder recorder, CancellationToken ct = default)
    {
        var freed = 0L;
        var folders = new[] { Path.GetTempPath(), Environment.ExpandEnvironmentVariables(@"%WINDIR%\Temp") };

        foreach (var folder in folders)
        {
            freed += DeleteContents(folder, ct);
        }

        recorder.ChangeLog.Add(new ChangeLogEntry
        {
            ModuleName = Name,
            SettingName = "Temporary files",
            OldValue = FormatBytes(freed),
            NewValue = "0 B"
        });

        _cachedSize = null;

        return Task.FromResult(OptimizationApplyResult.Ok($"Freed {FormatBytes(freed)} of temporary files.", recorder.ChangeLog));
    }

    private static long DeleteContents(string folder, CancellationToken ct)
    {
        long freed = 0;
        if (!Directory.Exists(folder)) return 0;

        foreach (var file in SafeEnumerateFiles(folder))
        {
            ct.ThrowIfCancellationRequested();
            try
            {
                var info = new FileInfo(file);
                var length = info.Length;
                info.Delete();
                freed += length;
            }
            catch
            {
                // File is in use / access denied — skip it and keep going, never crash the cleanup.
            }
        }

        foreach (var dir in SafeEnumerateDirectories(folder))
        {
            try
            {
                if (!Directory.EnumerateFileSystemEntries(dir).Any())
                    Directory.Delete(dir);
            }
            catch
            {
                // Non-empty or locked — leave it.
            }
        }

        return freed;
    }

    private static IEnumerable<string> SafeEnumerateFiles(string folder)
    {
        try { return Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).ToList(); }
        catch { return Enumerable.Empty<string>(); }
    }

    private static IEnumerable<string> SafeEnumerateDirectories(string folder)
    {
        try { return Directory.EnumerateDirectories(folder, "*", SearchOption.AllDirectories).OrderByDescending(d => d.Length).ToList(); }
        catch { return Enumerable.Empty<string>(); }
    }

    private static long CalculateTempSize()
    {
        long total = 0;
        foreach (var folder in new[] { Path.GetTempPath(), Environment.ExpandEnvironmentVariables(@"%WINDIR%\Temp") })
        {
            foreach (var file in SafeEnumerateFiles(folder))
            {
                try { total += new FileInfo(file).Length; }
                catch { /* skip inaccessible file */ }
            }
        }
        return total;
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = { "B", "KB", "MB", "GB" };
        double size = bytes;
        var unit = 0;
        while (size >= 1024 && unit < units.Length - 1)
        {
            size /= 1024;
            unit++;
        }
        return $"{size:0.#} {units[unit]}";
    }
}
