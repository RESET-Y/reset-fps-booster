// THE RESET ACCOUNT ON THE WEBSITE - the same Supabase account as in the app.
//
// BUYING NEEDS AN ACCOUNT. The buy buttons lead through konto.html: sign in or
// create an account there, and checkout opens with client_reference_id - the
// same as in the app - so Premium lands on the account by itself and there is
// no code to keep. A plan picked while signed out is remembered as "pending"
// and checkout continues right after signing in, even across the e-mail
// confirmation of a new account (same browser).
//
// Codes still exist as a safety net: whoever reaches a bare Payment Link while
// signed out gets one, it is remembered in this browser, and it is listed
// again for any account with the buyer's e-mail. Everything that matters is
// checked by the database; this file only holds the session and makes calls.
//
// A streamer's link (/r/CODE, which Netlify turns into /?ref=CODE) is kept
// for 30 days and prefilled into Stripe's checkout as the promotion code, so
// the discount applies and the sale is counted for that streamer.
window.RFB = (function () {
  const SUPABASE = "https://sjwparnlbtuiyqmagyfg.supabase.co";
  const ANON = "sb_publishable_EP3WkfFmKLBznsC7kRncfA_bIrELJ8g";
  const KEY = "rfb-session", CODES = "rfb-codes", REF = "rfb-ref", PENDING = "rfb-buy";
  const REF_DAYS = 30;
  // Where the confirmation e-mail of a new account leads back to. Must be in
  // Supabase -> Authentication -> URL Configuration -> Redirect URLs.
  const CONFIRMED = location.origin + "/konto.html";
  const LINKS = {
    monthly: "https://buy.stripe.com/aFa7sL3O0cn96DX4MA8og00",
    lifetime: "https://buy.stripe.com/6oUdR95W872P2nHena8og01"
  };

  const read = k => { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } };
  const write = (k, v) => { try { v == null ? localStorage.removeItem(k) : localStorage.setItem(k, JSON.stringify(v)); } catch {} };
  let session = read(KEY);

  // ?ref=CODE on any page: remember it (a newer link replaces an older one).
  (function () {
    const r = (new URLSearchParams(location.search).get("ref") || "").trim().toUpperCase();
    if (/^[A-Z0-9]{3,20}$/.test(r)) write(REF, { code: r, until: Date.now() + REF_DAYS * 864e5 });
  })();
  const ref = () => { const r = read(REF); if (r && r.until > Date.now()) return r.code; if (r) write(REF, null); return null; };

  async function auth(path, body) {
    const r = await fetch(SUPABASE + "/auth/v1/" + path, {
      method: "POST", headers: { apikey: ANON, "Content-Type": "application/json" }, body: JSON.stringify(body) });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) {
      const msg = String(j.error_description || j.msg || j.error || "");
      const e = new Error(msg);
      e.kind = /not confirmed/i.test(msg) ? "unconfirmed" : /invalid/i.test(msg) ? "invalid" : "failed";
      e.status = r.status;
      throw e;
    }
    return { access: j.access_token, refresh: j.refresh_token, expires: Date.now() + (j.expires_in - 60) * 1000,
             id: j.user && j.user.id, email: j.user && j.user.email };
  }
  function save(s) { session = s; write(KEY, s); }

  async function token() {
    if (!session) throw Object.assign(new Error("signed out"), { kind: "signedout" });
    if (Date.now() > session.expires) {
      // Only a clear "no" from Supabase (400/401: revoked, expired, already
      // used) signs out. A network hiccup keeps the session for the next try.
      try { const s = await auth("token?grant_type=refresh_token", { refresh_token: session.refresh }); save({ ...s, id: s.id || session.id, email: s.email || session.email }); }
      catch (e) {
        if (e && (e.status === 400 || e.status === 401)) { save(null); throw Object.assign(new Error("expired"), { kind: "signedout" }); }
        throw Object.assign(new Error("offline"), { kind: "failed" });
      }
    }
    return session.access;
  }

  async function rpc(fn, args) {
    const r = await fetch(SUPABASE + "/rest/v1/rpc/" + fn, {
      method: "POST",
      headers: { apikey: ANON, Authorization: "Bearer " + await token(), "Content-Type": "application/json" },
      body: JSON.stringify(args || {}) });
    const text = await r.text();
    if (r.status === 401) { save(null); throw Object.assign(new Error("signed out"), { kind: "signedout" }); }
    if (!r.ok) throw new Error(text);
    return text ? JSON.parse(text) : null;
  }

  return {
    get user() { return session ? { id: session.id, email: session.email } : null; },
    async signIn(email, password) { save(await auth("token?grant_type=password", { email, password })); },
    // A new account. With e-mail confirmation on (it is), Supabase answers
    // with the user but no session: "check your mail". The link in that mail
    // comes back to konto.html with the session in the URL - see fromLink().
    async signUp(email, password) {
      const r = await fetch(SUPABASE + "/auth/v1/signup?redirect_to=" + encodeURIComponent(CONFIRMED), {
        method: "POST", headers: { apikey: ANON, "Content-Type": "application/json" }, body: JSON.stringify({ email, password }) });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) {
        const msg = String(j.error_description || j.msg || j.error || "");
        const e = new Error(msg);
        e.kind = /already|registered|exists/i.test(msg) ? "exists" : /password/i.test(msg) ? "weak" : "failed";
        throw e;
      }
      if (j.access_token) { save({ access: j.access_token, refresh: j.refresh_token, expires: Date.now() + (j.expires_in - 60) * 1000, id: j.user && j.user.id, email: j.user && j.user.email }); return "signedin"; }
      // An address that is already registered answers like a new one, with an
      // empty identity list, so nobody can probe which e-mails have accounts.
      return "confirm";
    },
    // Back from the confirmation e-mail: #access_token=...&refresh_token=...
    // (or #error=... when the link has expired). Takes the session and cleans
    // the address bar. Returns "signedin", an error text, or null.
    async fromLink() {
      const h = new URLSearchParams(location.hash.slice(1));
      if (!h.get("access_token") && !h.get("error")) return null;
      history.replaceState(null, "", location.pathname + location.search);
      if (h.get("error")) return h.get("error_code") || h.get("error") || "failed";
      const access = h.get("access_token");
      const r = await fetch(SUPABASE + "/auth/v1/user", { headers: { apikey: ANON, Authorization: "Bearer " + access } });
      const u = await r.json().catch(() => ({}));
      if (!r.ok || !u.id) return "failed";
      save({ access, refresh: h.get("refresh_token"), expires: Date.now() + ((+h.get("expires_in") || 3600) - 60) * 1000, id: u.id, email: u.email });
      return "signedin";
    },
    // The plan picked before signing in, kept for a day.
    get pending() { const p = read(PENDING); if (p && p.until > Date.now() && LINKS[p.plan]) return p.plan; if (p) write(PENDING, null); return null; },
    setPending(plan) { write(PENDING, LINKS[plan] ? { plan, until: Date.now() + 864e5 } : null); },
    signOut() {
      const s = session; save(null);
      if (s) fetch(SUPABASE + "/auth/v1/logout", { method: "POST", headers: { apikey: ANON, Authorization: "Bearer " + s.access } }).catch(() => {});
    },
    rpc,
    // Public database functions need no session, only the public key.
    async publicRpc(fn, args) {
      const r = await fetch(SUPABASE + "/rest/v1/rpc/" + fn, {
        method: "POST", headers: { apikey: ANON, "Content-Type": "application/json" }, body: JSON.stringify(args || {}) });
      if (!r.ok) throw new Error(await r.text());
      const text = await r.text();
      return text ? JSON.parse(text) : null;
    },
    get referral() { return ref(); },
    dropReferral() { write(REF, null); },
    // The Payment Link for a plan - personal when signed in, so the purchase
    // goes to this account instead of becoming a code.
    buyUrl(plan) {
      const q = [];
      if (session && session.id) {
        q.push("client_reference_id=" + encodeURIComponent(session.id));
        if (session.email) q.push("prefilled_email=" + encodeURIComponent(session.email));
      }
      const r = ref();
      if (r) q.push("prefilled_promo_code=" + encodeURIComponent(r));
      return LINKS[plan] + (q.length ? "?" + q.join("&") : "");
    },
    // What a buy button points to: checkout when signed in, otherwise the
    // account page, which signs in first and then continues to checkout.
    checkoutUrl(plan) {
      return session && session.id ? this.buyUrl(plan) : "/konto.html?buy=" + encodeURIComponent(plan);
    },
    localCodes() { return read(CODES) || []; },
    rememberCode(code, plan) {
      const list = read(CODES) || [];
      if (!list.some(c => c.code === code)) { list.unshift({ code, plan, at: new Date().toISOString() }); write(CODES, list.slice(0, 20)); }
    },
    forgetCode(code) { write(CODES, (read(CODES) || []).filter(c => c.code !== code)); }
  };
})();
