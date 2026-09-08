using System.IO;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public sealed class GameAutoexecService : IGameAutoexecService
{
    private readonly IBackupService _backupService;
    private readonly IGameLibraryService _libraryService;
    private readonly List<GameAutoexecDefinition> _definitions;

    public GameAutoexecService(IBackupService backupService, IGameLibraryService libraryService)
    {
        _backupService = backupService;
        _libraryService = libraryService;
        _definitions = BuildDefinitions();
    }

    public bool SupportsAutoexec(GameProfile profile) => FindDefinition(profile) is not null;

    public string? GetSupportedGameName(GameProfile profile) => FindDefinition(profile)?.DisplayName;

    public string? GetSteamLaunchOption(GameProfile profile) => FindDefinition(profile)?.LaunchOption;

    public Task<(bool Success, string Message)> ApplyAsync(GameProfile profile, CancellationToken ct = default)
    {
        var definition = FindDefinition(profile);
        if (definition is null)
            return Task.FromResult((false, $"No performance autoexec is available for {profile.Name} yet."));

        if (!Directory.Exists(profile.InstallPath))
            return Task.FromResult((false, $"Could not find the {definition.DisplayName} install folder. Rescan or point the game manually and try again."));

        var cfgFolder = Path.Combine(profile.InstallPath, definition.CfgRelativeFolder);

        try
        {
            Directory.CreateDirectory(cfgFolder);
            var targetPath = Path.Combine(cfgFolder, "autoexec.cfg");

            var fileExisted = File.Exists(targetPath);
            var fileBackup = new FileContentBackup
            {
                TargetPath = targetPath,
                FileExisted = fileExisted,
                PreviousContentBase64 = fileExisted ? Convert.ToBase64String(File.ReadAllBytes(targetPath)) : null
            };

            File.WriteAllText(targetPath, definition.Content);

            var snapshot = _backupService.CommitFileSnapshot(
                $"{definition.DisplayName} Autoexec", $"{definition.DisplayName} FPS Autoexec — {profile.Name}", new[] { fileBackup });

            profile.IsAutoexecApplied = true;
            profile.AutoexecAppliedAt = DateTime.Now;
            profile.AutoexecSnapshotId = snapshot.Id;
            _libraryService.Save(profile);

            return Task.FromResult((true,
                $"Autoexec installed. Add \"{definition.LaunchOption}\" to {definition.DisplayName}'s Steam launch options so it actually loads on startup."));
        }
        catch (Exception ex)
        {
            return Task.FromResult((false, $"Could not write autoexec.cfg: {ex.Message}"));
        }
    }

    public (bool Success, string Message) Restore(GameProfile profile)
    {
        if (string.IsNullOrEmpty(profile.AutoexecSnapshotId))
            return (false, "No autoexec has been installed for this game yet.");

        var (success, message) = _backupService.RestoreSnapshot(profile.AutoexecSnapshotId);

        if (success)
        {
            profile.IsAutoexecApplied = false;
            profile.AutoexecSnapshotId = null;
            _libraryService.Save(profile);
        }

        return (success, message);
    }

    private GameAutoexecDefinition? FindDefinition(GameProfile profile) => _definitions.FirstOrDefault(d => d.Matches(profile));

    private static bool ExecutableIs(GameProfile profile, string fileName) =>
        !string.IsNullOrEmpty(profile.ExecutablePath) && string.Equals(Path.GetFileName(profile.ExecutablePath), fileName, StringComparison.OrdinalIgnoreCase);

    private sealed record GameAutoexecDefinition(
        string DisplayName,
        Func<GameProfile, bool> Matches,
        string CfgRelativeFolder,
        string LaunchOption,
        string Content);

    private static List<GameAutoexecDefinition> BuildDefinitions() => new()
    {
        new GameAutoexecDefinition(
            DisplayName: "Apex Legends",
            Matches: p => ExecutableIs(p, "r5apex.exe") || p.Name.Contains("Apex Legends", StringComparison.OrdinalIgnoreCase),
            CfgRelativeFolder: "cfg",
            LaunchOption: "+exec autoexec",
            Content: ApexAutoexecContent),

        new GameAutoexecDefinition(
            DisplayName: "Counter-Strike 2",
            Matches: p => ExecutableIs(p, "cs2.exe") || p.Name.Contains("Counter-Strike", StringComparison.OrdinalIgnoreCase),
            CfgRelativeFolder: Path.Combine("game", "csgo", "cfg"),
            LaunchOption: "+exec autoexec.cfg",
            Content: Cs2AutoexecContent),
    };

    // Performance-only cvars — no sensitivity, audio-cue, or netcode changes. Every setting here
    // only reduces local rendering cost; verified against Apex's real (Source-derived) console
    // variables via community-maintained, widely-used autoexecs.
    private const string ApexAutoexecContent = """
        // ============================================================
        // Managed by RESET FPS BOOSTER — safe to delete or use Restore
        // in the app to revert to the file that was here before.
        // Performance-only: no sensitivity, audio, or netcode changes.
        // ============================================================

        fps_max 0                          // Uncap the frame rate
        mat_vsync 0                        // Disable V-Sync

        // Rendering cost reduction
        r_shadows 0                        // Disable shadows
        r_shadowmaxrendered 0
        shadow_maxdynamic 0
        cl_ragdoll_maxcount 0              // Disable ragdolls
        cl_ragdoll_self_collision 0
        particle_cpu_level 0               // Reduce particle quality
        mat_bloomscale 0                   // Disable bloom
        mat_bloom_scalefactor_scalar 0
        mat_disable_bloom 1
        r_waterdrawreflection 0            // Disable water reflections
        r_waterforceexpensive 0
        r_forcecheapwater 1
        ssao_enabled 0                     // Disable ambient occlusion
        mat_depthfeather_enable 0          // Disable depth of field
        noise_filter_scale 0               // Disable film grain
        r_createmodeldecals 0              // Skip bullet-impact decals
        r_decalstaticprops 0
        r_cleardecals 1
        fog_enable 0                       // Disable fog
        mat_dynamic_tonemapping 0          // Disable dynamic HDR tonemapping
        mat_hdr_level 0
        r_jiggle_bones 0                   // Disable jiggle-bone simulation
        cl_jiggle_bone_framerate_cutoff 0
        mat_mipmaptextures 0               // Fast texture streaming
        stream_drop_unused 1               // Drop unused textures aggressively
        r_flex 0                           // Disable facial animation
        r_eyes 0
        r_teeth 0
        mp_usehwmmodels -1                 // Skip high-quality character models
        mp_usehwmvcds -1
        cl_phys_props_enable 0             // Reduce physics-prop simulation
        cl_phys_props_max 0
        """;

    // CS2 runs on Source 2, whose exposed client cvars are far more limited than the old Source 1
    // games — most of the classic "mat_*"/"r_*" toggles no longer exist or are handled internally
    // by the renderer. This set is intentionally short: only cvars verified against the actively
    // maintained, community-trusted ArminC-AutoExec project (376+ stars, CC0), and only the ones
    // that are pure engine/rendering performance with no gameplay-feel or competitive side effects
    // (e.g. deliberately excluding netcode-prediction and interpolation cvars, which trade fairness
    // or connection stability for perceived responsiveness).
    private const string Cs2AutoexecContent = """
        // ============================================================
        // Managed by RESET FPS BOOSTER — safe to delete or use Restore
        // in the app to revert to the file that was here before.
        // Performance-only: no sensitivity, audio, or netcode changes.
        // ============================================================

        fps_max 0                          // Uncap the frame rate in-game
        fps_max_ui 60                      // Cap the frame rate in menus — saves GPU/heat for no visible benefit there
        thread_pool_option 2               // Prefer performance cores on hybrid CPUs (Intel 12th-gen+/AMD equivalents)
        """;
}
