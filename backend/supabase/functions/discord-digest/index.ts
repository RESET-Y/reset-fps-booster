// DISCORD DAILY DIGEST for Lukas, every day at 12:00 Berlin time.
//
// Reads the last 24 hours of #bug-reports, #hilfe and #vorschläge (text or
// forum channels, threads included) and sends Lukas one DM:
//   - Bugs: newest reports, urgent first (crash, won't start, ... by keyword),
//     with app version and GPU when the text names them
//   - Hilfe: questions nobody has answered yet
//   - Vorschläge: sorted by reactions, the community's votes
//   - payment, refund and account topics in a section of their own
//   - three things for today
//
// No AI and nothing that costs money: the sorting is plain keywords and
// counts. The bot never writes into the community channels, only the DM.
//
// Called by pg_cron at 10:00 and 11:00 UTC (migration 0009). It only acts when
// it is 12:xx in Berlin and today's report has not gone out yet, so a stray
// call does nothing. For a test run outside that window, send the header
// x-digest-secret with the value of DIGEST_SECRET.
//
// Secrets (Supabase > Edge Functions > Secrets), never in the repo:
//   DISCORD_BOT_TOKEN, DISCORD_GUILD_ID   already set for discord-sync
//   DISCORD_OWNER_ID         Lukas's Discord user id: gets the report
//   DIGEST_SECRET            optional: allows a manual test run
//   DISCORD_DIGEST_CHANNELS  optional: channel ids "bugs,help,ideas" if the names differ
import { createClient } from "npm:@supabase/supabase-js@2";
import { chunks, discord, dmChannel, messageLink } from "../_shared/discord.ts";

const guildId = (Deno.env.get("DISCORD_GUILD_ID") ?? "").trim();
const ownerId = (Deno.env.get("DISCORD_OWNER_ID") ?? "").trim();
const digestSecret = (Deno.env.get("DIGEST_SECRET") ?? "").trim();
const channelOverride = (Deno.env.get("DISCORD_DIGEST_CHANNELS") ?? "").trim();

const admin = createClient(Deno.env.get("SUPABASE_URL")!, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!, {
  auth: { persistSession: false },
});

const WINDOW_MS = 24 * 60 * 60 * 1000;
const MAX_LIST = 15; // per section; the rest is counted, not listed

type Kind = "bugs" | "help" | "ideas";
const KINDS: { kind: Kind; label: string; names: string[] }[] = [
  { kind: "bugs", label: "bug-reports", names: ["bug-reports", "bugreports", "bug-report", "bugs"] },
  { kind: "help", label: "hilfe", names: ["hilfe", "help", "support"] },
  { kind: "ideas", label: "vorschläge", names: ["vorschläge", "vorschlaege", "vorschlage", "suggestions", "ideen"] },
];

// Keywords, lower case. Deliberately broad: a false alarm costs a glance, a miss costs a user.
const URGENT = ["absturz", "abstürz", "stürzt", "crash", "startet nicht", "öffnet nicht", "geht nicht mehr", "funktioniert nicht mehr",
  "bluescreen", "blue screen", "freeze", "friert", "hängt sich", "datenverlust", "virus", "trojaner"];
const PERSONAL = ["bezahlt", "zahlung", "abgebucht", "paypal", "stripe", "kreditkarte", "rückerstatt", "refund", "geld zurück",
  "abo ", "kündig", "rechnung", "premium nicht", "premium fehlt", "kein premium", "code funktioniert nicht", "account", "konto",
  "login", "einloggen", "anmelden", "passwort", "gehackt"];

interface Channel { id: string; name: string; type: number; parent_id?: string | null }
interface Msg {
  id: string; channel_id: string; type: number; content: string; timestamp: string;
  author: { id: string; username: string; global_name?: string | null; bot?: boolean };
  message_reference?: { message_id?: string };
  attachments?: { filename: string }[];
  reactions?: { count: number }[];
}
interface Item {
  id: string; channel_id: string; kind: Kind; thread: string | null; time: number; author: string;
  fromTeam: boolean; replyTo: string | null; text: string; attachments: string[]; reactions: number;
}

// ---------------------------------------------------------------- reading Discord
const norm = (s: string) => s.toLowerCase().replace(/[^a-z0-9äöüß-]/g, "");

async function findChannels(): Promise<{ kind: Kind; label: string; channel: Channel }[]> {
  const all = await discord<Channel[]>("GET", `/guilds/${guildId}/channels`);
  if (channelOverride) {
    const ids = channelOverride.split(",").map((s) => s.trim());
    return KINDS.flatMap((k, i) => {
      const ch = all.find((c) => c.id === ids[i]);
      return ch ? [{ kind: k.kind, label: k.label, channel: ch }] : [];
    });
  }
  // text (0), announcement (5) or forum (15)
  const readable = all.filter((c) => [0, 5, 15].includes(c.type));
  // Exact names first (emoji and separators ignored): "support" alone must not pick #premium-support.
  const bare = (s: string) => norm(s).replace(/^-+|-+$/g, "");
  const pick = (k: (typeof KINDS)[number]) =>
    k.names.map((n) => readable.find((c) => bare(c.name) === norm(n))).find(Boolean) ??
    k.names.map((n) => readable.find((c) => norm(c.name).includes(norm(n)))).find(Boolean);
  return KINDS.flatMap((k) => {
    const ch = pick(k);
    return ch ? [{ kind: k.kind, label: k.label, channel: ch }] : [];
  });
}

/** Messages of one channel or thread newer than `since`, oldest first. */
async function messagesSince(channelId: string, since: number): Promise<Msg[]> {
  const out: Msg[] = [];
  let before = "";
  for (let page = 0; page < 10; page++) {
    const batch = await discord<Msg[]>("GET", `/channels/${channelId}/messages?limit=100${before ? `&before=${before}` : ""}`);
    if (!batch.length) break;
    for (const m of batch) if (Date.parse(m.timestamp) >= since) out.push(m);
    const oldest = batch[batch.length - 1];
    if (batch.length < 100 || Date.parse(oldest.timestamp) < since) break;
    before = oldest.id;
  }
  return out.reverse();
}

/** Threads (forum posts or text-channel threads) under the given channels with activity since `since`. */
async function threadsOf(parentIds: Set<string>, forumIds: string[], since: number): Promise<Channel[]> {
  type T = Channel & { thread_metadata?: { archive_timestamp?: string } };
  const active = await discord<{ threads: T[] }>("GET", `/guilds/${guildId}/threads/active`);
  const threads = active.threads.filter((t) => t.parent_id && parentIds.has(t.parent_id));
  for (const forum of forumIds) {
    const archived = await discord<{ threads: T[] }>("GET", `/channels/${forum}/threads/archived/public?limit=50`)
      .catch(() => ({ threads: [] as T[] }));
    for (const t of archived.threads) if (Date.parse(t.thread_metadata?.archive_timestamp ?? "") >= since) threads.push(t);
  }
  return threads;
}

// Tokens, passwords and e-mail addresses never appear in the report.
function scrub(text: string): string {
  return text
    .replace(/[\w.+-]+@[\w-]+\.[\w.-]+/g, "[E-Mail entfernt]")
    .replace(/\b[\w-]{24,}\.[\w-]{6}\.[\w-]{27,}\b/g, "[Token entfernt]")
    .replace(/\b(sk|pk|rk|ghp|gho|ghs|sb_secret|sb_publishable|eyJ)[\w\-.]{16,}/g, "[Token entfernt]")
    .replace(/\b(passwor[dt]|password|pw|pass)\s*[:=]\s*\S+/gi, "$1: [entfernt]")
    .replace(/\b[A-Z]{2}\d{2}(?:\s?[A-Z0-9]{4}){3,7}\b/g, "[IBAN entfernt]");
}

async function collect(since: number) {
  const channels = await findChannels();
  const kindOf = new Map(channels.map((c) => [c.channel.id, c.kind]));
  const items: Item[] = [];
  const toItem = (m: Msg, kind: Kind, thread: string | null): Item => ({
    id: m.id, channel_id: m.channel_id, kind, thread, time: Date.parse(m.timestamp),
    author: m.author.global_name || m.author.username,
    fromTeam: m.author.id === ownerId || !!m.author.bot,
    replyTo: m.message_reference?.message_id ?? null,
    text: scrub(m.content ?? ""),
    attachments: (m.attachments ?? []).map((a) => a.filename),
    reactions: (m.reactions ?? []).reduce((n, r) => n + r.count, 0),
  });
  const userMessage = (m: Msg) => m.type === 0 || m.type === 19 || m.type === 21;

  // A channel or thread the bot may not read is named in the report instead of failing all of it.
  const blocked = new Set<string>();
  const read = async (id: string, name: string) => {
    try { return await messagesSince(id, since); } catch (e) { console.log("DIGEST NO ACCESS", name, e); blocked.add(`#${name}`); return []; }
  };

  for (const { kind, channel } of channels) {
    if (channel.type === 15) continue; // forum: only threads carry messages
    for (const m of await read(channel.id, channel.name)) if (userMessage(m)) items.push(toItem(m, kind, null));
  }
  const threads = await threadsOf(new Set(kindOf.keys()),
    channels.filter((c) => c.channel.type === 15).map((c) => c.channel.id), since);
  for (const t of threads) {
    const kind = kindOf.get(t.parent_id ?? "");
    if (!kind) continue;
    for (const m of await read(t.id, t.name)) if (userMessage(m)) items.push(toItem(m, kind, t.name));
  }
  items.sort((a, b) => a.time - b.time);
  return {
    items,
    found: channels.map((c) => `#${c.channel.name}`).filter((n) => !blocked.has(n)),
    missing: KINDS.filter((k) => !channels.some((c) => c.kind === k.kind)).map((k) => `#${k.label}`),
    blocked: [...blocked],
  };
}

// ---------------------------------------------------------------- sorting without AI
const has = (text: string, words: string[]) => { const t = text.toLowerCase(); return words.some((w) => t.includes(w)); };
const version = (t: string) => t.match(/\bv?(\d\.\d{1,2}(?:\.\d{1,2})?(?:-beta)?)\b/i)?.[1] ?? null;
const gpu = (t: string) =>
  t.match(/\b(rtx\s?\d{4}(?:\s?(?:ti|super))?|gtx\s?\d{3,4}(?:\s?ti)?|rx\s?\d{3,4}(?:\s?xtx?)?|arc\s?[ab]\d{3}|intel(?:\(r\))?\s?(?:uhd|iris|hd)?\s?graphics|radeon\s?\w+)\b/i)?.[1] ?? null;
const windows = (t: string) => t.match(/\b(win(?:dows)?\s?1[01])\b/i)?.[1] ?? null;
const oneLine = (t: string, max = 180) => { const s = t.replace(/\s+/g, " ").trim(); return s.length > max ? s.slice(0, max - 1) + "…" : s || "(nur Anhang)"; };

interface Report {
  bugs: { item: Item; urgent: boolean; meta: string; follow: number }[];
  open: Item[];
  answered: number;
  ideas: { item: Item; votes: number }[];
  personal: Item[];
}

function analyze(items: Item[]): Report {
  // A "post" is a message that starts something: a forum/thread opener or a channel message that is no reply.
  const byThread = new Map<string, Item[]>();
  for (const m of items) if (m.thread) (byThread.get(m.channel_id) ?? byThread.set(m.channel_id, []).get(m.channel_id)!).push(m);
  const repliedTo = new Set(items.filter((m) => m.replyTo).map((m) => m.replyTo!));
  const posts = items.filter((m) => !m.fromTeam && !m.replyTo && (!m.thread || byThread.get(m.channel_id)![0].id === m.id));
  // Everything a user wrote in the same thread counts toward the post (version, GPU, more detail).
  const context = (p: Item) => (p.thread ? byThread.get(p.channel_id)!.filter((m) => !m.fromTeam) : [p]).map((m) => m.text).join("\n");

  const personal = posts.filter((p) => has(context(p), PERSONAL));
  const isPersonal = new Set(personal.map((p) => p.id));

  const bugs = posts.filter((p) => p.kind === "bugs" && !isPersonal.has(p.id)).map((p) => {
    const ctx = context(p);
    const meta = [version(ctx) ? `Version ${version(ctx)}` : "Version unklar", gpu(ctx) ?? "GPU unklar", windows(ctx) ?? "Windows unklar"];
    if (p.attachments.length) meta.push(`📎 ${p.attachments.length}`);
    const follow = p.thread ? byThread.get(p.channel_id)!.filter((m) => !m.fromTeam).length - 1 : 0;
    return { item: p, urgent: has(ctx, URGENT), meta: meta.join(" · "), follow };
  }).sort((a, b) => Number(b.urgent) - Number(a.urgent) || b.item.time - a.item.time);

  // Open: nobody replied to it, and in a thread nobody else wrote after it.
  const helpPosts = posts.filter((p) => p.kind === "help" && !isPersonal.has(p.id));
  const isOpen = (p: Item) => !repliedTo.has(p.id) &&
    !(p.thread && byThread.get(p.channel_id)!.some((m) => m.time > p.time && m.author !== p.author));
  const open = helpPosts.filter(isOpen);

  const ideas = posts.filter((p) => p.kind === "ideas" && !isPersonal.has(p.id))
    .map((p) => ({ item: p, votes: p.reactions })).sort((a, b) => b.votes - a.votes || b.item.time - a.item.time);

  return { bugs, open, answered: helpPosts.length - open.length, ideas, personal };
}

// ---------------------------------------------------------------- the DM
function warnings(missing: string[], blocked: string[]): string[] {
  const out: string[] = [];
  if (missing.length) out.push(`⚠️ Nicht gefunden: ${missing.join(", ")}. Kanalnamen prüfen oder DISCORD_DIGEST_CHANNELS setzen.`);
  if (blocked.length) out.push(`⚠️ Kein Zugriff: ${blocked.join(", ")}. Dem Bot dort „Kanal ansehen“ und „Nachrichtenverlauf lesen“ erlauben.`);
  return out;
}

function render(r: Report, total: number, day: string, found: string[], missing: string[], blocked: string[]): string {
  const link = (m: Item) => `([Link](${messageLink(guildId, m.channel_id, m.id)}))`;
  const title = (m: Item) => m.thread ? `**${oneLine(m.thread, 80)}**: ${oneLine(m.text, 140)}` : oneLine(m.text);
  const more = (n: number) => n > MAX_LIST ? [`-# … und ${n - MAX_LIST} weitere`] : [];
  const lines: string[] = [];
  lines.push(`## RESET Discord · Bericht vom ${day}`);
  lines.push(`Letzte 24 Stunden · ${total} Nachrichten aus ${found.join(", ") || "keinem Kanal"}`);
  lines.push(...warnings(missing, blocked));

  const urgent = r.bugs.filter((b) => b.urgent).length;
  lines.push("", `### 🐞 Bugs${r.bugs.length ? ` · ${r.bugs.length} neu${urgent ? `, ${urgent} dringend` : ""}` : ""}`);
  if (!r.bugs.length) lines.push("Keine neuen Bug-Meldungen.");
  for (const b of r.bugs.slice(0, MAX_LIST)) {
    lines.push(`${b.urgent ? "🔴" : "🟡"} ${title(b.item)} ${link(b.item)}`,
      `-# ${b.item.author} · ${b.meta}${b.follow > 0 ? ` · ${b.follow} weitere Nachricht(en) im Thread` : ""}`);
  }
  lines.push(...more(r.bugs.length));

  lines.push("", `### ❓ Offene Fragen${r.open.length ? ` · ${r.open.length}` : ""}`);
  if (!r.open.length) lines.push(r.answered ? `Alle ${r.answered} Fragen haben schon eine Antwort.` : "Keine neuen Fragen.");
  for (const q of r.open.slice(0, MAX_LIST)) lines.push(`• ${title(q)} ${link(q)}`, `-# ${q.author}`);
  lines.push(...more(r.open.length));

  lines.push("", `### 💡 Vorschläge${r.ideas.length ? ` · ${r.ideas.length}` : ""}`);
  if (!r.ideas.length) lines.push("Keine neuen Vorschläge.");
  r.ideas.slice(0, MAX_LIST).forEach((s, i) => lines.push(`${i + 1}. ${title(s.item)} ${link(s.item)}${s.votes ? ` · ${s.votes} Reaktion(en)` : ""}`));
  lines.push(...more(r.ideas.length));

  if (r.personal.length) {
    lines.push("", `### ⚠️ Für dich persönlich · ${r.personal.length}`, "-# Zahlung, Rückerstattung, Premium oder Account: bitte selbst antworten.");
    for (const p of r.personal.slice(0, MAX_LIST)) lines.push(`• ${title(p)} ${link(p)}`, `-# ${p.author} · #${KINDS.find((k) => k.kind === p.kind)!.label}`);
    lines.push(...more(r.personal.length));
  }

  const tasks: string[] = [];
  if (urgent) tasks.push(`${urgent} dringende${urgent === 1 ? "n Bug" : " Bugs"} ansehen`);
  if (r.personal.length) tasks.push(`${r.personal.length} Zahlungs-/Account-Thema${r.personal.length === 1 ? "" : "en"} beantworten`);
  if (r.open.length) tasks.push(`${r.open.length} offene Frage${r.open.length === 1 ? "" : "n"} beantworten`);
  if (r.bugs.length - urgent > 0) tasks.push(`${r.bugs.length - urgent} weitere Bug-Meldung${r.bugs.length - urgent === 1 ? "" : "en"} sichten`);
  if (r.ideas.length) tasks.push(`Vorschläge durchgehen (Top: ${oneLine(r.ideas[0].item.thread ?? r.ideas[0].item.text, 60)})`);
  lines.push("", "### ✅ Heute wichtig");
  if (!tasks.length) lines.push("Nichts Dringendes. 🎉");
  tasks.slice(0, 3).forEach((t, i) => lines.push(`${i + 1}. ${t}`));
  return lines.join("\n");
}

async function run(day: string): Promise<void> {
  const { items, found, missing, blocked } = await collect(Date.now() - WINDOW_MS);
  const dm = await dmChannel(ownerId);
  const text = items.length
    ? render(analyze(items), items.length, day, found, missing, blocked)
    : [`## RESET Discord · Bericht vom ${day}`,
      `In den letzten 24 Stunden gab es keine neuen Nachrichten in ${found.join(", ") || "den Kanälen"}.`,
      ...warnings(missing, blocked)].join("\n");
  for (const part of chunks(text)) {
    await discord("POST", `/channels/${dm}/messages`, { content: part, flags: 4 /* no link previews */ });
  }
}

// ---------------------------------------------------------------- entry
function berlinNow(): { day: string; hour: number; label: string } {
  const parts = Object.fromEntries(new Intl.DateTimeFormat("en-CA", {
    timeZone: "Europe/Berlin", year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", hour12: false,
  }).formatToParts(new Date()).map((p) => [p.type, p.value]));
  return { day: `${parts.year}-${parts.month}-${parts.day}`, hour: Number(parts.hour) % 24, label: `${parts.day}.${parts.month}.${parts.year}` };
}

Deno.serve(async (req) => {
  if (!guildId || !ownerId || !Deno.env.get("DISCORD_BOT_TOKEN")) {
    return Response.json({ error: "DISCORD_BOT_TOKEN, DISCORD_GUILD_ID or DISCORD_OWNER_ID not set" }, { status: 500 });
  }
  const forced = !!digestSecret && req.headers.get("x-digest-secret") === digestSecret;
  const { day, hour, label } = berlinNow();

  if (!forced) {
    if (hour !== 12) return Response.json({ skipped: `it is ${hour}:xx in Berlin, the report goes out at 12` });
    // Claim today; a second call the same day finds the row and stops.
    const { error } = await admin.from("discord_digest_runs").insert({ day });
    if (error) return Response.json({ skipped: "today's report already went out" });
  }

  const work = run(label).catch(async (e) => {
    console.log("DIGEST FAILED", e);
    if (!forced) await admin.from("discord_digest_runs").delete().eq("day", day);
    try {
      const dm = await dmChannel(ownerId);
      await discord("POST", `/channels/${dm}/messages`, { content: `⚠️ Der Tagesbericht ist fehlgeschlagen: ${String(e?.message ?? e).slice(0, 1500)}` });
    } catch { /* nothing left to tell */ }
  });

  // Answer the cron call right away; the report is built in the background.
  // deno-lint-ignore no-explicit-any
  const runtime = (globalThis as any).EdgeRuntime;
  if (runtime?.waitUntil) runtime.waitUntil(work); else await work;
  return Response.json({ started: true, forced, day });
});
