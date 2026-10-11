using System.Globalization;
using System.Text.RegularExpressions;

namespace ResetFpsBooster.Core.Apex;

/// <summary>
/// READING AND CHANGING THE TWO TEXT FILES APEX KEEPS ITS SETTINGS IN, as plain
/// text, line by line, so that everything else in them stays exactly as it was
/// (key order, spacing, the line endings the game wrote).
///
///   videoconfig.txt   a block of  "setting.name"  "value"  lines between braces
///   profile.cfg       one  name "value"  line per setting
///
/// Nothing here touches a file; the service reads and writes, this only edits text.
/// </summary>
public static class ApexConfigText
{
    // Groups: 1 = what comes before the key, 2 = the key as the file spells it,
    //         3 = what sits between key and value, 4 = the value, 5 = the rest of the line.
    private static Regex VideoLine(string key) =>
        new($"^(\\s*\")({Regex.Escape(key)})(\"\\s+\")([^\"]*)(\".*)$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    private static Regex ProfileLine(string key) =>
        new($"^(\\s*)({Regex.Escape(key)})(\\s+\")([^\"]*)(\".*)$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    // ---------------------------------------------------------------- videoconfig.txt

    public static string? GetVideo(string text, string key) => Get(text, VideoLine(key));

    /// <summary>Sets a value; a key the file does not have yet is added before the closing brace.</summary>
    public static string SetVideo(string text, string key, string value)
    {
        var eol = text.Contains("\r\n") ? "\r\n" : "\n";
        var lines = Lines(text).ToList();
        if (Replace(lines, VideoLine(key), value)) return string.Join(eol, lines);

        var close = lines.FindLastIndex(l => l.Trim() == "}");
        var added = $"\t\"{key}\"\t\t\"{value}\"";
        if (close >= 0) lines.Insert(close, added);
        else lines.Add(added);
        return string.Join(eol, lines);
    }

    // ---------------------------------------------------------------- profile.cfg

    public static string? GetProfile(string text, string key) => Get(text, ProfileLine(key));

    /// <summary>Sets a value; a setting the file does not have yet is appended at the end.</summary>
    public static string SetProfile(string text, string key, string value)
    {
        var eol = text.Contains("\r\n") ? "\r\n" : "\n";
        var lines = Lines(text).ToList();
        if (Replace(lines, ProfileLine(key), value)) return string.Join(eol, lines);

        // before the final empty piece that a trailing line break leaves behind
        var at = lines.Count > 0 && lines[^1].Length == 0 ? lines.Count - 1 : lines.Count;
        lines.Insert(at, $"{key} \"{value}\"");
        return string.Join(eol, lines);
    }

    // ---------------------------------------------------------------- field of view

    /// <summary>The in-game FOV number for a cl_fovScale. The game's own menu works the same way:
    /// 70 is a scale of 1.0, 110 is 110 / 70.</summary>
    public const double FovPerScale = 70.0;

    public static double ScaleForFov(double fov) => fov / FovPerScale;

    public static string FormatScale(double fov) =>
        ScaleForFov(fov).ToString("0.######", CultureInfo.InvariantCulture);

    public static double? FovFromScale(string? scale) =>
        double.TryParse(scale, NumberStyles.Float, CultureInfo.InvariantCulture, out var s) && s > 0 ? s * FovPerScale : null;

    // ---------------------------------------------------------------- helpers

    private static string? Get(string text, Regex rx)
    {
        foreach (var line in Lines(text))
        {
            var m = rx.Match(line);
            if (m.Success) return m.Groups[4].Value;
        }
        return null;
    }

    private static bool Replace(List<string> lines, Regex rx, string value)
    {
        for (var i = 0; i < lines.Count; i++)
        {
            var m = rx.Match(lines[i]);
            if (!m.Success) continue;
            lines[i] = m.Groups[1].Value + m.Groups[2].Value + m.Groups[3].Value + value + m.Groups[5].Value;
            return true;
        }
        return false;
    }

    private static IEnumerable<string> Lines(string text) => text.Replace("\r\n", "\n").Split('\n');
}
