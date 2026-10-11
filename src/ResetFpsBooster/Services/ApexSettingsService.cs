using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using ResetFpsBooster.Core.Apex;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IApexSettingsService
{
    ApexSettingsState Read(ApexTarget target);
    ApexApplyResult Apply(ApexSettingsChoice choice, ApexTarget target);
    (bool Success, string Message) Restore(string snapshotId, ApexTarget target);
}

/// <summary>
/// APEX LEGENDS' SETTINGS, kept in one of two places, edited as the text files they are.
///
///   SettingsFiles   videoconfig.txt (graphics) and profile.cfg (field of view) in
///                   "Saved Games\Respawn\Apex". The game writes its own values over
///                   them when it closes, so nothing is written while it runs.
///   Autoexec        the same settings as game commands in one marked block at the end of
///                   cfg\autoexec.cfg in the install folder, which the game runs at
///                   start-up. The settings files are not touched, and the game cannot
///                   overwrite the block.
///
/// It never touches the game, its executables or its memory. Every change is backed up
/// first through the app's backup service, so the files come back exactly as they were,
/// read-only flag included.
/// </summary>
public sealed class ApexSettingsService : IApexSettingsService
{
    private static readonly string[] GameProcesses = { "r5apex", "r5apex_dx12" };

    private readonly IBackupService _backups;
    private readonly Func<string?> _installFolder;
    private readonly string _baseFolder;

    /// <param name="installFolder">The Apex install folder, as the game library knows it (for autoexec.cfg).</param>
    /// <param name="baseFolder">Where "local" and "profile" live. Only tests pass this; the app
    /// uses the game's own folder.</param>
    public ApexSettingsService(IBackupService backups, Func<string?>? installFolder = null, string? baseFolder = null)
    {
        _backups = backups;
        _installFolder = installFolder ?? (() => null);
        _baseFolder = baseFolder ?? GameFolder();
    }

    private static string GameFolder()
    {
        // "Saved Games" can be moved to another drive: ask Windows where it is.
        var savedGames = KnownFolder(new Guid("4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4"))
                         ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games");
        return Path.Combine(savedGames, "Respawn", "Apex");
    }

    private string VideoPath() => Path.Combine(_baseFolder, "local", "videoconfig.txt");
    private string ProfilePath() => Path.Combine(_baseFolder, "profile", "profile.cfg");

    private string? AutoexecPath()
    {
        var install = _installFolder();
        return !string.IsNullOrWhiteSpace(install) && Directory.Exists(install)
            ? Path.Combine(install, "cfg", "autoexec.cfg")
            : null;
    }

    public static bool IsGameRunning() => GameProcesses.Any(n =>
    {
        var found = Process.GetProcessesByName(n);
        var any = found.Length > 0;
        foreach (var p in found) p.Dispose();
        return any;
    });

    // ------------------------------------------------------------------ read

    public ApexSettingsState Read(ApexTarget target) =>
        target == ApexTarget.Autoexec ? ReadAutoexec() : ReadFiles();

    private ApexSettingsState ReadFiles()
    {
        var video = VideoPath();
        var profile = ProfilePath();
        var running = IsGameRunning();

        if (!File.Exists(video))
            return new ApexSettingsState(false, video, profile, false, running, false, false, false, false, false, null);

        var v = SafeRead(video) ?? "";
        string? Get(string key) => ApexConfigText.GetVideo(v, key);
        bool Off(params string[] keys) => keys.All(k => Get(k) == "0");

        return new ApexSettingsState(
            Found: true,
            VideoConfigPath: video,
            ProfilePath: profile,
            VideoConfigReadOnly: new FileInfo(video).IsReadOnly,
            GameRunning: running,
            ShadowsOff: Off("setting.csm_enabled", "setting.shadow_enable"),
            AmbientOcclusionOff: Off("setting.ssao_enabled"),
            VolumetricsOff: Off("setting.volumetric_lighting", "setting.volumetric_fog"),
            RagdollsOff: Off("setting.cl_ragdoll_maxcount"),
            DecalsOff: Off("setting.r_decals", "setting.r_createmodeldecals"),
            Fov: ProfileFov(profile),
            AutoexecPath: AutoexecPath(),
            StreamMemory: ToInt(Get("setting.stream_memory")),
            AutoexecStreamMemory: AutoexecStream());
    }

    // What autoexec.cfg alone sets for the streaming budget (null if it sets none or does not exist).
    private int? AutoexecStream()
    {
        var path = AutoexecPath();
        if (path is null || !File.Exists(path) || SafeRead(path) is not { } text) return null;
        return StreamIn(text);
    }

    private static int? StreamIn(string text) =>
        ToInt(ApexAutoexecBlock.GetEffective(text, "setting." + ApexAutoexecBlock.StreamCommand)
              ?? ApexAutoexecBlock.GetEffective(text, ApexAutoexecBlock.StreamCommand));

    private static int? ToInt(string? s) =>
        int.TryParse(s, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : null;

    private ApexSettingsState ReadAutoexec()
    {
        var video = VideoPath();
        var profile = ProfilePath();
        var path = AutoexecPath();
        if (path is null)
            return new ApexSettingsState(false, video, profile, false, false, false, false, false, false, false, null);

        var text = File.Exists(path) ? SafeRead(path) ?? "" : "";
        bool Off(string[] keys) => ApexAutoexecBlock.AllOff(text, keys);

        // What the game ends up with: the autoexec runs after the profile is loaded, so it wins.
        var fov = ApexConfigText.FovFromScale(ApexAutoexecBlock.GetEffective(text, ApexAutoexecBlock.FovCommand))
                  ?? ProfileFov(profile);

        // What this page manages: the same questions asked of the app's block alone.
        var block = BlockOf(text);
        bool BlockOff(string[] keys) => block.Length > 0 && ApexAutoexecBlock.AllOff(block, keys);

        // The streaming budget: the autoexec's own value if it sets one, else the settings file's.
        var fromAutoexec = StreamIn(text);
        var fromFile = File.Exists(video) && SafeRead(video) is { } vtext
            ? ToInt(ApexConfigText.GetVideo(vtext, "setting.stream_memory"))
            : null;

        return new ApexSettingsState(
            Found: true,
            VideoConfigPath: video,
            ProfilePath: profile,
            VideoConfigReadOnly: false,
            GameRunning: false,
            ShadowsOff: Off(ApexAutoexecBlock.Shadows),
            AmbientOcclusionOff: Off(ApexAutoexecBlock.AmbientOcclusion),
            VolumetricsOff: VolumetricsOff(text),
            RagdollsOff: Off(ApexAutoexecBlock.Ragdolls),
            DecalsOff: Off(ApexAutoexecBlock.Decals),
            Fov: fov,
            AutoexecPath: path,
            StreamMemory: fromAutoexec ?? fromFile,
            AutoexecStreamMemory: fromAutoexec)
        {
            Managed = new ApexOptionStates(
                BlockOff(ApexAutoexecBlock.Shadows),
                BlockOff(ApexAutoexecBlock.AmbientOcclusion),
                block.Length > 0 && VolumetricsOff(block),
                BlockOff(ApexAutoexecBlock.Ragdolls),
                BlockOff(ApexAutoexecBlock.Decals),
                block.Length > 0 ? StreamIn(block) : null)
        };
    }

    // Volumetrics are off if the game's own switch for them is off, or both of the video file's.
    private static bool VolumetricsOff(string text) =>
        ApexAutoexecBlock.IsOff(text, "r_volumetric_lighting_enabled") ||
        (ApexAutoexecBlock.IsOff(text, "volumetric_lighting") && ApexAutoexecBlock.IsOff(text, "volumetric_fog"));

    // The lines of the app's block, as text of their own (empty if there is none).
    private static string BlockOf(string text)
    {
        var lines = text.Replace("\r\n", "\n").Split('\n');
        var begin = Array.FindIndex(lines, l => l.TrimStart().StartsWith("// >>> RESET FPS BOOSTER: Apex Config", StringComparison.Ordinal));
        if (begin < 0) return "";
        var end = Array.FindIndex(lines, begin, l => l.TrimStart().StartsWith("// <<< RESET FPS BOOSTER: Apex Config", StringComparison.Ordinal));
        if (end < 0) end = lines.Length - 1;
        return string.Join("\n", lines[begin..(end + 1)]);
    }

    private static double? ProfileFov(string profile) =>
        File.Exists(profile) && SafeRead(profile) is { } p
            ? ApexConfigText.FovFromScale(ApexConfigText.GetProfile(p, "cl_fovScale"))
            : null;

    // ----------------------------------------------------------------- apply

    public ApexApplyResult Apply(ApexSettingsChoice c, ApexTarget target) =>
        target == ApexTarget.Autoexec ? ApplyAutoexec(c) : ApplyFiles(c);

    private ApexApplyResult ApplyFiles(ApexSettingsChoice c)
    {
        var video = VideoPath();
        var profile = ProfilePath();
        if (!File.Exists(video)) return new ApexApplyResult(ApexApplyOutcome.NotFound);
        if (IsGameRunning()) return new ApexApplyResult(ApexApplyOutcome.GameRunning);

        try
        {
            var videoOld = File.ReadAllText(video);
            var videoNew = videoOld;
            void Zero(params string[] keys) { foreach (var k in keys) videoNew = ApexConfigText.SetVideo(videoNew, k, "0"); }

            if (c.ShadowsOff) Zero("setting.csm_enabled", "setting.csm_coverage", "setting.shadow_enable", "setting.shadow_maxdynamic");
            if (c.AmbientOcclusionOff) Zero("setting.ssao_enabled");
            if (c.VolumetricsOff) Zero("setting.volumetric_lighting", "setting.volumetric_fog");
            if (c.RagdollsOff) Zero("setting.cl_ragdoll_maxcount", "setting.cl_ragdoll_self_collision");
            if (c.DecalsOff) Zero("setting.r_decals", "setting.r_createmodeldecals");
            if (c.StreamMemory is { } stream)
                videoNew = ApexConfigText.SetVideo(videoNew, "setting.stream_memory", stream.ToString(System.Globalization.CultureInfo.InvariantCulture));

            var profileExisted = File.Exists(profile);
            var profileOld = profileExisted ? File.ReadAllText(profile) : "";
            var profileNew = ApexConfigText.SetProfile(profileOld, "cl_fovScale", ApexConfigText.FormatScale(c.Fov));

            var videoChanged = videoNew != videoOld;
            var profileChanged = profileNew != profileOld;
            var lockChanged = c.LockReadOnly != new FileInfo(video).IsReadOnly;
            if (!videoChanged && !profileChanged && !lockChanged)
                return new ApexApplyResult(ApexApplyOutcome.Unchanged);

            // The way back, before anything is written.
            var backups = new List<FileContentBackup>();
            if (videoChanged || lockChanged) backups.Add(Backup(video));
            if (profileChanged) backups.Add(Backup(profile));
            var snapshot = _backups.CommitFileSnapshot("Apex Legends", "Apex Legends settings", backups);

            if (videoChanged || lockChanged)
            {
                if (videoChanged) WriteText(video, videoNew);
                SetReadOnly(video, c.LockReadOnly);
            }
            if (profileChanged)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(profile)!);
                WriteText(profile, profileNew);
            }

            return new ApexApplyResult(ApexApplyOutcome.Applied, snapshot.Id);
        }
        catch (Exception ex)
        {
            return new ApexApplyResult(ApexApplyOutcome.Failed, Detail: ex.Message);
        }
    }

    private ApexApplyResult ApplyAutoexec(ApexSettingsChoice c)
    {
        var path = AutoexecPath();
        if (path is null) return new ApexApplyResult(ApexApplyOutcome.NotFound);

        try
        {
            var existed = File.Exists(path);
            var old = existed ? File.ReadAllText(path) : "";

            // The field of view is only set when the slider was moved; otherwise what the block
            // already says stays, and the game's own setting is left alone.
            // A slider put back to what the profile has needs no override: the block drops the line.
            string? fovScale = null;
            if (c.FovChanged)
            {
                var own = ProfileFov(ProfilePath());
                if (own is null || Math.Abs(c.Fov - own.Value) > 0.5) fovScale = ApexConfigText.FormatScale(c.Fov);
            }
            else if (ApexAutoexecBlock.GetInBlock(old, ApexAutoexecBlock.FovCommand) is { Length: > 0 } kept) fovScale = kept;

            var updated = ApexAutoexecBlock.Apply(old, ApexAutoexecBlock.BuildLines(c, fovScale));
            if (updated == old) return new ApexApplyResult(ApexApplyOutcome.Unchanged);

            var snapshot = _backups.CommitFileSnapshot("Apex Legends", "Apex Legends autoexec", new[] { Backup(path) });
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            WriteText(path, updated);
            return new ApexApplyResult(ApexApplyOutcome.Applied, snapshot.Id);
        }
        catch (Exception ex)
        {
            return new ApexApplyResult(ApexApplyOutcome.Failed, Detail: ex.Message);
        }
    }

    public (bool Success, string Message) Restore(string snapshotId, ApexTarget target)
    {
        // The settings files would be overwritten again by a running game; the autoexec is not.
        if (target == ApexTarget.SettingsFiles && IsGameRunning()) return (false, "running");
        return _backups.RestoreSnapshot(snapshotId);
    }

    // --------------------------------------------------------------- helpers

    private static FileContentBackup Backup(string path)
    {
        var exists = File.Exists(path);
        return new FileContentBackup
        {
            TargetPath = path,
            FileExisted = exists,
            PreviousContentBase64 = exists ? Convert.ToBase64String(File.ReadAllBytes(path)) : null,
            WasReadOnly = exists && new FileInfo(path).IsReadOnly
        };
    }

    // The game writes plain ASCII/UTF-8 without a byte order mark; keep it that way.
    private static void WriteText(string path, string text)
    {
        SetReadOnly(path, false);
        File.WriteAllText(path, text, new UTF8Encoding(false));
    }

    private static void SetReadOnly(string path, bool readOnly)
    {
        if (!File.Exists(path)) return;
        var info = new FileInfo(path);
        if (info.IsReadOnly != readOnly) info.IsReadOnly = readOnly;
    }

    private static string? SafeRead(string path)
    {
        try { return File.ReadAllText(path); }
        catch { return null; }
    }

    private static string? KnownFolder(Guid id)
    {
        try
        {
            return SHGetKnownFolderPath(ref id, 0, IntPtr.Zero, out var path) == 0 ? path : null;
        }
        catch { return null; }
    }

    [DllImport("shell32.dll")]
    private static extern int SHGetKnownFolderPath(ref Guid folder, uint flags, IntPtr token,
                                                   [MarshalAs(UnmanagedType.LPWStr)] out string path);
}
