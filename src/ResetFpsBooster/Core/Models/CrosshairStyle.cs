namespace ResetFpsBooster.Core.Models;

public enum CrosshairShape
{
    Cross,
    CrossDot,
    Dot,
    Circle,
    CircleDot,
    TShape,
    Image
}

/// <summary>
/// How the crosshair overlay looks. Every length is in PHYSICAL screen pixels,
/// not WPF units: a 2 px line has to be exactly two pixels on a 125 % display
/// too, or it lands between pixels and turns into a blurred 3 px line.
/// </summary>
public sealed class CrosshairStyle
{
    public CrosshairShape Shape { get; set; } = CrosshairShape.Cross;

    /// <summary>#RRGGBB. Bright green is the classic: it stands out on almost
    /// every map.</summary>
    public string Color { get; set; } = "#00FF66";

    /// <summary>Arm length; for the circle shapes, the radius.</summary>
    public int Length { get; set; } = 8;

    /// <summary>Line thickness; also the size of the centre dot.</summary>
    public int Thickness { get; set; } = 2;

    /// <summary>Empty space between the centre and where the arms start.</summary>
    public int Gap { get; set; } = 4;

    public bool Outline { get; set; } = true;
    public int OutlineThickness { get; set; } = 1;

    /// <summary>10..100 percent.</summary>
    public int Opacity { get; set; } = 100;

    /// <summary>A PNG picked by the user, shown centred when Shape is Image.</summary>
    public string? ImagePath { get; set; }

    /// <summary>10..300 percent of the image's own pixel size.</summary>
    public int ImageScale { get; set; } = 100;

    /// <summary>0 = the primary monitor, then the others in Windows' order.</summary>
    public int Screen { get; set; }
}
