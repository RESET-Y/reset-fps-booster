namespace ResetFpsBooster.Core.Models;

/// <summary>How one game is set up on the player's side, as far as the aim test needs it.</summary>
public sealed class SensGameProfile
{
    /// <summary>The sensitivity in that game's own scale.</summary>
    public double Sens { get; set; }
    /// <summary>Horizontal field of view in degrees.</summary>
    public double Fov { get; set; }
    /// <summary>The game's resolution in pixels.</summary>
    public int ResWidth { get; set; }
    public int ResHeight { get; set; }
    /// <summary>The picture is stretched to fill the whole monitor (for example 4:3 on a wide screen).</summary>
    public bool Stretch { get; set; }
}
