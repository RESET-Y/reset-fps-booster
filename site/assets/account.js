// THE RESET ACCOUNT ON THE WEBSITE - the same Supabase account as in the app.
//
// Signing in here does two things: a purchase lands straight on the account
// (the Payment Link gets client_reference_id, exactly like the app sends it),
// and codes bought earlier with the account's e-mail can be found and
// redeemed again. Everything that matters is checked by the database; this
// file only holds the session and makes the calls.
//
// Codes bought while signed out are also remembered in this browser, so
// closing the thank-you page does not lose them.
//
// A streamer's link (/r/CODE, which Netlify turns into /?ref=CODE) is kept
// for 30 days and prefilled into Stripe's checkout as the promotion code, so
// the discount applies and the sale is counted for that streamer.
window.RFB = (function () {
  const SUPABASE = "https://sjwparnlbtuiyqmagyfg.supabase.co";
  const ANON = "sb_publishable_EP3WkfFmKLBznsC7kRncfA_bIrELJ8g";
  const KEY = "rfb-session", CODES = "rfb-codes", REF = "rfb-ref";
  const REF_DAYS = 30;
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
      throw e;
    }
    return { access: j.access_token, refresh: j.refresh_token, expires: Date.now() + (j.expires_in - 60) * 1000,
             id: j.user && j.user.id, email: j.user && j.user.email };
  }
  function save(s) { session = s; write(KEY, s); }

  async function token() {
    if (!session) throw Object.assign(new Error("signed out"), { kind: "signedout" });
    if (Date.now() > session.expires) {
      try { const s = await auth("token?grant_type=refresh_token", { refresh_token: session.refresh }); save({ ...s, id: s.id || session.id, email: s.email || session.email }); }
      catch { save(null); throw Object.assign(new Error("expired"), { kind: "signedout" }); }
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
    localCodes() { return read(CODES) || []; },
    rememberCode(code, plan) {
      const list = read(CODES) || [];
      if (!list.some(c => c.code === code)) { list.unshift({ code, plan, at: new Date().toISOString() }); write(CODES, list.slice(0, 20)); }
    },
    forgetCode(code) { write(CODES, (read(CODES) || []).filter(c => c.code !== code)); }
  };
})();
