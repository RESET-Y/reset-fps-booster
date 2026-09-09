namespace ResetFpsBooster.Core.Utilities;

/// <summary>Single source of truth for the app's own version, so the About page and the
/// update-checker never drift apart from each other or from the .csproj/installer version.</summary>
public static class AppVersionInfo
{
    public const string Current = "1.2.3";
}
