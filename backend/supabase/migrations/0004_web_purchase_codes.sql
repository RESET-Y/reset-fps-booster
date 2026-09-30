-- BUYING ON THE WEBSITE: THE PURCHASE BECOMES A CODE.
--
-- A purchase from the app carries the user's id (client_reference_id) and
-- lands on that account directly. A purchase from the website has no account
-- behind it, so the webhook turns it into a premium code instead. The buyer
-- sees the code on the website right after paying and redeems it in the app.
--
--   purchases        every completed checkout, keyed by Stripe's session id.
--                    The thank-you page looks its session up here: either
--                    "on your account" (app) or "here is your code" (web).
--   premium_codes    gains stripe_ref / valid_until, so a bought code keeps
--                    following its payment: a cancelled subscription or a
--                    refund ends the code, before or after it is redeemed.
--
-- A bought code redeems into an entitlement with source 'stripe' and the same
-- external_ref (pi_... or sub_...) the webhook uses for app purchases, so
-- renewals, cancellations and refunds find it exactly the same way.

-- ---------------------------------------------------------------------------
-- codes that come from a payment
-- ---------------------------------------------------------------------------
-- Bought codes are made by the webhook, not by a manager.
alter table public.premium_codes alter column created_by drop not null;
alter table public.premium_codes add column if not exists stripe_ref  text;         -- pi_... or sub_...
alter table public.premium_codes add column if not exists valid_until timestamptz;  -- subscription: end of what is paid
create index if not exists premium_codes_stripe_ref_idx on public.premium_codes (stripe_ref);

-- ---------------------------------------------------------------------------
-- purchases
-- ---------------------------------------------------------------------------
create table if not exists public.purchases (
    session_id text primary key,                              -- cs_live_... / cs_test_...
    plan       text not null check (plan in ('monthly', 'lifetime')),
    user_id    uuid references auth.users (id) on delete set null, -- bought in the app
    code       text references public.premium_codes (code),        -- bought on the website
    created_at timestamptz not null default now()
);
alter table public.purchases enable row level security;
-- No policies: only the webhook and the thank-you function (both service_role) touch it.

-- ---------------------------------------------------------------------------
-- webhook: one code per website checkout
-- ---------------------------------------------------------------------------
-- Idempotent on the session: Stripe delivers events at least once, and a
-- retried delivery must hand back the same code, never mint a second one.
create or replace function public.issue_purchase_code(
    p_session     text,
    p_plan        text,
    p_stripe_ref  text,
    p_valid_until timestamptz,
    p_note        text)
returns text
language plpgsql
security definer
set search_path = public
as $$
declare
    c text;
begin
    select code into c from public.purchases where session_id = p_session;
    if c is not null then return c; end if;

    loop
        c := public.new_code_text();
        begin
            insert into public.premium_codes (code, created_by, duration_days, max_uses, note, stripe_ref, valid_until)
            values (c, null, null, 1, p_note, p_stripe_ref, p_valid_until);
            exit;
        exception when unique_violation then
            -- next attempt
        end;
    end loop;

    insert into public.purchases (session_id, plan, code) values (p_session, p_plan, c);
    return c;
end;
$$;
revoke all on function public.issue_purchase_code(text, text, text, timestamptz, text) from public, anon, authenticated;
grant execute on function public.issue_purchase_code(text, text, text, timestamptz, text) to service_role;

-- ---------------------------------------------------------------------------
-- redeem: bought codes follow their payment
-- ---------------------------------------------------------------------------
-- Same status words as before, so the app needs no change:
--   ok, not_found, inactive, used_up, already_redeemed
-- A bought code whose subscription has ended reads as 'inactive'.
create or replace function public.redeem_premium_code(p_code text)
returns text
language plpgsql
security definer
set search_path = public
as $$
declare
    uid uuid := auth.uid();
    c public.premium_codes%rowtype;
    normalized text := upper(regexp_replace(coalesce(p_code, ''), '\s', '', 'g'));
begin
    if uid is null then
        raise exception 'not signed in' using errcode = '42501';
    end if;

    -- Locked for the rest of the transaction, so two people redeeming the
    -- last use of a code at the same moment cannot both get it.
    select * into c from public.premium_codes where code = normalized for update;
    if not found then return 'not_found'; end if;
    if not c.active then return 'inactive'; end if;
    if c.valid_until is not null and c.valid_until <= now() then return 'inactive'; end if;
    if c.uses >= c.max_uses then return 'used_up'; end if;
    if exists (select 1 from public.code_redemptions where code = c.code and user_id = uid) then
        return 'already_redeemed';
    end if;

    insert into public.code_redemptions (code, user_id) values (c.code, uid);
    update public.premium_codes set uses = uses + 1 where code = c.code;

    if c.stripe_ref is not null then
        -- Bought: from here on it is an ordinary Stripe entitlement.
        insert into public.entitlements (user_id, product, source, external_ref, valid_until)
        values (uid, 'premium', 'stripe', c.stripe_ref, c.valid_until)
        on conflict (user_id, product, source, external_ref)
            do update set valid_until = excluded.valid_until;
    else
        insert into public.entitlements (user_id, product, source, external_ref, valid_until)
        values (uid, 'premium', 'code', c.code,
                case when c.duration_days is null then null
                     else now() + make_interval(days => c.duration_days) end)
        on conflict (user_id, product, source, external_ref) do nothing;
    end if;

    return 'ok';
end;
$$;
grant execute on function public.redeem_premium_code(text) to authenticated;
