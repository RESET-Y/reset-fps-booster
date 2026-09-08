using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

/// <summary>
/// Installs a curated, performance-only autoexec config for supported titles — the same
/// well-established technique those communities have used for years (a config file the engine
/// auto-loads via a Steam launch option). Every cvar is client-side rendering/engine performance
/// only; nothing here touches netcode, audio cues, sensitivity, or anything that could be read as
/// a competitive-advantage exploit.
/// </summary>
public interface IGameAutoexecService
{
    bool SupportsAutoexec(GameProfile profile);

    /// <summary>Display name of the matched title, e.g. "Apex Legends" — null if unsupported.</summary>
    string? GetSupportedGameName(GameProfile profile);

    /// <summary>The Steam launch option the user must add manually — RESET FPS BOOSTER cannot set this
    /// itself, since Steam exposes no safe API for it. Null if unsupported.</summary>
    string? GetSteamLaunchOption(GameProfile profile);

    Task<(bool Success, string Message)> ApplyAsync(GameProfile profile, CancellationToken ct = default);
    (bool Success, string Message) Restore(GameProfile profile);
}
