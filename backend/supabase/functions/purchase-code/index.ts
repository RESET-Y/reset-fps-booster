// THE THANK-YOU PAGE ASKS: WHAT DID THIS CHECKOUT BUY?
//
// After paying, Stripe sends the buyer to site/danke.html with
// ?session_id={CHECKOUT_SESSION_ID}. That page calls this function with the
// session id and gets back one of:
//
//   { status: "code", plan, code }   bought on the website - here is the code
//   { status: "account", plan }      bought in the app - already on the account
//   { status: "pending" }            the webhook has not got there yet, ask again
//
// The session id is the only key. Stripe makes it long and random
// (cs_live_ + ~60 characters) and only the buyer's browser is sent it, so
// knowing it is proof enough of having made that checkout. Nothing is
// listed, searched or written here - one exact lookup, read only.
//
// Environment: SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY (provided by Supabase).
// Deploy with JWT verification OFF (see ../../config.toml): the website has
// no Supabase session to send.

import { createClient } from "jsr:@supabase/supabase-js@2";

const SESSION = /^cs_(live|test)_[A-Za-z0-9]{10,200}$/;

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, OPTIONS",
  "Access-Control-Allow-Headers": "content-type",
};

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { ...CORS, "Content-Type": "application/json", "Cache-Control": "no-store" },
  });

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
  if (req.method !== "GET") return json({ error: "method not allowed" }, 405);

  const session = new URL(req.url).searchParams.get("session_id") ?? "";
  if (!SESSION.test(session)) return json({ error: "bad session id" }, 400);

  const db = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
    { auth: { persistSession: false } },
  );

  const { data, error } = await db.from("purchases")
    .select("plan, user_id, code").eq("session_id", session).maybeSingle();
  if (error) {
    console.error(error);
    return json({ error: "lookup failed" }, 500);
  }

  if (!data) return json({ status: "pending" });
  if (data.code) return json({ status: "code", plan: data.plan, code: data.code });
  return json({ status: "account", plan: data.plan });
});
