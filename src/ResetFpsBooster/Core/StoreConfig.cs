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
    // Live-mode links. Test links look like buy.stripe.com/test_... and only
    // accept Stripe's test cards - never ship one of those here.
    public const string MonthlyPaymentLink = "https://buy.stripe.com/aFa7sL3O0cn96DX4MA8og00";
    public const string LifetimePaymentLink = "https://buy.stripe.com/6oUdR95W872P2nHena8og01";

    /// The master switch for buying in the app. With this false the Account
    /// page says purchasing is not open yet, and Premium comes only from codes.
    public const bool StoreLive = true;

    public static bool IsConfigured =>
        StoreLive
        && !string.IsNullOrWhiteSpace(MonthlyPaymentLink)
        && !string.IsNullOrWhiteSpace(LifetimePaymentLink);
}
