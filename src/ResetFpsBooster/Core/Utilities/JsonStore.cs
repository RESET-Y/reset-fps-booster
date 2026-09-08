using System.IO;
using System.Text.Json;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>Small helper for reading/writing simple JSON-backed state files used across the app.</summary>
public static class JsonStore
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true
    };

    public static T Load<T>(string path, Func<T> defaultFactory)
    {
        try
        {
            if (!File.Exists(path)) return defaultFactory();
            var json = File.ReadAllText(path);
            var value = JsonSerializer.Deserialize<T>(json, Options);
            return value ?? defaultFactory();
        }
        catch
        {
            return defaultFactory();
        }
    }

    public static void Save<T>(string path, T value)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
            Directory.CreateDirectory(directory);

        var json = JsonSerializer.Serialize(value, Options);
        File.WriteAllText(path, json);
    }
}
