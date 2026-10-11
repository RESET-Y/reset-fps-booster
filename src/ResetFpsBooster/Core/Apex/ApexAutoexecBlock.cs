using System.Text.RegularExpressions;

namespace ResetFpsBooster.Core.Apex;

/// <summary>
/// THE SAME SETTINGS, AS GAME COMMANDS IN AUTOEXEC.CFG.
///
/// Apex runs the commands in cfg\autoexec.cfg at start-up (given the Steam
/// launch option "+exec autoexec"), so every setting can be given there as a
/// command instead of being written to videoconfig.txt and profile.cfg. The
/// app's part of the file is one marked block at the very end - later lines win,
/// so it settles anything above it - and everything the player wrote stays as
/// it was. Applying again replaces the block; switching everything off removes it.
///
/// Each setting is written under both of its names, with and without the
/// "setting." prefix the video file uses: community configs use either, and a
/// command the game does not know is simply ignored.
/// </summary>
public static class ApexAutoexecBlock
{
    public const string Begin = "// >>> RESET FPS BOOSTER: Apex Config (managed block, change it on the Apex Config page) >>>";
    public const string End = "// <<< RESET FPS BOOSTER: Apex Config <<<";

    private const string BeginMark = "// >>> RESET FPS BOOSTER: Apex Config";
    private const string EndMark = "// <<< RESET FPS BOOSTER: Apex Config";

    // The commands behind each option (the part after "setting." in videoconfig.txt).
    public static readonly string[] Shadows = { "csm_enabled", "csm_coverage", "shadow_enable", "shadow_maxdynamic" };
    public static readonly string[] AmbientOcclusion = { "ssao_enabled" };
    public static readonly string[] Volumetrics = { "volumetric_lighting", "volumetric_fog", "r_volumetric_lighting_enabled" };
    public static readonly string[] Ragdolls = { "cl_ragdoll_maxcount", "cl_ragdoll_self_collision" };
    public static readonly string[] Decals = { "r_decals", "r_createmodeldecals" };

    /// <summary>The field of view is one command, no prefix.</summary>
    public const string FovCommand = "cl_fovScale";

    /// <summary>The texture streaming budget (see ApexStreaming), written under both names like the rest.</summary>
    public const string StreamCommand = "stream_memory";

    // ---------------------------------------------------------------- writing

    /// <summary>The block for these choices; empty when nothing is to be set.</summary>
    public static IReadOnlyList<string> BuildLines(ApexSettingsChoice c, string? fovScale)
    {
        var body = new List<string>();
        void Zero(string[] keys)
        {
            foreach (var k in keys)
            {
                body.Add($"{k} \"0\"");
                if (k != "r_volumetric_lighting_enabled") body.Add($"setting.{k} \"0\"");
            }
        }

        if (c.ShadowsOff) Zero(Shadows);
        if (c.AmbientOcclusionOff) Zero(AmbientOcclusion);
        if (c.VolumetricsOff) Zero(Volumetrics);
        if (c.RagdollsOff) Zero(Ragdolls);
        if (c.DecalsOff) Zero(Decals);
        if (c.StreamMemory is { } stream)
        {
            body.Add($"{StreamCommand} \"{stream}\"");
            body.Add($"setting.{StreamCommand} \"{stream}\"");
        }
        if (fovScale is not null) body.Add($"{FovCommand} \"{fovScale}\"");

        if (body.Count == 0) return Array.Empty<string>();
        var lines = new List<string> { Begin };
        lines.AddRange(body);
        lines.Add(End);
        return lines;
    }

    /// <summary>The file without the app's block (and the blank line that set it apart).</summary>
    public static string Remove(string text)
    {
        var eol = text.Contains("\r\n") ? "\r\n" : "\n";
        var lines = text.Replace("\r\n", "\n").Split('\n').ToList();
        var begin = lines.FindIndex(l => l.TrimStart().StartsWith(BeginMark, StringComparison.Ordinal));
        if (begin < 0) return text;

        var end = lines.FindIndex(begin, l => l.TrimStart().StartsWith(EndMark, StringComparison.Ordinal));
        if (end < 0) end = lines.Count - 1;      // a block someone cut short: take what is left of it
        lines.RemoveRange(begin, end - begin + 1);
        if (begin > 0 && begin - 1 < lines.Count && lines[begin - 1].Length == 0 && (begin >= lines.Count || lines[begin].Length == 0))
            lines.RemoveAt(begin - 1);
        return string.Join(eol, lines);
    }

    /// <summary>The file with the block replaced by this one, at the end.</summary>
    public static string Apply(string text, IReadOnlyList<string> block)
    {
        var eol = text.Contains("\r\n") || text.Length == 0 ? "\r\n" : "\n";
        var rest = Remove(text);
        if (block.Count == 0) return rest;

        var sb = new System.Text.StringBuilder(rest);
        if (sb.Length > 0 && !rest.EndsWith('\n')) sb.Append(eol);
        if (sb.Length > 0) sb.Append(eol);        // a blank line between the player's lines and ours
        foreach (var line in block) sb.Append(line).Append(eol);
        return sb.ToString();
    }

    // ---------------------------------------------------------------- reading

    private static Regex Command(string key) =>
        new($"^\\s*{Regex.Escape(key)}\\s+\"([^\"]*)\"", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    /// <summary>What the file finally makes of a command: the last line that sets it wins.</summary>
    public static string? GetEffective(string text, string key)
    {
        var rx = Command(key);
        string? value = null;
        foreach (var line in text.Replace("\r\n", "\n").Split('\n'))
        {
            var m = rx.Match(line);
            if (m.Success) value = m.Groups[1].Value;
        }
        return value;
    }

    /// <summary>The value of a command inside the app's block only (null if the block lacks it).</summary>
    public static string? GetInBlock(string text, string key)
    {
        var rx = Command(key);
        var inside = false;
        string? value = null;
        foreach (var line in text.Replace("\r\n", "\n").Split('\n'))
        {
            var t = line.TrimStart();
            if (t.StartsWith(BeginMark, StringComparison.Ordinal)) { inside = true; continue; }
            if (t.StartsWith(EndMark, StringComparison.Ordinal)) { inside = false; continue; }
            if (!inside) continue;
            var m = rx.Match(line);
            if (m.Success) value = m.Groups[1].Value;
        }
        return value;
    }

    /// <summary>True when the file makes this command 0, under either spelling.</summary>
    public static bool IsOff(string text, string key) =>
        (GetEffective(text, "setting." + key) ?? GetEffective(text, key)) == "0";

    /// <summary>True when the file makes every one of these commands 0.</summary>
    public static bool AllOff(string text, string[] keys) => keys.All(k => IsOff(text, k));
}
