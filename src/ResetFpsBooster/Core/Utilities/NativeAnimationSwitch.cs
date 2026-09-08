namespace ResetFpsBooster.Core.Utilities;

internal static class NativeAnimationSwitch
{
    public enum Effect
    {
        MenuAnimation,
        ComboBoxAnimation,
        TooltipAnimation,
        ListBoxSmoothScrolling
    }

    public static void Disable(Effect effect)
    {
        var action = effect switch
        {
            Effect.MenuAnimation => NativeMethods.SPI_SETMENUANIMATION,
            Effect.ComboBoxAnimation => NativeMethods.SPI_SETCOMBOBOXANIMATION,
            Effect.TooltipAnimation => NativeMethods.SPI_SETTOOLTIPANIMATION,
            Effect.ListBoxSmoothScrolling => NativeMethods.SPI_SETLISTBOXSMOOTHSCROLLING,
            _ => throw new ArgumentOutOfRangeException(nameof(effect))
        };

        NativeMethods.SystemParametersInfo(action, 0, IntPtr.Zero, NativeMethods.SPIF_SENDCHANGE);
    }
}
