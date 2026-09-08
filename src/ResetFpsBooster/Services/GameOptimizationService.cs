using System.IO;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using Microsoft.Win32;

namespace ResetFpsBooster.Services;

/// <summary>
/// Applies the one per-game tweak that genuinely is executable-specific: disabling Windows'
/// "Fullscreen Optimizations" compatibility layer for that exe (the same flag set by
/// right-click → Properties → Compatibility → "Disable fullscreen optimizations").
/// </summary>
/// <remarks>
/// This used to also force the HIGHDPIAWARE compatibility flag onto the executable "to avoid
/// blurry upscaling on older titles". That was removed: modern games already declare their own
/// DPI awareness in their manifest, and overriding it externally can conflict with how the game
/// (or its anti-cheat) expects to run — a user reported Delta Force crashing on launch after this
/// module ran, which stopped once HIGHDPIAWARE was no longer forced. Disabling fullscreen
/// optimizations alone is the well-established, low-risk part of this tweak.
/// </remarks>
public sealed class GameOptimizationService : IGameOptimizationService
{
    private const string LayersKey = @"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers";
    private const string FlagValue = "~ DISABLEDXMAXIMIZEDWINDOWEDMODE";

    private readonly IBackupService _backupService;
    private readonly IGameLibraryService _libraryService;

    public GameOptimizationService(IBackupService backupService, IGameLibraryService libraryService)
    {
        _backupService = backupService;
        _libraryService = libraryService;
    }

    public Task<(bool Success, string Message)> OptimizeGameAsync(GameProfile profile, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(profile.ExecutablePath) || !File.Exists(profile.ExecutablePath))
            return Task.FromResult((false, "Could not find this game's executable. Point RESET FPS BOOSTER to the .exe manually and try again."));

        var recorder = new RegistryChangeRecorder($"Game: {profile.Name}");

        recorder.SetValue(
            RegistryHive.CurrentUser, LayersKey, profile.ExecutablePath, FlagValue, RegistryValueKind.String,
            $"Fullscreen optimizations — {profile.Name}",
            v => string.IsNullOrEmpty(v as string) ? "Default" : "Disabled");

        var snapshot = _backupService.CommitSnapshot(recorder, $"Game optimization: {profile.Name}");

        profile.IsOptimized = true;
        profile.LastOptimizedAt = DateTime.Now;
        profile.LastBackupSnapshotId = snapshot.Id;
        _libraryService.Save(profile);

        return Task.FromResult((true, $"Applied fullscreen-optimization fix for {profile.Name}."));
    }

    public (bool Success, string Message) RestoreGameSettings(GameProfile profile)
    {
        if (string.IsNullOrEmpty(profile.LastBackupSnapshotId))
            return (false, "No optimization has been applied to this game yet.");

        var (success, message) = _backupService.RestoreSnapshot(profile.LastBackupSnapshotId);

        if (success)
        {
            profile.IsOptimized = false;
            profile.LastBackupSnapshotId = null;
            _libraryService.Save(profile);
        }

        return (success, message);
    }
}
