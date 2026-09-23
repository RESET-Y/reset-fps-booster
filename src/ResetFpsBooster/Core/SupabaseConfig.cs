namespace ResetFpsBooster.Core;

/// THE TWO PUBLIC VALUES the app needs to reach the account backend.
///
/// Both are meant to be public. The anon key only grants what the database's
/// row level security policies allow - reading your own profile and your own
/// entitlements, never writing an entitlement. That is enforced in
/// backend/supabase/migrations, not here.
///
/// The service_role key must NEVER be placed in this file or anywhere in the
/// app. It bypasses every policy, and anything shipped in a desktop binary can
/// be read out of it.
public static class SupabaseConfig
{
    public const string Url = "https://sjwparnlbtuiyqmagyfg.supabase.co";
    // The PUBLISHABLE key (sb_publishable_...), Supabase's current name for
    // the public anon key. Settings -> API Keys. Safe to ship; RLS guards it.
    public const string AnonKey = "sb_publishable_EP3WkfFmKLBznsC7kRncfA_bIrELJ8g";

    public static bool IsConfigured =>
        !string.IsNullOrWhiteSpace(Url) && !string.IsNullOrWhiteSpace(AnonKey);
}
