// NEWSLETTER: SIGN-UP WITH DOUBLE OPT-IN, CONFIRM, UNSUBSCRIBE, SEND.
//
// Public, no login (verify_jwt is off for this function, see config.toml):
//   POST { action: "subscribe", email, lang, source, consent: true, website }
//        Answers { ok: true } for every valid address - new, pending or
//        already confirmed - so the answer never tells a stranger who is
//        subscribed. A confirmation mail goes out for a new or unsubscribed
//        address, and again for a pending one at most every 10 minutes.
//        "website" is a honeypot field that people never fill in.
//   POST { action: "confirm", token }       { ok: true } | { error }
//   POST { action: "unsubscribe", token }   { ok: true } | { error }
//   POST ?u=TOKEN   one-click unsubscribe from the mail program (RFC 8058)
//   GET  ?u=TOKEN   sends the browser to the unsubscribe page instead, so a
//                   link scanner that opens every link unsubscribes nobody
//
// Manager only (Authorization: Bearer <session of a manager>):
//   POST { action: "stats" }   counts per status and the last newsletters
//   POST { action: "send", subject, body, test: true }
//        sends to the manager's own address only, marked [TEST]
//   POST { action: "send", subject, body, test: false, expected: N }
//        sends to every confirmed address. Refused when N is not the current
//        number of confirmed addresses (the page showed an old number) or the
//        same subject went out in the last 10 minutes (a double click).
//
// Secrets (Supabase > Edge Functions > Secrets), never in the repo:
//   RESEND_API_KEY    already set for the partner notice
//   NEWSLETTER_FROM   sender, e.g. "RESET <news@reset-booster.online>". The
//                     domain must be verified at Resend: the test sender
//                     onboarding@resend.dev only reaches your own address.
//   SITE_URL          optional, default https://reset-booster.online
import { createClient, type SupabaseClient } from "npm:@supabase/supabase-js@2";

const SUBS = "newsletter_subscribers";
const ISSUES = "newsletter_issues";
const RESEND_MINUTES = 10;      // a pending address gets a new confirmation mail at most this often
const CONFIRM_DAYS = 7;         // how long the confirmation link works
const PENDING_DAYS = 30;        // unconfirmed sign-ups are deleted after this
const HOURLY_CAP = 60;          // confirmation mails per hour, all addresses together
const BATCH = 100;              // Resend's limit per batch call

const IMPRINT = "RESET FPS BOOSTER · Lukas Reschke · Römerstraße 13 · 70180 Stuttgart · Deutschland · lukas.reschke80@gmail.com";

// The exact sentence next to the checkbox. The website shows the same text;
// it is stored with each sign-up as the record of what was agreed to.
export const CONSENT = {
  de: "Ja, ich möchte den RESET-Newsletter mit Neuigkeiten zu Updates, Funktionen und Angeboten per E-Mail bekommen. Abmelden kann ich mich jederzeit über den Link in jeder Mail.",
  en: "Yes, I want to get the RESET newsletter with news about updates, features and offers by e-mail. I can unsubscribe at any time with the link in every mail.",
} as const;
type Lang = keyof typeof CONSENT;

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "authorization, apikey, content-type, x-client-info",
};
const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { ...CORS, "Content-Type": "application/json" } });

const env = (k: string) => (Deno.env.get(k) ?? "").trim();
const site = () => env("SITE_URL").replace(/\/+$/, "") || "https://reset-booster.online";
const fnUrl = () => env("SUPABASE_URL").replace(/\/+$/, "") + "/functions/v1/newsletter";
const sender = () => env("NEWSLETTER_FROM") || env("NOTIFY_FROM") || "RESET <onboarding@resend.dev>";
const iso = (ms: number) => new Date(ms).toISOString();

let client: SupabaseClient | null = null;
const db = () => client ??= createClient(env("SUPABASE_URL"), env("SUPABASE_SERVICE_ROLE_KEY"), {
  auth: { persistSession: false },
});

const EMAIL_RE = /^[^@\s]+@[^@\s]+\.[^@\s]+$/;
const TOKEN_RE = /^[0-9a-f]{64}$/;
const hex = (b: ArrayBuffer | Uint8Array) => [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, "0")).join("");
function newToken() {
  const b = new Uint8Array(32);
  crypto.getRandomValues(b);
  return hex(b);
}
async function sha256(s: string) {
  return hex(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s)));
}

// ---------------------------------------------------------------------------
// mail
// ---------------------------------------------------------------------------
async function resend(path: string, payload: unknown) {
  const key = env("RESEND_API_KEY");
  if (!key) throw new Error("RESEND_API_KEY not set");
  const r = await fetch("https://api.resend.com" + path, {
    method: "POST",
    headers: { Authorization: `Bearer ${key}`, "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (!r.ok) throw new Error(`resend ${r.status}: ${(await r.text()).slice(0, 200)}`);
  return await r.json().catch(() => ({}));
}

const esc = (s: string) =>
  s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]!));
// Plain text to simple HTML: blank lines make paragraphs, web addresses links.
export function toHtml(text: string) {
  return text.split(/\n{2,}/).map((p) =>
    "<p>" + esc(p).replace(/\n/g, "<br>").replace(/https?:\/\/[^\s<]+/g, (u) => `<a href="${u}" style="color:#e8121f">${u}</a>`) + "</p>"
  ).join("\n");
}
function page(inner: string) {
  return `<!doctype html><html><body style="margin:0;padding:24px;background:#f4f4f4;font-family:Arial,Helvetica,sans-serif;color:#111;line-height:1.55">` +
    `<div style="max-width:560px;margin:0 auto;background:#fff;padding:28px;border-top:4px solid #e8121f">${inner}</div></body></html>`;
}

const CONFIRM_MAIL = {
  de: {
    subject: "Bitte bestätige deine Newsletter-Anmeldung",
    lines: (link: string) => [
      "Hallo,",
      "",
      "du hast dich für den RESET-Newsletter angemeldet, oder jemand hat deine Adresse eingetragen. Bitte bestätige die Anmeldung mit diesem Link:",
      "",
      link,
      "",
      `Der Link gilt ${CONFIRM_DAYS} Tage. Hast du dich nicht angemeldet, ignoriere diese Mail einfach: Ohne Bestätigung bekommst du keinen Newsletter, und deine Adresse wird nach ${PENDING_DAYS} Tagen gelöscht.`,
    ],
    button: "Anmeldung bestätigen",
  },
  en: {
    subject: "Please confirm your newsletter sign-up",
    lines: (link: string) => [
      "Hi,",
      "",
      "you signed up for the RESET newsletter, or someone entered your address. Please confirm the sign-up with this link:",
      "",
      link,
      "",
      `The link works for ${CONFIRM_DAYS} days. If you didn't sign up, just ignore this mail: without confirming you get no newsletter, and your address is deleted after ${PENDING_DAYS} days.`,
    ],
    button: "Confirm sign-up",
  },
};

async function sendConfirmation(email: string, lang: Lang, token: string) {
  const link = `${site()}/newsletter.html?confirm=${token}`;
  const m = CONFIRM_MAIL[lang];
  const lines = m.lines(link);
  const text = [...lines, "", "--", IMPRINT].join("\n");
  const intro = lines.slice(0, 3).filter(Boolean).map((l) => `<p>${esc(l)}</p>`).join("");
  const html = page(
    intro +
      `<p style="margin:24px 0"><a href="${link}" style="background:#e8121f;color:#fff;padding:12px 22px;text-decoration:none;font-weight:bold;display:inline-block">${esc(m.button)}</a></p>` +
      `<p style="font-size:13px;color:#555">${esc(lines[6])}</p>` +
      `<p style="font-size:12px;color:#777;margin-top:24px">${esc(IMPRINT)}</p>`,
  );
  await resend("/emails", { from: sender(), to: [email], subject: m.subject, text, html });
}

function newsletterMail(to: string, lang: Lang, unsubToken: string, subject: string, body: string) {
  const unsubPage = `${site()}/newsletter.html?u=${unsubToken}`;
  const foot = lang === "en"
    ? ["You get this mail because you confirmed the RESET newsletter.", `Unsubscribe: ${unsubPage}`]
    : ["Du bekommst diese Mail, weil du den RESET-Newsletter bestätigt hast.", `Abmelden: ${unsubPage}`];
  return {
    from: sender(),
    to: [to],
    subject,
    text: [body, "", "--", ...foot, IMPRINT].join("\n"),
    html: page(
      toHtml(body) +
        `<hr style="border:0;border-top:1px solid #ddd;margin:28px 0 14px">` +
        `<p style="font-size:12px;color:#777">${esc(foot[0])}<br><a href="${unsubPage}" style="color:#777">${lang === "en" ? "Unsubscribe" : "Abmelden"}</a><br>${esc(IMPRINT)}</p>`,
    ),
    // One-click unsubscribe in the mail program itself (RFC 8058).
    headers: {
      "List-Unsubscribe": `<${fnUrl()}?u=${unsubToken}>`,
      "List-Unsubscribe-Post": "List-Unsubscribe=One-Click",
    },
  };
}

// ---------------------------------------------------------------------------
// public: subscribe, confirm, unsubscribe
// ---------------------------------------------------------------------------
async function subscribe(b: Record<string, unknown>) {
  if (String(b.website ?? "") !== "") return json({ ok: true }); // a bot: pretend, store nothing
  const email = String(b.email ?? "").trim().toLowerCase();
  const lang: Lang = b.lang === "en" ? "en" : "de";
  const source = ["website", "account", "app"].includes(String(b.source)) ? String(b.source) : "website";
  if (email.length < 5 || email.length > 200 || !EMAIL_RE.test(email)) return json({ error: "email" }, 400);
  if (b.consent !== true) return json({ error: "consent" }, 400);

  const now = Date.now();
  // Housekeeping on the way: unconfirmed sign-ups do not stay.
  await db().from(SUBS).delete().eq("status", "pending").lt("requested_at", iso(now - PENDING_DAYS * 864e5));

  const { count } = await db().from(SUBS).select("id", { count: "exact", head: true }).gt("confirm_sent_at", iso(now - 3600e3));
  if ((count ?? 0) >= HOURLY_CAP) return json({ error: "busy" }, 429);

  const { data: row, error } = await db().from(SUBS).select("id, status, confirm_sent_at").eq("email", email).maybeSingle();
  if (error) throw error;
  if (row?.status === "confirmed") return json({ ok: true });
  if (row?.status === "pending" && row.confirm_sent_at && now - Date.parse(row.confirm_sent_at) < RESEND_MINUTES * 60e3) {
    return json({ ok: true });
  }

  const token = newToken();
  const fields = {
    lang, source, status: "pending", consent_text: CONSENT[lang],
    confirm_hash: await sha256(token), confirm_sent_at: iso(now), requested_at: iso(now), confirmed_at: null,
  };
  if (row) {
    const { error: e } = await db().from(SUBS).update(fields).eq("id", row.id);
    if (e) throw e;
  } else {
    const { error: e } = await db().from(SUBS).insert({ ...fields, email, unsubscribe_token: newToken() });
    if (e) {
      // Two sign-ups for the same address at the same moment: the other one mails.
      if (e.code === "23505") return json({ ok: true });
      throw e;
    }
  }

  try {
    await sendConfirmation(email, lang, token);
  } catch (e) {
    console.error(e);
    // Nothing went out, so do not hold the address back for 10 minutes.
    await db().from(SUBS).update({ confirm_sent_at: null }).eq("email", email);
    return json({ error: "mail" }, 502);
  }
  return json({ ok: true });
}

async function confirm(b: Record<string, unknown>) {
  const token = String(b.token ?? "");
  if (!TOKEN_RE.test(token)) return json({ error: "invalid" }, 400);
  const { data: row } = await db().from(SUBS).select("id, status, confirm_sent_at").eq("confirm_hash", await sha256(token)).maybeSingle();
  if (!row || row.status !== "pending") return json({ error: "invalid" }, 400);
  if (!row.confirm_sent_at || Date.now() - Date.parse(row.confirm_sent_at) > CONFIRM_DAYS * 864e5) {
    return json({ error: "expired" }, 400);
  }
  const { error } = await db().from(SUBS)
    .update({ status: "confirmed", confirmed_at: iso(Date.now()), confirm_hash: null }).eq("id", row.id);
  if (error) throw error;
  return json({ ok: true });
}

async function unsubscribe(token: string) {
  if (!TOKEN_RE.test(token)) return false;
  const { data: row } = await db().from(SUBS).select("id, status").eq("unsubscribe_token", token).maybeSingle();
  if (!row) return false;
  if (row.status !== "unsubscribed") {
    const { error } = await db().from(SUBS)
      .update({ status: "unsubscribed", unsubscribed_at: iso(Date.now()), confirm_hash: null }).eq("id", row.id);
    if (error) throw error;
  }
  return true;
}

// ---------------------------------------------------------------------------
// manager: numbers and sending
// ---------------------------------------------------------------------------
async function manager(req: Request) {
  const jwt = (req.headers.get("authorization") ?? "").replace(/^Bearer\s+/i, "");
  if (!jwt) return null;
  const { data: who } = await db().auth.getUser(jwt);
  if (!who?.user) return null;
  const { data: role } = await db().from("user_roles")
    .select("role").eq("user_id", who.user.id).eq("role", "manager").maybeSingle();
  return role ? { id: who.user.id as string, email: String(who.user.email ?? "") } : null;
}

async function stats() {
  const n = async (status: string) =>
    (await db().from(SUBS).select("id", { count: "exact", head: true }).eq("status", status)).count ?? 0;
  const { data: issues } = await db().from(ISSUES)
    .select("id, subject, recipients, failed, sent_at").order("sent_at", { ascending: false }).limit(10);
  return json({
    confirmed: await n("confirmed"), pending: await n("pending"), unsubscribed: await n("unsubscribed"),
    issues: issues ?? [], sender: sender(),
  });
}

async function confirmedSubscribers() {
  const all: { email: string; lang: Lang; unsubscribe_token: string }[] = [];
  for (let from = 0;; from += 1000) {
    const { data, error } = await db().from(SUBS).select("email, lang, unsubscribe_token")
      .eq("status", "confirmed").order("id").range(from, from + 999);
    if (error) throw error;
    all.push(...(data ?? []));
    if (!data || data.length < 1000) return all;
  }
}

async function send(b: Record<string, unknown>, mgr: { id: string; email: string }) {
  const subject = String(b.subject ?? "").trim();
  const body = String(b.body ?? "").replace(/\r\n/g, "\n").trim();
  if (subject.length < 3 || subject.length > 150) return json({ error: "subject" }, 400);
  if (body.length < 10 || body.length > 20000) return json({ error: "body" }, 400);

  if (b.test === true) {
    if (!mgr.email) return json({ error: "no manager e-mail" }, 400);
    await resend("/emails", newsletterMail(mgr.email, "de", "0".repeat(64), "[TEST] " + subject, body));
    return json({ ok: true, sent: 1, to: mgr.email });
  }

  const { data: recent } = await db().from(ISSUES).select("id")
    .eq("subject", subject).gt("sent_at", iso(Date.now() - 10 * 60e3)).limit(1);
  if (recent && recent.length) return json({ error: "just sent" }, 409);

  const subs = await confirmedSubscribers();
  if (!subs.length) return json({ error: "no subscribers" }, 400);
  if (Number(b.expected) !== subs.length) return json({ error: "count changed", count: subs.length }, 409);

  let sent = 0, failed = 0;
  for (let i = 0; i < subs.length; i += BATCH) {
    const batch = subs.slice(i, i + BATCH).map((s) => newsletterMail(s.email, s.lang, s.unsubscribe_token, subject, body));
    try {
      await resend("/emails/batch", batch);
      sent += batch.length;
    } catch (e) {
      console.error(e);
      failed += batch.length;
    }
  }
  await db().from(ISSUES).insert({ subject, body, recipients: sent, failed, sent_by: mgr.id });
  return json({ ok: failed === 0, sent, failed }, failed && !sent ? 502 : 200);
}

// ---------------------------------------------------------------------------
export async function handler(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });

  const u = new URL(req.url).searchParams.get("u");
  if (u !== null) {
    if (req.method === "GET") return Response.redirect(`${site()}/newsletter.html?u=${encodeURIComponent(u)}`, 302);
    if (req.method === "POST") {
      try {
        const ok = await unsubscribe(u);
        return new Response(ok ? "unsubscribed" : "invalid", { status: ok ? 200 : 400, headers: CORS });
      } catch (e) {
        console.error(e);
        return new Response("error", { status: 500, headers: CORS });
      }
    }
  }
  if (req.method !== "POST") return json({ error: "method not allowed" }, 405);

  let b: Record<string, unknown>;
  try {
    b = await req.json();
  } catch {
    return json({ error: "bad json" }, 400);
  }
  try {
    switch (b?.action) {
      case "subscribe":
        return await subscribe(b);
      case "confirm":
        return await confirm(b);
      case "unsubscribe":
        return (await unsubscribe(String(b.token ?? ""))) ? json({ ok: true }) : json({ error: "invalid" }, 400);
      case "stats":
      case "send": {
        const mgr = await manager(req);
        if (!mgr) return json({ error: "not allowed" }, 403);
        return b.action === "stats" ? await stats() : await send(b, mgr);
      }
      default:
        return json({ error: "unknown action" }, 400);
    }
  } catch (e) {
    console.error(e);
    return json({ error: "server" }, 500);
  }
}
