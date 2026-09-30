// DISCORD PREMIUM ROLE, KEPT IN STEP WITH THE DATABASE.
//
// Premium lives in public.entitlements. This gives the "Premium" role on the
// RESET Discord server to every account that has premium AND a linked Discord
// identity, and takes it away again when premium ends.
//
// Two ways in:
//   - with a user's access token (the app, right after linking Discord,
//     redeeming a code, or at start-up): syncs that one user
//   - without one (the hourly cron job): syncs everyone with Discord linked
//
// The second needs no secret on purpose. It only ever sets roles to what the
// database already says, so calling it cannot grant anything - the worst a
// stranger can do is make it run early, and a full run is throttled to once
// every five minutes.
//
// Secrets (Supabase > Edge Functions > Secrets), never in the repo:
//   DISCORD_BOT_TOKEN   the bot's token from the Discord developer portal
//   DISCORD_GUILD_ID    the RESET server's id
//   DISCORD_ROLE_NAME   optional, defaults to "Premium"
import { createClient } from "npm:@supabase/supabase-js@2";

const url = Deno.env.get("SUPABASE_URL")!;
const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;
const botToken = (Deno.env.get("DISCORD_BOT_TOKEN") ?? "").trim();
const guildId = (Deno.env.get("DISCORD_GUILD_ID") ?? "").trim();
const roleName = (Deno.env.get("DISCORD_ROLE_NAME") ?? "Premium").trim();

const admin = createClient(url, serviceKey, { auth: { persistSession: false } });
const api = "https://discord.com/api/v10";

let roleIdCache: string | null = null;
let lastFullSync = 0;

async function discord(method: string, path: string): Promise<Response> {
  for (let attempt = 0; attempt < 3; attempt++) {
    const res = await fetch(api + path, { method, headers: { Authorization: `Bot ${botToken}` } });
    if (res.status !== 429) return res;
    // Rate limited: Discord says how long to wait.
    const body = await res.json().catch(() => ({}));
    await new Promise((r) => setTimeout(r, Math.ceil((body.retry_after ?? 1) * 1000)));
  }
  return new Response(null, { status: 429 });
}

async function premiumRoleId(): Promise<string | null> {
  if (roleIdCache) return roleIdCache;
  const res = await discord("GET", `/guilds/${guildId}/roles`);
  if (!res.ok) { console.log("ROLES FAILED", res.status, await res.text()); return null; }
  const roles = await res.json() as { id: string; name: string }[];
  roleIdCache = roles.find((r) => r.name.toLowerCase() === roleName.toLowerCase())?.id ?? null;
  if (!roleIdCache) console.log(`NO ROLE named "${roleName}" on the server`);
  return roleIdCache;
}

// The Discord user id from a Supabase identity. Newer projects put it in
// provider_id; older ones in identity_data.sub or id.
// deno-lint-ignore no-explicit-any
function discordIdOf(user: any): string | null {
  // deno-lint-ignore no-explicit-any
  const identity = (user?.identities ?? []).find((i: any) => i.provider === "discord");
  if (!identity) return null;
  return identity.identity_data?.provider_id ?? identity.identity_data?.sub ?? identity.provider_id ?? null;
}

async function hasPremium(userId: string): Promise<boolean> {
  const { data, error } = await admin
    .from("entitlements")
    .select("valid_until")
    .eq("user_id", userId)
    .eq("product", "premium");
  if (error) { console.log("ENTITLEMENTS FAILED", error.message); return false; }
  const now = Date.now();
  return (data ?? []).some((e) => e.valid_until === null || Date.parse(e.valid_until) > now);
}

type Outcome = "granted" | "removed" | "not_linked" | "not_in_server" | "error";

// deno-lint-ignore no-explicit-any
async function syncUser(user: any): Promise<Outcome> {
  const discordId = discordIdOf(user);
  if (!discordId) return "not_linked";
  const roleId = await premiumRoleId();
  if (!roleId) return "error";

  const premium = await hasPremium(user.id);
  const res = await discord(premium ? "PUT" : "DELETE", `/guilds/${guildId}/members/${discordId}/roles/${roleId}`);
  if (res.ok || res.status === 204) return premium ? "granted" : "removed";
  const text = await res.text();
  if (res.status === 404) return "not_in_server"; // not a member (yet) - next run catches it
  console.log("ROLE CHANGE FAILED", res.status, text);
  return "error";
}

Deno.serve(async (req) => {
  if (!botToken || !guildId) {
    return Response.json({ error: "DISCORD_BOT_TOKEN or DISCORD_GUILD_ID not set" }, { status: 500 });
  }

  const auth = req.headers.get("Authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7) : "";

  // One user: the caller, identified by their own access token.
  if (token) {
    const { data, error } = await admin.auth.getUser(token);
    if (!error && data.user) {
      // getUser returns identities; re-read through the admin API to be sure
      // they are complete right after linking.
      const { data: full } = await admin.auth.admin.getUserById(data.user.id);
      const outcome = await syncUser(full.user ?? data.user);
      return Response.json({ outcome });
    }
    // An anon key or an expired token falls through to the full sync.
  }

  // Everyone with Discord linked.
  if (Date.now() - lastFullSync < 5 * 60 * 1000) return Response.json({ skipped: "ran less than 5 minutes ago" });
  lastFullSync = Date.now();

  const counts: Record<Outcome, number> = { granted: 0, removed: 0, not_linked: 0, not_in_server: 0, error: 0 };
  for (let page = 1; ; page++) {
    const { data, error } = await admin.auth.admin.listUsers({ page, perPage: 200 });
    if (error) { console.log("LIST USERS FAILED", error.message); break; }
    for (const user of data.users) {
      if (!discordIdOf(user)) continue;
      counts[await syncUser(user)]++;
    }
    if (data.users.length < 200) break;
  }
  console.log("FULL SYNC", JSON.stringify(counts));
  return Response.json(counts);
});
