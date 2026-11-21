namespace ResetFpsBooster.Core.Picture;

public enum PictureAction { None, Apply, Restore }

/// <summary>
/// WHEN THE PICTURE IS ON. The player's picture is shown unless auto-off pauses it while a game runs.
/// Decided from the state alone, so it can be tested without a screen.
/// </summary>
public static class PictureRules
{
    /// <param name="autoOff">Pause the picture while a game runs.</param>
    /// <param name="gameRunning">A game that is not on the ignore list is running.</param>
    /// <param name="pictureWanted">The player's picture is not the neutral one.</param>
    /// <param name="applied">The app has changed the screens and not put them back yet.</param>
    public static PictureAction Decide(bool autoOff, bool gameRunning, bool pictureWanted, bool applied)
    {
        var shouldShow = pictureWanted && !(autoOff && gameRunning);
        if (shouldShow && !applied) return PictureAction.Apply;
        if (!shouldShow && applied) return PictureAction.Restore;
        return PictureAction.None;
    }
}
