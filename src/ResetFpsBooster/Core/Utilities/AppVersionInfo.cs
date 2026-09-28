using System.Reflection;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>The app's own version, read from the built exe - the same &lt;Version&gt; in the
/// .csproj that names the release and the installer.
///
/// This used to be a hand-typed constant. It said 1.2.5 through the 1.3.0 and 1.3.1
/// releases because nobody bumps a second copy of a number, so every build thought
/// the latest release was newer than itself and the update pop-up came on every start.
/// Read from the assembly, there is only one number left to change.</summary>
public static class AppVersionInfo
{
    public static string Current { get; } = Read();

    private static string Read()
    {
        var assembly = typeof(AppVersionInfo).Assembly;
        var informational = assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        if (!string.IsNullOrWhiteSpace(informational))
            return informational.Split('+')[0];   // "1.3.2+<commit>" -> "1.3.2"

        var v = assembly.GetName().Version;
        return v is null ? "0.0.0" : $"{v.Major}.{v.Minor}.{v.Build}";
    }
}
