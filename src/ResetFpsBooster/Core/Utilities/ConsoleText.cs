using System.Runtime.InteropServices;
using System.Text;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// THE ENCODING WINDOWS CONSOLE PROGRAMS WRITE IN when the app reads their output: the OEM code page
/// (850 on German Windows), not the ANSI one .NET reads with by default (1252). Read with the wrong one,
/// powercfg's "Höchstleistung" arrives as "H”chstleistung". Measured for powercfg, schtasks and PowerShell,
/// each started hidden with redirected output, the way the app starts them.
/// </summary>
public static class ConsoleText
{
    private static readonly Lazy<Encoding> OemEncoding = new(() =>
    {
        var codePage = (int)GetOEMCP();
        // Windows' "UTF-8 for worldwide language support" option makes the OEM code page UTF-8
        if (codePage == 65001) return new UTF8Encoding(false);
        try
        {
            // .NET only carries the code pages beyond UTF and Latin-1 once this provider is registered
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            return Encoding.GetEncoding(codePage);
        }
        catch
        {
            return new UTF8Encoding(false);
        }
    });

    /// <summary>Set as StandardOutputEncoding / StandardErrorEncoding for console programs.</summary>
    public static Encoding Oem => OemEncoding.Value;

    [DllImport("kernel32.dll")]
    private static extern uint GetOEMCP();
}
