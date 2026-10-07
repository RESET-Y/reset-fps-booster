// DISCORD INTERACTIONS: the buttons under the reply drafts in Lukas's DM.
//
//   Senden      posts the draft as a reply to the original question
//   Bearbeiten  opens a text field with the draft; submitting it posts the edited text
//   Verwerfen   drops the draft
//
// This is the only place a reply ever reaches the community, and only for a
// click by DISCORD_OWNER_ID. Every request is checked against Discord's
// Ed25519 signature first; anything unsigned is rejected.
//
// Set as "Interactions Endpoint URL" in the Discord developer portal:
//   https://sjwparnlbtuiyqmagyfg.supabase.co/functions/v1/discord-interactions
//
// Secrets: DISCORD_BOT_TOKEN, DISCORD_OWNER_ID (see discord-digest) and
//   DISCORD_PUBLIC_KEY   the application's public key from the developer portal
import { createClient } from "npm:@supabase/supabase-js@2";
import { discord } from "../_shared/discord.ts";

const ownerId = (Deno.env.get("DISCORD_OWNER_ID") ?? "").trim();
const publicKeyHex = (Deno.env.get("DISCORD_PUBLIC_KEY") ?? "").trim();
const admin = createClient(Deno.env.get("SUPABASE_URL")!, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!, {
  auth: { persistSession: false },
});

// Interaction and response types, see Discord's interaction docs.
const PING = 1, COMPONENT = 3, MODAL_SUBMIT = 5;
const PONG = 1, CHANNEL_MESSAGE = 4, DEFERRED_UPDATE = 6, UPDATE_MESSAGE = 7, MODAL = 9;
const EPHEMERAL = 64;

const hex = (s: string) => new Uint8Array(s.match(/../g)!.map((b) => parseInt(b, 16)));
let publicKey: CryptoKey | null = null;

async function verified(req: Request, body: string): Promise<boolean> {
  const sig = req.headers.get("x-signature-ed25519");
  const ts = req.headers.get("x-signature-timestamp");
  if (!sig || !ts || !publicKeyHex) return false;
  try {
    publicKey ??= await crypto.subtle.importKey("raw", hex(publicKeyHex), { name: "Ed25519" }, false, ["verify"]);
    return await crypto.subtle.verify("Ed25519", publicKey, hex(sig), new TextEncoder().encode(ts + body));
  } catch {
    return false;
  }
}

interface Draft { id: number; channel_id: string; message_id: string; draft: string; status: string }

async function loadDraft(id: number): Promise<Draft | null> {
  const { data } = await admin.from("discord_drafts").select("id, channel_id, message_id, draft, status").eq("id", id).single();
  return data as Draft | null;
}

/** Posts the reply under the original question. Mentions nobody except the person asking. */
async function postReply(d: Draft, text: string): Promise<string> {
  const msg = await discord<{ id: string }>("POST", `/channels/${d.channel_id}/messages`, {
    content: text.slice(0, 2000),
    message_reference: { message_id: d.message_id, channel_id: d.channel_id, fail_if_not_exists: false },
    allowed_mentions: { parse: [], replied_user: true },
  });
  return msg.id;
}

const stamp = () => new Intl.DateTimeFormat("de-DE", { timeZone: "Europe/Berlin", hour: "2-digit", minute: "2-digit" }).format(new Date());

/** Rewrites the DM message after a decision: same text, a status line, no buttons. */
// deno-lint-ignore no-explicit-any
async function finish(interaction: any, status: string, newDraft?: string): Promise<void> {
  let content: string = interaction.message?.content ?? "";
  if (newDraft !== undefined) content = content.replace(/\n\n[\s\S]*$/, `\n\n${newDraft}`);
  content = `${content}\n\n${status}`.slice(0, 2000);
  await discord("PATCH", `/webhooks/${interaction.application_id}/${interaction.token}/messages/@original`, { content, components: [] });
}

// deno-lint-ignore no-explicit-any
async function decide(interaction: any, action: string, id: number, editedText?: string): Promise<void> {
  const d = await loadDraft(id);
  if (!d) return finish(interaction, "⚠️ Entwurf nicht gefunden.");
  if (d.status !== "pending") return finish(interaction, `-# Schon erledigt (${d.status === "sent" ? "gesendet" : "verworfen"}).`);

  // Claim the draft first, so a double click cannot post it twice.
  const { data: claimed } = await admin.from("discord_drafts")
    .update({ status: action === "discard" ? "discarded" : "sent", decided_at: new Date().toISOString(), ...(editedText ? { draft: editedText } : {}) })
    .eq("id", id).eq("status", "pending").select("id");
  if (!claimed?.length) return finish(interaction, "-# Schon erledigt.");

  if (action === "discard") return finish(interaction, `🗑️ **Verworfen** um ${stamp()}`);

  try {
    const sentId = await postReply(d, editedText ?? d.draft);
    await admin.from("discord_drafts").update({ sent_message_id: sentId }).eq("id", id);
    await finish(interaction, `✅ **${editedText ? "Bearbeitet gesendet" : "Gesendet"}** um ${stamp()}`, editedText);
  } catch (e) {
    // Not posted: put it back so Lukas can try again.
    await admin.from("discord_drafts").update({ status: "pending", decided_at: null }).eq("id", id);
    await finish(interaction, `⚠️ Senden fehlgeschlagen: ${String((e as Error).message).slice(0, 300)}`);
  }
}

Deno.serve(async (req) => {
  const body = await req.text();
  if (!(await verified(req, body))) return new Response("invalid request signature", { status: 401 });
  const interaction = JSON.parse(body);

  if (interaction.type === PING) return Response.json({ type: PONG });

  const userId = interaction.user?.id ?? interaction.member?.user?.id;
  if (!ownerId || userId !== ownerId) {
    return Response.json({ type: CHANNEL_MESSAGE, data: { flags: EPHEMERAL, content: "Nur Lukas kann Antwortentwürfe freigeben." } });
  }

  const [scope, action, rawId] = String(interaction.data?.custom_id ?? "").split(":");
  const id = Number(rawId);
  if (scope !== "draft" || !Number.isInteger(id)) {
    return Response.json({ type: CHANNEL_MESSAGE, data: { flags: EPHEMERAL, content: "Unbekannte Aktion." } });
  }

  if (interaction.type === COMPONENT && action === "edit") {
    const d = await loadDraft(id);
    if (!d || d.status !== "pending") {
      return Response.json({ type: CHANNEL_MESSAGE, data: { flags: EPHEMERAL, content: "Dieser Entwurf ist schon erledigt." } });
    }
    return Response.json({
      type: MODAL,
      data: {
        custom_id: `draft:modal:${id}`,
        title: "Antwort bearbeiten",
        components: [{ type: 1, components: [{
          type: 4, custom_id: "text", style: 2, label: "Antwort", value: d.draft.slice(0, 2000),
          min_length: 1, max_length: 2000, required: true,
        }] }],
      },
    });
  }

  let work: Promise<void> | null = null;
  if (interaction.type === COMPONENT && (action === "send" || action === "discard")) {
    work = decide(interaction, action, id);
  } else if (interaction.type === MODAL_SUBMIT && action === "modal") {
    const text = String(interaction.data?.components?.[0]?.components?.[0]?.value ?? "").trim();
    if (!text) return Response.json({ type: CHANNEL_MESSAGE, data: { flags: EPHEMERAL, content: "Leere Antwort wird nicht gesendet." } });
    work = decide(interaction, "send", id, text);
  }
  if (!work) return Response.json({ type: CHANNEL_MESSAGE, data: { flags: EPHEMERAL, content: "Unbekannte Aktion." } });

  // Discord wants an answer within 3 seconds: acknowledge now, do the work after.
  // deno-lint-ignore no-explicit-any
  const runtime = (globalThis as any).EdgeRuntime;
  const logged = work.catch((e) => console.log("INTERACTION FAILED", e));
  if (runtime?.waitUntil) runtime.waitUntil(logged); else await logged;
  return Response.json({ type: DEFERRED_UPDATE });
});
