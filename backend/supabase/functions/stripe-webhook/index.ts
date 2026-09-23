// STRIPE -> PREMIUM.
//
// Stripe calls this after every checkout, renewal, cancellation and refund.
// It is the ONLY thing that writes to public.entitlements, and it can only do
// so because it runs with the service_role key - which Supabase hands Edge
// Functions as an environment variable, so it never touches the app.
//
// Payments go through Stripe MANAGED PAYMENTS, Stripe's merchant-of-record
// product: Stripe is the seller of record and handles VAT/sales tax, fraud,
// disputes and customer support. That is why purchases arrive here through
// Payment Links / Checkout only - the only integrations it supports.
//
// NOTHING IS TRUSTED UNTIL THE SIGNATURE CHECKS OUT. Stripe signs
// "<timestamp>.<raw body>" with HMAC-SHA256 using the endpoint's signing
// secret (whsec_...). Without that check, anyone who found this URL could post
// a fake checkout and give themselves premium. The timestamp is checked too,
// so a captured request cannot be replayed later.
//
// WHOSE PURCHASE: the app opens the Payment Link with ?client_reference_id=
// set to the signed-in user's id, and Stripe hands it back on
// checkout.session.completed. Later subscription events carry no such field,
// so the subscription id is stored as external_ref on that first event and
// every later event finds its user through it.
//
// Environment (Supabase -> Edge Functions -> Secrets):
//   STRIPE_WEBHOOK_SECRET        whsec_... from the webhook endpoint in Stripe
//   SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY   provided by Supabase itself
//
// Deploy with JWT verification OFF (see ../../config.toml): Stripe does not
// send a Supabase token. The signature check is what protects this endpoint.

import { createClient } from "jsr:@supabase/supabase-js@2";

const PRODUCT = "premium";
const SOURCE = "stripe";

// A renewal can land a little after the period ends. Premium is kept this long
// past current_period_end so a paying user is never locked out by a late event.
const RENEWAL_GRACE_S = 3 * 24 * 60 * 60;

// Stripe's own default tolerance for how old a signed request may be.
const SIGNATURE_TOLERANCE_S = 5 * 60;

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

async function hmacHex(secret: string, message: string) {
  const key = await crypto.subtle.importKey(
    "raw", new TextEncoder().encode(secret),
    { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const mac = new Uint8Array(await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(message)));
  return Array.from(mac, (b) => b.toString(16).padStart(2, "0")).join("");
}

// Constant time: stopping at the first differing byte would leak, through
// timing, how much of a forged signature was right.
function safeEqual(a: string, b: string) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// Returns null when the signature is good, otherwise the reason - logged, so a
// rejection says WHY without ever printing the secret itself.
async function signatureProblem(rawBody: string, header: string | null, secret: string): Promise<string | null> {
  if (!header) return "no Stripe-Signature header";
  let timestamp = "";
  const candidates: string[] = [];
  for (const part of header.split(",")) {
    const [k, v] = part.split("=", 2);
    if (k === "t") timestamp = v;
    if (k === "v1") candidates.push(v);
  }
  if (!timestamp || candidates.length === 0) return "header without t= or v1=";

  const age = Math.abs(Date.now() / 1000 - Number(timestamp));
  if (!Number.isFinite(age) || age > SIGNATURE_TOLERANCE_S) return `timestamp too old or invalid (age ${Math.round(age)} s)`;

  const expected = await hmacHex(secret, `${timestamp}.${rawBody}`);
  if (candidates.some((c) => safeEqual(expected, c))) return null;
  return `no v1 matched (secret starts with whsec_: ${secret.startsWith("whsec_")}, length ${secret.length})`;
}

const iso = (unixSeconds: number) => new Date(unixSeconds * 1000).toISOString();

Deno.serve(async (req) => {
  if (req.method !== "POST") return new Response("method not allowed", { status: 405 });

  // Trimmed: the Supabase secrets field accepts multi-line values, and a
  // pasted secret can carry a trailing newline or space. One invisible
  // character there made every genuine Stripe delivery fail as "invalid
  // signature" in the first end-to-end test.
  const secret = Deno.env.get("STRIPE_WEBHOOK_SECRET")?.trim();
  if (!secret) {
    console.error("STRIPE_WEBHOOK_SECRET is not set");
    return new Response("not configured", { status: 500 });
  }

  const raw = await req.text();
  const problem = await signatureProblem(raw, req.headers.get("stripe-signature"), secret);
  if (problem) {
    console.warn(`rejected: ${problem}`);
    return new Response("invalid signature", { status: 400 });
  }

  let event: any;
  try { event = JSON.parse(raw); } catch { return new Response("bad json", { status: 400 }); }
  const obj = event?.data?.object ?? {};

  const db = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
    { auth: { persistSession: false } },
  );

  const upsert = (userId: string, externalRef: string, validUntil: string | null) =>
    db.from("entitlements").upsert(
      { user_id: userId, product: PRODUCT, source: SOURCE, external_ref: externalRef, valid_until: validUntil },
      { onConflict: "user_id,product,source,external_ref" },
    );

  // Later subscription events carry no user id; find it through the
  // subscription id stored on the first event.
  const userForSubscription = async (subId: string) => {
    const { data } = await db.from("entitlements")
      .select("user_id").eq("source", SOURCE).eq("external_ref", `sub_${subId}`)
      .limit(1).maybeSingle();
    return data?.user_id as string | undefined;
  };

  const fail = (what: unknown) => { console.error(what); return new Response("db error", { status: 500 }); };

  switch (event?.type) {
    // ---- A COMPLETED CHECKOUT: lifetime payment or a new subscription -------
    case "checkout.session.completed": {
      const userId: string | undefined = obj.client_reference_id;
      if (!userId || !UUID.test(userId)) {
        // Acknowledged, so Stripe does not retry forever - retrying cannot add
        // the missing id. Logged with what is needed to assign it by hand.
        console.error(`UNMATCHED PURCHASE: session=${obj.id} email=${obj.customer_details?.email ?? "?"} - no client_reference_id`);
        break;
      }
      if (obj.payment_status !== "paid" && obj.payment_status !== "no_payment_required") break;

      if (obj.mode === "subscription" && obj.subscription) {
        // Valid for a first period until the subscription event brings the
        // exact end; a generous placeholder is fine because
        // customer.subscription.created/updated overwrites it within seconds.
        const provisional = iso(Date.now() / 1000 + 35 * 24 * 60 * 60);
        const { error } = await upsert(userId, `sub_${obj.subscription}`, provisional);
        if (error) return fail(error);
        console.log(`subscription started: user=${userId} sub=${obj.subscription}`);
      } else if (obj.mode === "payment") {
        const ref = `pi_${obj.payment_intent ?? obj.id}`;
        const { error } = await upsert(userId, ref, null);
        if (error) return fail(error);
        console.log(`lifetime granted: user=${userId} ref=${ref}`);
      }
      break;
    }

    // ---- SUBSCRIPTION LIFECYCLE: valid until the end of what is paid for -----
    case "customer.subscription.created":
    case "customer.subscription.updated":
    case "customer.subscription.deleted": {
      const subId: string = obj.id;
      const userId = await userForSubscription(subId);
      if (!userId) {
        // Can arrive before checkout.session.completed; that event will create
        // the row, and the next update will set the exact date.
        console.log(`subscription ${event.type} for unknown sub=${subId} - waiting for checkout`);
        break;
      }

      let validUntil: string;
      const status: string = obj.status ?? "";
      if (event.type === "customer.subscription.deleted" || status === "canceled" || status === "unpaid" || status === "incomplete_expired") {
        // Over. If it was cancelled at period end, Stripe only sends "deleted"
        // once that end is reached - so paid-up time was already honoured.
        validUntil = new Date().toISOString();
      } else {
        const end = obj.current_period_end ?? obj.items?.data?.[0]?.current_period_end;
        if (!end) break;
        validUntil = iso(end + RENEWAL_GRACE_S);
      }

      const { error } = await upsert(userId, `sub_${subId}`, validUntil);
      if (error) return fail(error);
      console.log(`subscription ${event.type}: user=${userId} sub=${subId} status=${status} until=${validUntil}`);
      break;
    }

    // ---- REFUND OF A ONE-TIME PAYMENT: lifetime ends -------------------------
    case "charge.refunded": {
      if (!obj.refunded || !obj.payment_intent) break; // partial refunds keep premium
      const ref = `pi_${obj.payment_intent}`;
      // Ended rather than deleted, so the history of what happened stays.
      const { data, error } = await db.from("entitlements")
        .update({ valid_until: new Date().toISOString() })
        .eq("source", SOURCE).eq("external_ref", ref).select("user_id");
      if (error) return fail(error);
      console.log(`refund: ref=${ref} rows=${data?.length ?? 0}`);
      break;
    }

    default:
      // Other events are fine to ignore; acknowledged so they are not resent.
      break;
  }

  return new Response("ok", { status: 200 });
});
