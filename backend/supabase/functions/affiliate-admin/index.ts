// AFFILIATES FROM THE MANAGER PAGE: THE PART THAT TALKS TO STRIPE.
//
// An affiliate's code is a real Stripe promotion code, so buyers can type it
// at checkout (app or website) and Stripe applies the discount. Creating or
// switching one needs the Stripe API, and the Stripe key must never reach a
// browser - so the manager page asks this function, and this function asks
// Stripe. Everything that only touches the database (commission, payouts,
// sales) goes through plain database functions instead.
//
//   POST { action: "create", name, code, contact?, discount_percent,
//          discount_forever, commission_percent, commission_recurring }
//   POST { action: "set_active", id, active }
//
// WHO MAY: the caller's own Supabase session (Authorization: Bearer ...) must
// belong to a manager - checked here against user_roles, the same table
// is_manager() reads.
//
// Environment (Supabase -> Edge Functions -> Secrets):
//   STRIPE_API_KEY   a RESTRICTED key (rk_live_...) with write access to
//                    Coupons and Promotion codes - nothing else
//   SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY   provided by Supabase itself
//
// JWT verification stays ON (the default): only signed-in users get through.

import { createClient } from "jsr:@supabase/supabase-js@2";

// Pinned, so a change of the account's default API version cannot change the
// shape of these calls under us.
const STRIPE_VERSION = "2024-06-20";

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
  "Access-Control-Allow-Headers": "authorization, apikey, content-type, x-client-info",
};
const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { ...CORS, "Content-Type": "application/json" } });

async function stripe(path: string, params: Record<string, string>) {
  const key = Deno.env.get("STRIPE_API_KEY")?.trim();
  if (!key) throw new Error("STRIPE_API_KEY is not set");
  const r = await fetch("https://api.stripe.com/v1/" + path, {
    method: "POST",
    headers: {
      Authorization: "Bearer " + key,
      "Stripe-Version": STRIPE_VERSION,
      "Content-Type": "application/x-www-form-urlencoded",
    },
    body: new URLSearchParams(params),
  });
  const j = await r.json();
  if (!r.ok) throw Object.assign(new Error(j?.error?.message ?? `stripe ${r.status}`), { stripe: true });
  return j;
}

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
  if (req.method !== "POST") return json({ error: "method not allowed" }, 405);

  const db = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
    { auth: { persistSession: false } },
  );

  // ---- is the caller a manager? ----
  const jwt = (req.headers.get("authorization") ?? "").replace(/^Bearer\s+/i, "");
  const { data: who } = await db.auth.getUser(jwt);
  if (!who?.user) return json({ error: "not signed in" }, 401);
  const { data: role } = await db.from("user_roles")
    .select("role").eq("user_id", who.user.id).eq("role", "manager").maybeSingle();
  if (!role) return json({ error: "not allowed" }, 403);

  let body: any;
  try { body = await req.json(); } catch { return json({ error: "bad json" }, 400); }

  try {
    if (body.action === "create") {
      const code = String(body.code ?? "").trim().toUpperCase();
      const name = String(body.name ?? "").trim();
      const discount = Math.round(Number(body.discount_percent));
      const commission = Math.round(Number(body.commission_percent) * 100) / 100;
      if (!/^[A-Z0-9]{3,20}$/.test(code)) return json({ error: "code: 3-20 letters or digits" }, 400);
      if (!name) return json({ error: "name missing" }, 400);
      if (!(discount >= 1 && discount <= 100)) return json({ error: "discount: 1-100" }, 400);
      if (!(commission >= 0 && commission <= 100)) return json({ error: "commission: 0-100" }, 400);

      const { data: taken } = await db.from("affiliates").select("id").eq("code", code).maybeSingle();
      if (taken) return json({ error: "code already exists" }, 409);

      // "once" covers a one-time payment fully; for the monthly plan it means
      // the first month only. "forever" keeps the discount on every renewal.
      const coupon = await stripe("coupons", {
        percent_off: String(discount),
        duration: body.discount_forever ? "forever" : "once",
        name: `${code} (${discount} %)`,
        "metadata[affiliate_code]": code,
      });
      // If this fails (e.g. Stripe already has an active code with that
      // name), the coupon above stays unused - harmless, nobody can apply it.
      const promo = await stripe("promotion_codes", {
        coupon: coupon.id,
        code,
        "metadata[affiliate_code]": code,
      });

      const { data, error } = await db.from("affiliates").insert({
        code, name,
        contact: String(body.contact ?? "").trim() || null,
        discount_percent: discount,
        discount_forever: !!body.discount_forever,
        commission_percent: commission,
        commission_recurring: !!body.commission_recurring,
        stripe_coupon_id: coupon.id,
        stripe_promo_id: promo.id,
      }).select("id").single();
      if (error) {
        // Do not leave a live code in Stripe that nothing counts.
        await stripe(`promotion_codes/${promo.id}`, { active: "false" }).catch(() => {});
        throw error;
      }
      console.log(`affiliate created: ${code} promo=${promo.id}`);
      return json({ ok: true, id: data.id, code });
    }

    if (body.action === "set_active") {
      const { data: a } = await db.from("affiliates").select("id, stripe_promo_id").eq("id", body.id).maybeSingle();
      if (!a) return json({ error: "not found" }, 404);
      const active = !!body.active;
      if (a.stripe_promo_id) await stripe(`promotion_codes/${a.stripe_promo_id}`, { active: String(active) });
      const { error } = await db.from("affiliates").update({ active }).eq("id", a.id);
      if (error) throw error;
      return json({ ok: true });
    }

    return json({ error: "unknown action" }, 400);
  } catch (e: any) {
    console.error(e);
    return json({ error: e?.stripe ? "stripe: " + e.message : "failed" }, e?.stripe ? 502 : 500);
  }
});
