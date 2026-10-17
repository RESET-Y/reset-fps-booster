namespace ResetFpsBooster.Core.Sensitivity;

public enum SensGame
{
    ApexLegends,
    CounterStrike2,
    Valorant,
    Fortnite,
    ArcRaiders
}

/// <summary>
/// THE CONVERSION BETWEEN A GAME'S SENSITIVITY SETTING AND THE REAL DISTANCE
/// THE MOUSE TRAVELS FOR A FULL TURN (cm per 360°).
///
/// Every shooter turns the view by (counts) x (sensitivity) x (yaw), where a
/// count is one step the mouse reports and "yaw" is the game's fixed degrees
/// per count at sensitivity 1. With the mouse's DPI (counts per inch) that is
///
///     cm/360 = 360 x 2.54 / (yaw x sensitivity x DPI)
///
/// The yaw values (checked against community converters, see below):
///   - Apex Legends 0.022: the same as Counter-Strike, so a CS sensitivity
///     carries over 1:1 at the same DPI. That holds for the ordinary view; the
///     aim-down-sights multiplier is a separate setting and not covered here.
///   - Counter-Strike 2 0.022.
///   - Valorant 0.07.
///   - Fortnite 0.005555 degrees per count for each 1 % of the X-axis
///     sensitivity. Sources differ by under 1 % on this one; treat a Fortnite
///     result as accurate to about a percent.
/// Nothing here reads a game: the factors are published constants.
/// </summary>
public static class SensMath
{
    private const double CmPerInch = 2.54;

    /// <summary>Degrees of turn per mouse count, at a sensitivity of 1 (Fortnite: at 1 %).</summary>
    public static double YawPerCount(SensGame game) => game switch
    {
        SensGame.ApexLegends => 0.022,
        SensGame.CounterStrike2 => 0.022,
        SensGame.Valorant => 0.07,
        SensGame.Fortnite => 0.005555,
        SensGame.ArcRaiders => throw new InvalidOperationException(
            "ARC Raiders has no published yaw: it comes from the player's own calibration (Sens-Finder)."),
        _ => 0.022
    };

    public static string Name(SensGame game) => game switch
    {
        SensGame.ApexLegends => "Apex Legends",
        SensGame.CounterStrike2 => "Counter-Strike 2",
        SensGame.Valorant => "Valorant",
        SensGame.Fortnite => "Fortnite",
        SensGame.ArcRaiders => "ARC Raiders",
        _ => game.ToString()
    };

    /// <summary>Decimals the game's settings screen takes, for showing a result the player can type in.</summary>
    public static int Decimals(SensGame game) => game switch
    {
        SensGame.ApexLegends => 2,
        SensGame.CounterStrike2 => 3,
        SensGame.Valorant => 3,
        SensGame.Fortnite => 1,
        SensGame.ArcRaiders => 1,
        _ => 2
    };

    /// <summary>"%" for Fortnite, nothing for the others.</summary>
    public static string Unit(SensGame game) => game == SensGame.Fortnite ? " %" : "";

    /// <summary>Centimetres of mouse travel for a full 360° turn.</summary>
    public static double Cm360(SensGame game, double dpi, double sensitivity) =>
        Cm360(YawPerCount(game), dpi, sensitivity);

    /// <summary>The same for a yaw that is not published: the player's own calibration.</summary>
    public static double Cm360(double yaw, double dpi, double sensitivity) =>
        360.0 * CmPerInch / (yaw * sensitivity * dpi);

    /// <summary>The in-game sensitivity that gives this cm/360.</summary>
    public static double SensitivityFor(SensGame game, double dpi, double cm360) =>
        SensitivityFor(YawPerCount(game), dpi, cm360);

    public static double SensitivityFor(double yaw, double dpi, double cm360) =>
        360.0 * CmPerInch / (yaw * dpi * cm360);

    /// <summary>Whether the game's yaw is published. ARC Raiders' is not: each player measures it once.</summary>
    public static bool HasPublishedYaw(SensGame game) => game != SensGame.ArcRaiders;

    /// <summary>The yaw a measured full turn implies: degrees per mouse count at sensitivity 1. The turn is
    /// made with the DPI and sensitivity given here; the yaw itself does not depend on them.</summary>
    public static double YawFromCm360(double dpi, double sensitivity, double cm360) =>
        360.0 * CmPerInch / (dpi * sensitivity * cm360);

    /// <summary>Degrees the view turns for one mouse count at this cm/360 and DPI.</summary>
    public static double DegreesPerCount(double dpi, double cm360) =>
        360.0 / (dpi * cm360 / CmPerInch);

    /// <summary>The sensitivity as the player types it: rounded to what the game accepts, no trailing noise.</summary>
    public static string Format(SensGame game, double sensitivity) =>
        sensitivity.ToString("0." + new string('0', Decimals(game)), System.Globalization.CultureInfo.InvariantCulture) + Unit(game);
}
