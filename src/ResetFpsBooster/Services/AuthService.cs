using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using ResetFpsBooster.Core;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Core.Localization;

namespace ResetFpsBooster.Services;

/// ACCOUNTS, SPOKEN TO SUPABASE AUTH DIRECTLY OVER HTTPS.
///
/// No client library: the four calls this needs - sign up, sign in, refresh,
/// sign out - plus one database function are plain REST, and owning them means
/// owning exactly what is sent and what is stored.
///
/// The session is kept between runs so nobody signs in every launch. It holds a
/// refresh token, which can sign in as that user, so it is written ONLY through
/// DPAPI for the current Windows user: the file is useless on another machine
/// or under another account, and it is never plain text on disk.
public sealed class AuthService : IAuthService
{
    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(15) };
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };

    private Session? _session;

    public string? CurrentEmail => _session?.User?.Email;
    public string? CurrentUserId => _session?.User?.Id;
    public bool IsSignedIn => _session is not null;
    public event EventHandler? SignInStateChanged;

    public async Task RestoreAsync(CancellationToken ct = default)
    {
        if (!SupabaseConfig.IsConfigured) return;
        var stored = LoadSession();
        if (stored?.RefreshToken is null) return;

        // Always refresh on start: the access token is short-lived and may have
        // expired while the app was closed. A refresh that fails means the
        // session was revoked or expired - signed out, and the stale file goes.
        var refreshed = await RefreshAsync(stored.RefreshToken, ct);
        if (refreshed is null) { DeleteSession(); return; }
        SetSession(refreshed);

        // Premium may have started or ended while the app was closed.
        _ = SyncDiscordRoleAsync(ct);
    }

    public async Task<string?> SignInAsync(string email, string password, CancellationToken ct = default)
    {
        if (!SupabaseConfig.IsConfigured) return Loc.T("Auth.NotSetUp");
        try
        {
            using var res = await PostAuthAsync("token?grant_type=password",
                new { email, password }, bearer: null, ct);
            var body = await res.Content.ReadAsStringAsync(ct);
            if (!res.IsSuccessStatusCode) return DescribeError(body, signingUp: false);

            var session = JsonSerializer.Deserialize<Session>(body, Json);
            if (session?.AccessToken is null) return Loc.T("Auth.Unreadable");
            SetSession(session);
            return null;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return Loc.T("Auth.Offline");
        }
    }

    public async Task<string?> SignUpAsync(string email, string password, CancellationToken ct = default)
    {
        if (!SupabaseConfig.IsConfigured) return Loc.T("Auth.NotSetUp");
        try
        {
            using var res = await PostAuthAsync("signup", new { email, password }, bearer: null, ct);
            var body = await res.Content.ReadAsStringAsync(ct);
            if (!res.IsSuccessStatusCode) return DescribeError(body, signingUp: true);

            // With e-mail confirmation on, sign-up returns a user but no session.
            var session = JsonSerializer.Deserialize<Session>(body, Json);
            if (session?.AccessToken is null)
                return Loc.T("Auth.ConfirmPending");
            SetSession(session);
            return null;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return Loc.T("Auth.Offline");
        }
    }

    public async Task SignOutAsync(CancellationToken ct = default)
    {
        var token = _session?.AccessToken;
        _session = null;
        DeleteSession();
        SignInStateChanged?.Invoke(this, EventArgs.Empty);

        // Tell the server too, so the refresh token stops working everywhere.
        // Local sign-out has already happened; a failure here changes nothing.
        if (token is null || !SupabaseConfig.IsConfigured) return;
        try { using var _ = await PostAuthAsync("logout", new { }, token, ct); } catch { }
    }

    // ---- Discord ------------------------------------------------------------
    //
    // OAuth in the user's own browser, with PKCE: the app makes a secret
    // (verifier), sends only its hash (challenge) along, and later trades the
    // code Supabase hands back for a session by showing the secret. The code
    // alone is worthless to anyone who sees it.
    //
    // The browser returns to a local address this app listens on for as long
    // as the sign-in takes. It must be listed in Supabase under
    // Authentication > URL Configuration > Redirect URLs.

    private const string LoopbackRedirect = "http://127.0.0.1:53682/callback";

    public async Task<string?> SignInWithDiscordAsync(CancellationToken ct = default)
    {
        if (!SupabaseConfig.IsConfigured) return Loc.T("Auth.NotSetUp");
        var (verifier, challenge) = NewPkce();
        var authorizeUrl = $"{SupabaseConfig.Url.TrimEnd('/')}/auth/v1/authorize?provider=discord"
                         + $"&redirect_to={Uri.EscapeDataString(LoopbackRedirect)}"
                         + $"&code_challenge={challenge}&code_challenge_method=s256"
                         + "&scopes=identify%20email";
        var error = await CompleteBrowserFlowAsync(authorizeUrl, verifier, ct);
        if (error is null) await SyncDiscordRoleAsync(ct);
        return error;
    }

    public async Task<string?> LinkDiscordAsync(CancellationToken ct = default)
    {
        if (!SupabaseConfig.IsConfigured) return Loc.T("Auth.NotSetUp");
        if (_session?.RefreshToken is null) return Loc.T("Discord.SignInFirst");

        // Linking needs a live access token; the stored one may be hours old.
        var fresh = await RefreshAsync(_session.RefreshToken, ct);
        if (fresh is null) return Loc.T("Discord.SignInFirst");
        SetSession(fresh);

        var (verifier, challenge) = NewPkce();
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get,
                $"{SupabaseConfig.Url.TrimEnd('/')}/auth/v1/user/identities/authorize?provider=discord"
                + $"&redirect_to={Uri.EscapeDataString(LoopbackRedirect)}"
                + $"&code_challenge={challenge}&code_challenge_method=s256"
                + "&scopes=identify%20email&skip_http_redirect=true");
            req.Headers.Add("apikey", SupabaseConfig.AnonKey);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", fresh.AccessToken);
            using var res = await Http.SendAsync(req, ct);
            var body = await res.Content.ReadAsStringAsync(ct);
            if (!res.IsSuccessStatusCode)
                return body.Contains("manual_linking_disabled") || body.Contains("Manual linking")
                    ? Loc.T("Discord.LinkingOff") : Loc.T("Discord.Failed");

            using var doc = JsonDocument.Parse(body);
            var url = doc.RootElement.TryGetProperty("url", out var u) ? u.GetString() : null;
            if (url is null) return Loc.T("Discord.Failed");
            var error = await CompleteBrowserFlowAsync(url, verifier, ct);
            if (error is null) await SyncDiscordRoleAsync(ct);
            return error;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException)
        {
            return Loc.T("Auth.Offline");
        }
    }

    public async Task<string?> LinkedDiscordNameAsync(CancellationToken ct = default)
    {
        if (_session?.AccessToken is null || !SupabaseConfig.IsConfigured) return null;
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, $"{SupabaseConfig.Url.TrimEnd('/')}/auth/v1/user");
            req.Headers.Add("apikey", SupabaseConfig.AnonKey);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _session.AccessToken);
            using var res = await Http.SendAsync(req, ct);
            if (!res.IsSuccessStatusCode) return null;
            using var doc = JsonDocument.Parse(await res.Content.ReadAsStringAsync(ct));
            if (!doc.RootElement.TryGetProperty("identities", out var ids) || ids.ValueKind != JsonValueKind.Array) return null;
            foreach (var id in ids.EnumerateArray())
            {
                if (!id.TryGetProperty("provider", out var p) || p.GetString() != "discord") continue;
                if (id.TryGetProperty("identity_data", out var data))
                    foreach (var key in new[] { "custom_claims.global_name", "full_name", "name", "user_name" })
                    {
                        var node = data;
                        var found = true;
                        foreach (var part in key.Split('.'))
                            if (!node.TryGetProperty(part, out node)) { found = false; break; }
                        if (found && node.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(node.GetString()))
                            return node.GetString();
                    }
                return "Discord";
            }
            return null;
        }
        catch { return null; }
    }

    public async Task SyncDiscordRoleAsync(CancellationToken ct = default)
    {
        if (_session?.AccessToken is null || !SupabaseConfig.IsConfigured) return;
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Post,
                $"{SupabaseConfig.Url.TrimEnd('/')}/functions/v1/discord-sync")
            {
                Content = new StringContent("{}", Encoding.UTF8, "application/json"),
            };
            req.Headers.Add("apikey", SupabaseConfig.AnonKey);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _session.AccessToken);
            using var _ = await Http.SendAsync(req, ct);
        }
        catch { /* the hourly sync catches up */ }
    }

    /// Opens the browser, waits for Supabase to send it back to the local
    /// address with a code, and trades that code for a session.
    private async Task<string?> CompleteBrowserFlowAsync(string url, string verifier, CancellationToken ct)
    {
        using var listener = new System.Net.HttpListener();
        listener.Prefixes.Add("http://127.0.0.1:53682/callback/");
        try { listener.Start(); }
        catch { return Loc.T("Discord.PortBusy"); }

        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true }); }
        catch { return Loc.T("Discord.NoBrowser"); }

        // Five minutes to finish in the browser, then give up quietly.
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromMinutes(5));
        System.Net.HttpListenerContext ctx;
        try { ctx = await listener.GetContextAsync().WaitAsync(timeout.Token); }
        catch (OperationCanceledException) { return Loc.T("Discord.TimedOut"); }

        var query = System.Web.HttpUtility.ParseQueryString(ctx.Request.Url?.Query ?? "");
        var code = query["code"];
        var error = query["error_description"] ?? query["error"];

        // Something the user can read in the tab they are looking at.
        var ok = code is not null && error is null;
        var page = "<!doctype html><meta charset=utf-8><title>RESET FPS BOOSTER</title>"
                 + "<body style=\"background:#050505;color:#fff;font-family:Segoe UI,sans-serif;display:grid;place-items:center;height:100vh;margin:0\">"
                 + "<div style=\"text-align:center\"><div style=\"height:6px;width:320px;margin:0 auto 24px;background:repeating-linear-gradient(-56deg,#e8121f 0 5px,#000 5px 10px)\"></div>"
                 + $"<h1 style=\"font-style:italic;font-weight:900;text-transform:uppercase\">{(ok ? System.Net.WebUtility.HtmlEncode(Loc.T("Discord.BrowserDone")) : System.Net.WebUtility.HtmlEncode(Loc.T("Discord.BrowserFailed")))}</h1>"
                 + $"<p style=\"color:#a8a8a8\">{System.Net.WebUtility.HtmlEncode(Loc.T("Discord.BrowserClose"))}</p></div>";
        var bytes = Encoding.UTF8.GetBytes(page);
        ctx.Response.ContentType = "text/html; charset=utf-8";
        ctx.Response.ContentLength64 = bytes.Length;
        await ctx.Response.OutputStream.WriteAsync(bytes, ct);
        ctx.Response.Close();

        if (!ok) return error ?? Loc.T("Discord.Failed");

        try
        {
            using var res = await PostAuthAsync("token?grant_type=pkce",
                new { auth_code = code, code_verifier = verifier }, bearer: null, ct);
            var body = await res.Content.ReadAsStringAsync(ct);
            if (!res.IsSuccessStatusCode) return DescribeError(body, signingUp: false);
            var session = JsonSerializer.Deserialize<Session>(body, Json);
            if (session?.AccessToken is null) return Loc.T("Auth.Unreadable");
            SetSession(session);
            return null;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return Loc.T("Auth.Offline");
        }
    }

    private static (string Verifier, string Challenge) NewPkce()
    {
        var verifier = Base64Url(RandomNumberGenerator.GetBytes(48));
        var challenge = Base64Url(SHA256.HashData(Encoding.ASCII.GetBytes(verifier)));
        return (verifier, challenge);
    }

    private static string Base64Url(byte[] bytes) =>
        Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    public async Task<bool> IsPremiumAsync(CancellationToken ct = default)
    {
        if (_session?.AccessToken is null || !SupabaseConfig.IsConfigured) return false;
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Post,
                $"{SupabaseConfig.Url.TrimEnd('/')}/rest/v1/rpc/is_premium")
            {
                Content = new StringContent("{}", Encoding.UTF8, "application/json"),
            };
            req.Headers.Add("apikey", SupabaseConfig.AnonKey);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _session.AccessToken);

            using var res = await Http.SendAsync(req, ct);
            if (!res.IsSuccessStatusCode) return false;
            var body = (await res.Content.ReadAsStringAsync(ct)).Trim();
            return body == "true";
        }
        catch { return false; }
    }

    // ---- manager role and codes ----------------------------------------------

    public async Task<bool> IsManagerAsync(CancellationToken ct = default)
    {
        var (ok, body) = await RpcAsync("is_manager", new { }, ct);
        return ok && body.Trim() == "true";
    }

    public async Task<(bool Active, DateTimeOffset? Until)> PremiumStatusAsync(CancellationToken ct = default)
    {
        var (ok, body) = await RpcAsync("premium_status", new { }, ct);
        if (!ok) return (false, null);
        try
        {
            using var doc = JsonDocument.Parse(body);
            var root = doc.RootElement;
            var active = root.TryGetProperty("active", out var a) && a.ValueKind == JsonValueKind.True;
            DateTimeOffset? until = root.TryGetProperty("until", out var u) && u.ValueKind == JsonValueKind.String
                && DateTimeOffset.TryParse(u.GetString(), out var parsed) ? parsed : null;
            return (active, until);
        }
        catch { return (false, null); }
    }

    public async Task<string> RedeemCodeAsync(string code, CancellationToken ct = default)
    {
        var (ok, body) = await RpcAsync("redeem_premium_code", new { p_code = code }, ct);
        if (!ok) return "error";
        _ = SyncDiscordRoleAsync(ct);   // a new premium shows on Discord at once
        try { return JsonSerializer.Deserialize<string>(body) ?? "error"; }
        catch { return "error"; }
    }

    public async Task<string?> CreateCodeAsync(int? durationDays, int maxUses, string? note, CancellationToken ct = default)
    {
        var (ok, body) = await RpcAsync("create_premium_code",
            new { p_duration_days = durationDays, p_max_uses = maxUses, p_note = note }, ct);
        if (!ok) return null;
        try { return JsonSerializer.Deserialize<string>(body); }
        catch { return null; }
    }

    public async Task<IReadOnlyList<ResetFpsBooster.Core.Models.PremiumCode>> ListCodesAsync(CancellationToken ct = default)
    {
        var (ok, body) = await RpcAsync("list_premium_codes", new { }, ct);
        if (!ok) return Array.Empty<ResetFpsBooster.Core.Models.PremiumCode>();
        try
        {
            return JsonSerializer.Deserialize<List<ResetFpsBooster.Core.Models.PremiumCode>>(body, Json)
                   ?? new List<ResetFpsBooster.Core.Models.PremiumCode>();
        }
        catch { return Array.Empty<ResetFpsBooster.Core.Models.PremiumCode>(); }
    }

    public async Task<bool> SetCodeActiveAsync(string code, bool active, CancellationToken ct = default)
    {
        var (ok, _) = await RpcAsync("set_premium_code_active", new { p_code = code, p_active = active }, ct);
        return ok;
    }

    /// One call to a database function as the signed-in user. The token is
    /// what lets the function know WHO is asking - auth.uid() on the server.
    private async Task<(bool Ok, string Body)> RpcAsync(string function, object args, CancellationToken ct)
    {
        if (_session?.AccessToken is null || !SupabaseConfig.IsConfigured) return (false, "");
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Post,
                $"{SupabaseConfig.Url.TrimEnd('/')}/rest/v1/rpc/{function}")
            {
                Content = new StringContent(JsonSerializer.Serialize(args), Encoding.UTF8, "application/json"),
            };
            req.Headers.Add("apikey", SupabaseConfig.AnonKey);
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _session.AccessToken);
            using var res = await Http.SendAsync(req, ct);
            var body = await res.Content.ReadAsStringAsync(ct);
            return (res.IsSuccessStatusCode, body);
        }
        catch { return (false, ""); }
    }

    // ---- internals -----------------------------------------------------------

    private async Task<Session?> RefreshAsync(string refreshToken, CancellationToken ct)
    {
        try
        {
            using var res = await PostAuthAsync("token?grant_type=refresh_token",
                new { refresh_token = refreshToken }, bearer: null, ct);
            if (!res.IsSuccessStatusCode) return null;
            var body = await res.Content.ReadAsStringAsync(ct);
            var session = JsonSerializer.Deserialize<Session>(body, Json);
            return session?.AccessToken is null ? null : session;
        }
        catch { return null; }
    }

    private static Task<HttpResponseMessage> PostAuthAsync(string path, object payload,
                                                           string? bearer, CancellationToken ct)
    {
        var req = new HttpRequestMessage(HttpMethod.Post,
            $"{SupabaseConfig.Url.TrimEnd('/')}/auth/v1/{path}")
        {
            Content = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json"),
        };
        req.Headers.Add("apikey", SupabaseConfig.AnonKey);
        if (bearer is not null) req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", bearer);
        return Http.SendAsync(req, ct);
    }

    private void SetSession(Session session)
    {
        _session = session;
        SaveSession(session);
        SignInStateChanged?.Invoke(this, EventArgs.Empty);
    }

    /// Supabase answers in a few shapes; the user gets a sentence, not a code.
    private static string DescribeError(string body, bool signingUp)
    {
        string raw = "";
        try
        {
            using var doc = JsonDocument.Parse(body);
            foreach (var key in new[] { "error_description", "msg", "message", "error" })
                if (doc.RootElement.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String)
                { raw = v.GetString() ?? ""; break; }
        }
        catch { }

        var lower = raw.ToLowerInvariant();
        if (lower.Contains("invalid login")) return Loc.T("Auth.WrongCredentials");
        if (lower.Contains("not confirmed")) return Loc.T("Auth.NotConfirmed");
        if (lower.Contains("already registered")) return Loc.T("Auth.AlreadyRegistered");
        if (lower.Contains("password")) return string.IsNullOrEmpty(raw) ? Loc.T("Auth.PasswordRejected") : raw;
        if (lower.Contains("rate limit")) return Loc.T("Auth.RateLimited");
        return string.IsNullOrEmpty(raw)
            ? (signingUp ? Loc.T("Auth.SignUpFailed") : Loc.T("Auth.SignInFailed"))
            : raw;
    }

    private static Session? LoadSession()
    {
        try
        {
            if (!File.Exists(AppPaths.SessionFile)) return null;
            var sealedBytes = File.ReadAllBytes(AppPaths.SessionFile);
            var plain = ProtectedData.Unprotect(sealedBytes, null, DataProtectionScope.CurrentUser);
            return JsonSerializer.Deserialize<Session>(plain, Json);
        }
        catch
        {
            // Unreadable - another Windows user, a copied file, corruption.
            // Treated as no session, never as an error the user must deal with.
            DeleteSession();
            return null;
        }
    }

    private static void SaveSession(Session session)
    {
        try
        {
            AppPaths.EnsureFoldersExist();
            var plain = JsonSerializer.SerializeToUtf8Bytes(session, Json);
            var sealedBytes = ProtectedData.Protect(plain, null, DataProtectionScope.CurrentUser);
            File.WriteAllBytes(AppPaths.SessionFile, sealedBytes);
        }
        catch
        {
            // Not being remembered costs one sign-in next launch. Not worth
            // failing the sign-in over.
        }
    }

    private static void DeleteSession()
    {
        try { if (File.Exists(AppPaths.SessionFile)) File.Delete(AppPaths.SessionFile); } catch { }
    }

    private sealed class Session
    {
        [JsonPropertyName("access_token")]  public string? AccessToken { get; set; }
        [JsonPropertyName("refresh_token")] public string? RefreshToken { get; set; }
        [JsonPropertyName("expires_at")]    public long? ExpiresAt { get; set; }
        [JsonPropertyName("user")]          public SessionUser? User { get; set; }
    }

    private sealed class SessionUser
    {
        [JsonPropertyName("id")]    public string? Id { get; set; }
        [JsonPropertyName("email")] public string? Email { get; set; }
    }
}
