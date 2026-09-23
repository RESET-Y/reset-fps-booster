using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class AccountViewModel : ViewModelBase
{
    private readonly IAuthService _auth;

    [ObservableProperty] private string _email = "";
    [ObservableProperty] private string? _message;
    [ObservableProperty] private bool _messageIsError;
    [ObservableProperty] private bool _isBusy;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanUpgrade))]
    private bool _isPremium;

    /// Offered only to someone signed in without premium: buying needs an
    /// account to attach the purchase to, and a premium user has nothing to buy.
    public bool CanUpgrade => _auth.IsSignedIn && !IsPremium;
    public bool StoreOpen => StoreConfig.IsConfigured;

    /// The password never lives in a bound property. It is handed over from the
    /// PasswordBox at the moment of the call and dropped straight after, so it
    /// is not sitting in memory for the lifetime of the page.
    public Func<string>? ReadPassword { get; set; }

    public bool IsSignedIn => _auth.IsSignedIn;
    public bool IsSignedOut => !_auth.IsSignedIn;
    public string? SignedInEmail => _auth.CurrentEmail;
    public bool IsConfigured => SupabaseConfig.IsConfigured;

    public AccountViewModel(IAuthService auth)
    {
        _auth = auth;
        _auth.SignInStateChanged += (_, _) => OnSignInStateChanged();
        _ = RefreshPremiumAsync();
    }

    private void OnSignInStateChanged()
    {
        OnPropertyChanged(nameof(IsSignedIn));
        OnPropertyChanged(nameof(IsSignedOut));
        OnPropertyChanged(nameof(SignedInEmail));
        OnPropertyChanged(nameof(CanUpgrade));
        _ = RefreshPremiumAsync();
    }

    private async Task RefreshPremiumAsync() => IsPremium = await _auth.IsPremiumAsync();

    [RelayCommand]
    private async Task SignInAsync() => await RunAsync(pw => _auth.SignInAsync(Email.Trim(), pw), success: null);

    [RelayCommand]
    private async Task SignUpAsync() => await RunAsync(pw => _auth.SignUpAsync(Email.Trim(), pw), success: Loc.T("Account.Created"));

    [RelayCommand] private void BuyMonthly() => OpenCheckout(StoreConfig.MonthlyPaymentLink);
    [RelayCommand] private void BuyLifetime() => OpenCheckout(StoreConfig.LifetimePaymentLink);

    /// Opens the Stripe Payment Link in the browser with the user's id as
    /// client_reference_id - the one thing that lets the webhook attach the
    /// purchase to this account - and the e-mail prefilled so nobody pays
    /// under a different address by mistake.
    private void OpenCheckout(string link)
    {
        if (string.IsNullOrWhiteSpace(link) || _auth.CurrentUserId is not { } uid)
        {
            ShowError(Loc.T("Account.StoreSoon"));
            return;
        }
        var url = $"{link}?client_reference_id={Uri.EscapeDataString(uid)}";
        if (_auth.CurrentEmail is { } email)
            url += $"&prefilled_email={Uri.EscapeDataString(email)}";
        try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
        catch (Exception ex) { ShowError(ex.Message); }
    }

    /// After paying in the browser, the webhook grants premium within seconds.
    /// Asking the server again is all it takes - nothing is set locally.
    [RelayCommand]
    private async Task CheckAgainAsync()
    {
        await RefreshPremiumAsync();
        if (IsPremium) Message = null;
        else { MessageIsError = false; Message = Loc.T("Account.StillFree"); }
    }

    [RelayCommand]
    private async Task SignOutAsync()
    {
        await _auth.SignOutAsync();
        Message = null;
    }

    private async Task RunAsync(Func<string, Task<string?>> action, string? success)
    {
        var password = ReadPassword?.Invoke() ?? "";
        if (string.IsNullOrWhiteSpace(Email) || password.Length == 0)
        {
            ShowError(Loc.T("Account.EnterBoth"));
            return;
        }

        IsBusy = true;
        try
        {
            var error = await action(password);
            if (error is null)
            {
                MessageIsError = false;
                Message = success;
            }
            // Compared against the translated text, not an English prefix: in
            // German or Russian an English check would never match and a
            // successful sign-up would be shown as an error.
            else if (error == Loc.T("Auth.ConfirmPending"))
            {
                // Not a failure: sign-up worked, confirmation is pending.
                MessageIsError = false;
                Message = error;
            }
            else ShowError(error);
        }
        finally { IsBusy = false; }
    }

    private void ShowError(string text)
    {
        MessageIsError = true;
        Message = text;
    }
}
