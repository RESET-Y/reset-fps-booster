namespace ResetFpsBooster.Services;

public interface IAuthService
{
    /// The signed-in user's e-mail, or null when signed out.
    string? CurrentEmail { get; }

    /// The signed-in user's id, or null. Passed to the checkout so the payment
    /// webhook can tell whose purchase it is.
    string? CurrentUserId { get; }
    bool IsSignedIn { get; }

    /// Raised whenever the sign-in state changes, so views can follow it.
    event EventHandler? SignInStateChanged;

    /// Restores a stored session at start-up, refreshing it if needed. Quiet on
    /// failure: an expired or revoked session simply means signed out.
    Task RestoreAsync(CancellationToken ct = default);

    /// Returns null on success, or a message fit to show the user.
    Task<string?> SignInAsync(string email, string password, CancellationToken ct = default);

    /// Returns null on success, or a message fit to show the user. Supabase may
    /// require the address to be confirmed before the first sign-in; the message
    /// says so when it does.
    Task<string?> SignUpAsync(string email, string password, CancellationToken ct = default);

    Task SignOutAsync(CancellationToken ct = default);

    /// Asks the server - not a local flag - whether this user holds premium.
    /// False when signed out, offline, or on any error: premium is never
    /// assumed.
    Task<bool> IsPremiumAsync(CancellationToken ct = default);
}
