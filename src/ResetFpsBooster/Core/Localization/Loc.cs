using System.Globalization;
using System.Windows;

namespace ResetFpsBooster.Core.Localization;

/// THE APP'S LANGUAGE, switchable while it runs.
///
/// Every user-facing text lives in Languages/Strings.{code}.xaml as a named
/// string. XAML reads them with {DynamicResource Key}, so swapping the
/// dictionary here repaints every open page at once - no restart. Code reads
/// them with Loc.T("Key"), and anything that builds text in code listens to
/// LanguageChanged to rebuild it.
///
/// Those three files are GENERATED from Languages/strings.json by
/// Languages/generate.js - edit the JSON, never the XAML, or a language will
/// silently lose a text the others have.
public static class Loc
{
    public sealed record Language(string Code, string NativeName);

    /// Offered in the Settings dropdown, each in its own language so a user
    /// who cannot read the current one can still find theirs.
    public static IReadOnlyList<Language> Available { get; } = new[]
    {
        new Language("en", "English"),
        new Language("de", "Deutsch"),
        new Language("ru", "Русский"),
    };

    public static string Current { get; private set; } = "en";

    public static event EventHandler? LanguageChanged;

    private static ResourceDictionary? _active;

    /// Windows' display language when we offer it, English otherwise.
    public static string DefaultCode()
    {
        var two = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
        return Available.Any(l => l.Code == two) ? two : "en";
    }

    public static void Apply(string? code)
    {
        if (string.IsNullOrWhiteSpace(code) || Available.All(l => l.Code != code))
            code = DefaultCode();

        var dict = new ResourceDictionary
        {
            Source = new Uri($"pack://application:,,,/Languages/Strings.{code}.xaml", UriKind.Absolute)
        };

        var merged = Application.Current.Resources.MergedDictionaries;
        if (_active is not null) merged.Remove(_active);
        merged.Add(dict);
        _active = dict;

        Current = code;
        CultureInfo.CurrentUICulture = new CultureInfo(code);
        LanguageChanged?.Invoke(null, EventArgs.Empty);
    }

    /// The text for a key in the current language. A missing key shows the key
    /// itself - visibly wrong, instead of an empty space nobody notices.
    public static string T(string key) =>
        Application.Current?.TryFindResource(key) as string ?? key;

    /// THE ENGLISH TEXT, TRANSLATED IF WE HAVE IT.
    ///
    /// For texts that are built in English by code we keep in English on
    /// purpose - the optimization modules, whose names and results also land
    /// in the change log and backups, where one language keeps the history
    /// readable. They are translated only where they are shown: the English
    /// sentence is looked up among the English strings, and its key gives the
    /// current language. Anything not found is shown as it came.
    public static string Tr(string? english)
    {
        if (string.IsNullOrEmpty(english) || Current == "en") return english ?? string.Empty;
        const string currently = "Currently: ";
        if (english.StartsWith(currently, StringComparison.Ordinal))
            return T("Mod.Currently") + Tr(english[currently.Length..]);
        return EnglishKeys().TryGetValue(english, out var key) ? T(key) : english;
    }

    private static Dictionary<string, string>? _englishKeys;

    private static Dictionary<string, string> EnglishKeys()
    {
        if (_englishKeys is not null) return _englishKeys;
        var en = new ResourceDictionary { Source = new Uri("pack://application:,,,/Languages/Strings.en.xaml", UriKind.Absolute) };
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var key in en.Keys)
            if (key is string k && en[k] is string text && k.StartsWith("Mod.", StringComparison.Ordinal))
                map.TryAdd(text, k);
        return _englishKeys = map;
    }

    /// T with string.Format arguments, for texts that carry numbers.
    public static string F(string key, params object[] args) =>
        string.Format(CultureInfo.CurrentCulture, T(key), args);
}
