namespace ResetFpsBooster.Core.Apex;

/// <summary>Where the settings are kept: the game's two settings files, or the commands in autoexec.cfg.</summary>
public enum ApexTarget { SettingsFiles, Autoexec }

/// <summary>Which of the options are in place.</summary>
public sealed record ApexOptionStates(bool Shadows, bool AmbientOcclusion, bool Volumetrics, bool Ragdolls, bool Decals, int? StreamMemory = null);

/// <summary>
/// THE TEXTURE STREAMING BUDGET, in the game's own steps. The number is not a byte count but a
/// budget tier; these are the values of the game's menu entries as players have worked them out
/// (Respawn publishes none), so the page says so. 0 and anything below the first step leave the
/// game with almost no room for sharp textures.
/// </summary>
public static class ApexStreaming
{
    public sealed record Tier(int Value, string Key);

    public static readonly IReadOnlyList<Tier> Tiers = new[]
    {
        new Tier(160000, "VeryLow"),     // about 2 GB
        new Tier(300000, "Low"),         // 2 - 3 GB
        new Tier(600000, "Medium"),      // 3 GB
        new Tier(1000000, "High"),       // 4 GB
        new Tier(2000000, "VeryHigh"),   // 6 GB
        new Tier(3000000, "Extreme"),    // 8 GB
    };

    /// <summary>Below the smallest step of the game's menu: textures stay blurry.</summary>
    public static bool IsTooLow(int? value) => value is { } v && v < Tiers[0].Value;
}

/// <summary>What the two settings files say right now.</summary>
public sealed record ApexSettingsState(
    bool Found,
    string VideoConfigPath,
    string ProfilePath,
    bool VideoConfigReadOnly,
    bool GameRunning,
    bool ShadowsOff,
    bool AmbientOcclusionOff,
    bool VolumetricsOff,
    bool RagdollsOff,
    bool DecalsOff,
    /// <summary>The field of view the game ends up with, in the number its menu uses; null if none is set.</summary>
    double? Fov,
    /// <summary>The autoexec.cfg this page would edit (may not exist yet); null if the Apex folder is unknown.</summary>
    string? AutoexecPath = null,
    /// <summary>The texture streaming budget the game ends up with on this target; null if none is set.</summary>
    int? StreamMemory = null,
    /// <summary>What autoexec.cfg alone says about it, whatever the target: it runs after the settings files
    /// and wins, so a value here quietly overrides the settings files at every start. Null if it sets none.</summary>
    int? AutoexecStreamMemory = null)
{
    /// <summary>For autoexec: what the app's own block sets (the options above are what the whole file ends up with).
    /// Null for the settings files, where there is only one answer.</summary>
    public ApexOptionStates? Managed { get; init; }
}

/// <summary>What the player wants changed. An option left off means "leave that as it is".</summary>
public sealed record ApexSettingsChoice(
    bool ShadowsOff,
    bool AmbientOcclusionOff,
    bool VolumetricsOff,
    bool RagdollsOff,
    bool DecalsOff,
    double Fov,
    /// <summary>Whether videoconfig.txt is read-only afterwards. The app never sets this on its own:
    /// the page starts from what the file has now, so the player's own choice is kept.</summary>
    bool LockReadOnly,
    /// <summary>The slider was moved. With autoexec as the target the field of view is only written then
    /// (or kept as the block already has it), so the game's own FOV setting is not overridden for nothing.</summary>
    bool FovChanged = true,
    /// <summary>The texture streaming budget to set; null leaves it as it is.</summary>
    int? StreamMemory = null);

public enum ApexApplyOutcome { Applied, Unchanged, NotFound, GameRunning, Failed }

public sealed record ApexApplyResult(ApexApplyOutcome Outcome, string? SnapshotId = null, string? Detail = null);
