namespace ResetFpsBooster.Core;

/// WHERE PREMIUM IS BOUGHT: two Stripe Payment Links, one per plan.
///
/// Payments run through Stripe Managed Payments, where Stripe is the seller of
/// record and handles VAT, fraud, disputes and customer support. Payment Links
/// are one of the two integrations it supports, and the simplest: created in
/// the Stripe dashboard, opened in the browser, nothing to host.
///
/// The app appends ?client_reference_id=<user id>. Stripe returns it to the
/// webhook on checkout.session.completed, which is how a purchase finds its
/// account. These URLs are public by nature - anyone can open a Payment Link.
public static class StoreConfig
{
    // TEST MODE links (buy.stripe.com/test_...). Replace with the live links
    // before release - test links only accept Stripe's test cards.
    public const string MonthlyPaymentLink = "https://buy.stripe.com/test_aFafZh0Cg5992rObCc73G00";
    public const string LifetimePaymentLink = "https://buy.stripe.com/test_00w6oH5WAdFFfeAfSs73G01";

    /// OFF UNTIL STRIPE IS LIVE. The links above are test-mode links: they only
    /// accept Stripe's test cards, so a real user clicking Buy would reach a
    /// checkout that can never take their money. With this false the Account
    /// page says purchasing is not open yet, and Premium comes only from codes.
    /// Flip to true together with swapping in the live links.
    public const bool StoreLive = false;

    public static bool IsConfigured =>
        StoreLive
        && !string.IsNullOrWhiteSpace(MonthlyPaymentLink)
        && !string.IsNullOrWhiteSpace(LifetimePaymentLink);
}
