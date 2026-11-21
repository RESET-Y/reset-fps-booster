namespace ResetFpsBooster.Core.Models;

/// <summary>
/// A picture setting, as the player sets it. Each value runs from -100 to +100, and 0 means the
/// screen as Windows and the driver have it: nothing is changed for that value.
/// </summary>
public sealed class PicturePreset
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get; set; } = string.Empty;
    public bool IsBuiltIn { get; set; }
    public int Saturation { get; set; }
    public int Brightness { get; set; }
    public int Contrast { get; set; }
}

/// <summary>One display's saturation (NVIDIA Digital Vibrance level) before the app changed it.</summary>
public sealed class PictureDisplay
{
    public string Name { get; set; } = string.Empty;
    public int Level { get; set; }
}

/// <summary>
/// What the screens had before the app changed them. Kept in the settings file, so a crash or a
/// forced close cannot leave a changed picture behind: the next start puts these values back.
/// </summary>
public sealed class PictureOriginals
{
    public List<PictureDisplay> Displays { get; set; } = new();

    /// <summary>The gamma ramp Windows had, 768 values (red, green, blue) as little-endian bytes, base64.</summary>
    public string? GammaRampBase64 { get; set; }
}
