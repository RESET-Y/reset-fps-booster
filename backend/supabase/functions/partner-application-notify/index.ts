// PARTNER APPLICATION NOTICE: one e-mail to Lukas per new application.
//
// Called by the database trigger of migration 0011 with {"id": 123}, without a
// login. It reads the application with the service role, mails it, and sets
// notified_at, so a second call for the same application sends nothing.
//
// Secrets (Supabase > Edge Functions > Secrets), never in the repo:
//   RESEND_API_KEY   the API key from resend.com (the e-mail service)
//   NOTIFY_FROM      sender, e.g. "RESET <bewerbung@reset-booster.online>";
//                    the domain must be verified at Resend. Without it, the
//                    test sender onboarding@resend.dev is used.
//   NOTIFY_TO        optional: where the mail goes, default lukas.reschke80@gmail.com
import { createClient } from "npm:@supabase/supabase-js@2";

const admin = createClient(Deno.env.get("SUPABASE_URL")!, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!, {
  auth: { persistSession: false },
});

Deno.serve(async (req) => {
  let id: number;
  try {
    id = Number((await req.json()).id);
  } catch {
    return Response.json({ error: "bad request" }, { status: 400 });
  }
  if (!Number.isInteger(id) || id <= 0) return Response.json({ error: "bad id" }, { status: 400 });

  const { data: app, error } = await admin.from("partner_applications").select("*").eq("id", id).maybeSingle();
  if (error || !app) return Response.json({ error: "not found" }, { status: 404 });
  if (app.notified_at) return Response.json({ skipped: "already sent" });

  const key = (Deno.env.get("RESEND_API_KEY") ?? "").trim();
  if (!key) return Response.json({ error: "RESEND_API_KEY not set" }, { status: 500 });

  const text = [
    `Neue Partner-Bewerbung (#${app.id})`,
    "",
    `Name: ${app.name}`,
    `Kanal: ${app.channel}`,
    `Reichweite: ${app.audience ?? "-"}`,
    `Kontakt: ${app.contact}`,
    "",
    app.message ?? "(keine Nachricht)",
    "",
    "Beantworten im Manager: https://reset-booster.online/manager/",
  ].join("\n");

  const res = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: { Authorization: `Bearer ${key}`, "Content-Type": "application/json" },
    body: JSON.stringify({
      from: (Deno.env.get("NOTIFY_FROM") ?? "").trim() || "RESET <onboarding@resend.dev>",
      to: [(Deno.env.get("NOTIFY_TO") ?? "").trim() || "lukas.reschke80@gmail.com"],
      subject: `Partner-Bewerbung: ${app.name}`,
      text,
    }),
  });
  if (!res.ok) {
    return Response.json({ error: "mail failed", status: res.status, detail: (await res.text()).slice(0, 300) }, { status: 502 });
  }

  await admin.from("partner_applications").update({ notified_at: new Date().toISOString() }).eq("id", id);
  return Response.json({ sent: true });
});
