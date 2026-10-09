using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>
/// Shared motion for the app. Everything moves by transform and opacity only:
/// those run on the render thread and cost the GPU next to nothing, which
/// matters because RFB is often open next to a game. Nothing here keeps a
/// timer; the animations end by themselves, or (the ambient ones) belong to a
/// window that WPF stops drawing when it is hidden or minimized.
/// </summary>
public static class Motion
{
    /// <summary>False when Windows has its animation effects switched off
    /// (Settings > Accessibility > Visual effects). The code-driven motion
    /// then does nothing and every screen is simply there.</summary>
    public static bool Enabled => SystemParameters.ClientAreaAnimation;

    private static readonly IEasingFunction OutExpo = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 6 };

    /// <summary>
    /// Every card on the page rises into place one after the other, top to
    /// bottom and left to right, so a page "builds" instead of appearing.
    /// Capped so long lists do not spend a second on their tail.
    /// </summary>
    public static void EnterCards(FrameworkElement root, int max = 14)
    {
        if (!Enabled || root is null || !root.IsLoaded) return;

        var cardStyle = Application.Current.TryFindResource("Card") as Style;
        if (cardStyle is null) return;

        var cards = new List<(Border Card, Point At)>();
        Collect(root, root, cardStyle, cards);
        cards.Sort((a, b) =>
        {
            var rowA = (int)(a.At.Y / 24);
            var rowB = (int)(b.At.Y / 24);
            return rowA != rowB ? rowA.CompareTo(rowB) : a.At.X.CompareTo(b.At.X);
        });

        for (var i = 0; i < cards.Count && i < max; i++)
            RiseIn(cards[i].Card, TimeSpan.FromMilliseconds(45 * i));
    }

    private static void Collect(DependencyObject node, FrameworkElement root, Style cardStyle, List<(Border, Point)> found)
    {
        var count = VisualTreeHelper.GetChildrenCount(node);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(node, i);
            if (child is Border b && b.IsVisible && b.ActualHeight > 8 && IsCard(b, cardStyle))
            {
                try { found.Add((b, b.TransformToAncestor(root).Transform(new Point(0, 0)))); }
                catch (InvalidOperationException) { /* not connected to the tree yet */ }
                continue;   // a card inside a card moves with its parent
            }
            Collect(child, root, cardStyle, found);
        }
    }

    private static bool IsCard(Border border, Style cardStyle)
    {
        for (var style = border.Style; style is not null; style = style.BasedOn)
            if (ReferenceEquals(style, cardStyle)) return true;
        return false;
    }

    // The card is never invisible: it starts a little low and a little small, fully
    // opaque, and slides into place. (Fading cards in from nothing made the whole page
    // flash empty for a moment when switching tabs.)
    private static void RiseIn(Border card, TimeSpan delay)
    {
        var scale = new ScaleTransform(0.97, 0.97);
        var shift = new TranslateTransform(0, 26);
        card.RenderTransformOrigin = new Point(0.5, 0.5);
        card.RenderTransform = new TransformGroup { Children = { scale, shift } };

        var rise = new DoubleAnimation(26, 0, TimeSpan.FromMilliseconds(420)) { BeginTime = delay, EasingFunction = OutExpo };
        var grow = new DoubleAnimation(0.97, 1, TimeSpan.FromMilliseconds(420)) { BeginTime = delay, EasingFunction = OutExpo };
        rise.Completed += (_, _) =>
        {
            // Hand the transform back: no animation clocks left on a finished card.
            card.RenderTransform = Transform.Identity;
        };

        shift.BeginAnimation(TranslateTransform.YProperty, rise);
        scale.BeginAnimation(ScaleTransform.ScaleXProperty, grow);
        scale.BeginAnimation(ScaleTransform.ScaleYProperty, grow);
    }
}
