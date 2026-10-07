// Small Discord REST helper shared by discord-digest and discord-interactions.
const api = "https://discord.com/api/v10";
const botToken = (Deno.env.get("DISCORD_BOT_TOKEN") ?? "").trim();

export const MAX_MESSAGE = 2000;

/** A Discord REST call as the bot, retried on rate limits. Returns parsed JSON (or null). */
export async function discord<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
  for (let attempt = 0; attempt < 4; attempt++) {
    const res = await fetch(api + path, {
      method,
      headers: { Authorization: `Bot ${botToken}`, ...(body === undefined ? {} : { "Content-Type": "application/json" }) },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (res.status === 429) {
      const wait = await res.json().catch(() => ({}));
      await new Promise((r) => setTimeout(r, Math.ceil(((wait as { retry_after?: number }).retry_after ?? 1) * 1000)));
      continue;
    }
    const text = await res.text();
    if (!res.ok) throw new Error(`Discord ${method} ${path} -> ${res.status}: ${text.slice(0, 300)}`);
    return (text ? JSON.parse(text) : null) as T;
  }
  throw new Error(`Discord ${method} ${path} -> still rate limited`);
}

/** Splits text into chunks Discord accepts, preferring line breaks. */
export function chunks(text: string, size = MAX_MESSAGE - 50): string[] {
  const out: string[] = [];
  let rest = text.trim();
  while (rest.length > size) {
    let cut = rest.lastIndexOf("\n", size);
    if (cut < size / 2) cut = size;
    out.push(rest.slice(0, cut));
    rest = rest.slice(cut).trimStart();
  }
  if (rest) out.push(rest);
  return out;
}

/** The DM channel between the bot and a user. */
export async function dmChannel(userId: string): Promise<string> {
  const ch = await discord<{ id: string }>("POST", "/users/@me/channels", { recipient_id: userId });
  return ch.id;
}

export function messageLink(guildId: string, channelId: string, messageId: string): string {
  return `https://discord.com/channels/${guildId}/${channelId}/${messageId}`;
}

/** Buttons under a draft in Lukas's DM. The draft id travels in custom_id. */
export function draftButtons(draftId: number) {
  return [{
    type: 1,
    components: [
      { type: 2, style: 3, label: "Senden", custom_id: `draft:send:${draftId}` },
      { type: 2, style: 1, label: "Bearbeiten", custom_id: `draft:edit:${draftId}` },
      { type: 2, style: 4, label: "Verwerfen", custom_id: `draft:discard:${draftId}` },
    ],
  }];
}
