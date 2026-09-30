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
// BOUGHT ON THE WEBSITE: no client_reference_id, because nobody is signed in
// there. Such a checkout becomes a premium code (issue_purchase_code, see
// migration 0004), shown to the buyer by the purchase-code function on the
// thank-you page and redeemed in the app. The code carries the same pi_/sub_
// reference, so renewals, cancellations and refunds keep it in step with the
// payment - before it is redeemed on the code, afterwards on the entitlement.
// The buyer's e-mail is kept with it (migration 0005), so a lost code shows
// up again on the website's account page once they sign in with that e-mail.
//
// AFFILIATES: a checkout paid with an affiliate's promotion code is counted
// for them (affiliate_sales, migration 0006), with the commission fixed from
// the net amount at that moment. Renewals of such a subscription count too
// when the affiliate gets recurring commission (invoice.paid). A full refund
// of a one-time payment takes its sale back out.
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

  // Counted once per payment: the id is the checkout session or invoice, so a
  // retried delivery hits the primary key and changes nothing.
  const countSale = async (affiliate: any, id: string, plan: string, kind: string, ref: string, netCents: number) => {
    const commission = Math.round(netCents * Number(affiliate.commission_percent) / 100);
    const { error } = await db.from("affiliate_sales").upsert({
      id, affiliate_id: affiliate.id, plan, kind, stripe_ref: ref,
      net_cents: netCents, commission_cents: commission, currency: obj.currency ?? "eur",
    }, { onConflict: "id", ignoreDuplicates: true });
    if (!error) console.log(`affiliate sale: ${affiliate.code} ${kind} ${plan} net=${netCents} commission=${commission}`);
    return error;
  };

  switch (event?.type) {
    // ---- A COMPLETED CHECKOUT: lifetime payment or a new subscription -------
    case "checkout.session.completed": {
      if (obj.payment_status !== "paid" && obj.payment_status !== "no_payment_required") break;

      let plan: "monthly" | "lifetime";
      let ref: string;
      let validUntil: string | null;
      if (obj.mode === "subscription" && obj.subscription) {
        // Valid for a first period until the subscription event brings the
        // exact end; a generous placeholder is fine because
        // customer.subscription.created/updated overwrites it within seconds.
        plan = "monthly";
        ref = `sub_${obj.subscription}`;
        validUntil = iso(Date.now() / 1000 + 35 * 24 * 60 * 60);
      } else if (obj.mode === "payment") {
        plan = "lifetime";
        ref = `pi_${obj.payment_intent ?? obj.id}`;
        validUntil = null;
      } else {
        break;
      }

      const userId: string | undefined = obj.client_reference_id;
      if (userId && UUID.test(userId)) {
        // Bought in the app: straight onto the account.
        const { error } = await upsert(userId, ref, validUntil);
        if (error) return fail(error);
        const { error: logError } = await db.from("purchases")
          .upsert({ session_id: obj.id, plan, user_id: userId }, { onConflict: "session_id", ignoreDuplicates: true });
        if (logError) return fail(logError);
        console.log(`${plan} granted: user=${userId} ref=${ref}`);
      } else {
        // Bought on the website: a code for the buyer to redeem in the app.
        const email = obj.customer_details?.email ?? "?";
        const { data: code, error } = await db.rpc("issue_purchase_code", {
          p_session: obj.id,
          p_plan: plan,
          p_stripe_ref: ref,
          p_valid_until: validUntil,
          p_note: `Website ${plan} · ${email}`,
          p_email: obj.customer_details?.email ?? null,
        });
        if (error) return fail(error);
        console.log(`${plan} code issued: code=${code} ref=${ref} session=${obj.id}`);
      }

      // Paid with an affiliate's code? discounts[].promotion_code is the id
      // (or, if ever expanded, the object).
      const promoIds = (obj.discounts ?? [])
        .map((d: any) => typeof d.promotion_code === "string" ? d.promotion_code : d.promotion_code?.id)
        .filter(Boolean);
      if (promoIds.length) {
        const { data: affiliate, error: affError } = await db.from("affiliates")
          .select("id, code, commission_percent").in("stripe_promo_id", promoIds).limit(1).maybeSingle();
        if (affError) return fail(affError);
        if (affiliate) {
          const net = (obj.amount_total ?? 0) - (obj.total_details?.amount_tax ?? 0);
          const saleError = await countSale(affiliate, obj.id, plan, "first", ref, net);
          if (saleError) return fail(saleError);
        }
      }
      break;
    }

    // ---- A SUBSCRIPTION RENEWAL: recurring affiliate commission --------------
    case "invoice.paid": {
      // The first invoice is already counted with its checkout.
      if (obj.billing_reason !== "subscription_cycle") break;
      const subId: string | undefined = obj.subscription ?? obj.parent?.subscription_details?.subscription;
      if (!subId) break;
      const { data: first, error: firstError } = await db.from("affiliate_sales")
        .select("affiliates(id, code, commission_percent, commission_recurring)")
        .eq("stripe_ref", `sub_${subId}`).eq("kind", "first").limit(1).maybeSingle();
      if (firstError) return fail(firstError);
      const affiliate: any = (first as any)?.affiliates;
      if (!affiliate?.commission_recurring) break;
      const tax = typeof obj.tax === "number" ? obj.tax
        : (obj.total_taxes ?? []).reduce((sum: number, t: any) => sum + (t.amount ?? 0), 0);
      const saleError = await countSale(affiliate, obj.id, "monthly", "renewal", `sub_${subId}`, (obj.amount_paid ?? 0) - tax);
      if (saleError) return fail(saleError);
      break;
    }

    // ---- SUBSCRIPTION LIFECYCLE: valid until the end of what is paid for -----
    case "customer.subscription.created":
    case "customer.subscription.updated":
    case "customer.subscription.deleted": {
      const subId: string = obj.id;

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

      // A code bought on the website follows its subscription too, so an
      // unredeemed code for a cancelled subscription stops working.
      const { data: codes, error: codeError } = await db.from("premium_codes")
        .update({ valid_until: validUntil }).eq("stripe_ref", `sub_${subId}`).select("code");
      if (codeError) return fail(codeError);

      const userId = await userForSubscription(subId);
      if (!userId) {
        // Either a website code not redeemed yet (updated above), or this
        // arrived before checkout.session.completed - that event creates the
        // row, and the next update sets the exact date.
        console.log(`subscription ${event.type}: sub=${subId} no account yet, codes updated=${codes?.length ?? 0}`);
        break;
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
      // A website code for this payment stops working, redeemed or not.
      const { data: codes, error: codeError } = await db.from("premium_codes")
        .update({ active: false }).eq("stripe_ref", ref).select("code");
      if (codeError) return fail(codeError);
      // No commission on money that went back.
      const { error: saleError } = await db.from("affiliate_sales").update({ reversed: true }).eq("stripe_ref", ref);
      if (saleError) return fail(saleError);
      console.log(`refund: ref=${ref} rows=${data?.length ?? 0} codes=${codes?.length ?? 0}`);
      break;
    }

    default:
      // Other events are fine to ignore; acknowledged so they are not resent.
      break;
  }

  return new Response("ok", { status: 200 });
});
