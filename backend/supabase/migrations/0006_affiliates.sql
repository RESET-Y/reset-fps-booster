-- AFFILIATES: STREAMERS WITH THEIR OWN CODE AND A SHARE OF WHAT IT SELLS.
--
-- Each affiliate is a Stripe promotion code (e.g. MONTE10) that gives buyers a
-- discount. Whoever pays with it - on the website or in the app - is counted
-- for that affiliate, and a commission is worked out on what was actually
-- paid, VAT excluded. Everything is set per affiliate on the manager page:
--
--   discount_percent       what the buyer saves
--   discount_forever       monthly plan: discount every month, or the first only
--   commission_percent     the affiliate's share of the net amount
--   commission_recurring   monthly plan: commission on every renewal, or the
--                          first payment only
--
--   affiliates         one row per affiliate, created by the affiliate-admin
--                      function (it also creates the code in Stripe)
--   affiliate_sales    one row per counted payment, written by the webhook.
--                      The commission is fixed when the sale happens, so
--                      changing the percentage later never rewrites the past.
--   affiliate_payouts  what has been paid out, entered by hand
--
-- None of this is readable from outside; the manager reads it through the
-- functions below, which check is_manager() first.

create table if not exists public.affiliates (
    id                   bigint generated always as identity primary key,
    code                 text not null unique check (code ~ '^[A-Z0-9]{3,20}$'),
    name                 text not null,
    contact              text,
    discount_percent     int not null check (discount_percent between 1 and 100),
    discount_forever     boolean not null default false,
    commission_percent   numeric(5,2) not null check (commission_percent between 0 and 100),
    commission_recurring boolean not null default false,
    active               boolean not null default true,
    stripe_coupon_id     text,
    stripe_promo_id      text unique,
    created_at           timestamptz not null default now()
);
alter table public.affiliates enable row level security;

create table if not exists public.affiliate_sales (
    id               text primary key,          -- checkout session or invoice id: a retried event cannot count twice
    affiliate_id     bigint not null references public.affiliates (id),
    plan             text not null check (plan in ('monthly', 'lifetime')),
    kind             text not null check (kind in ('first', 'renewal')),
    stripe_ref       text,                      -- pi_... or sub_..., as in entitlements
    net_cents        int not null,              -- paid, VAT excluded
    commission_cents int not null,
    currency         text not null default 'eur',
    reversed         boolean not null default false,  -- refunded: no commission
    created_at       timestamptz not null default now()
);
create index if not exists affiliate_sales_affiliate_idx on public.affiliate_sales (affiliate_id);
create index if not exists affiliate_sales_ref_idx on public.affiliate_sales (stripe_ref);
alter table public.affiliate_sales enable row level security;

create table if not exists public.affiliate_payouts (
    id           bigint generated always as identity primary key,
    affiliate_id bigint not null references public.affiliates (id),
    amount_cents int not null check (amount_cents > 0),
    note         text,
    paid_at      timestamptz not null default now()
);
alter table public.affiliate_payouts enable row level security;
-- No policies on any of the three: only the functions below and the
-- service_role (webhook, affiliate-admin) touch them.

-- ---------------------------------------------------------------------------
-- public: what a referral code offers, for the banner on the website
-- ---------------------------------------------------------------------------
-- Only the discount of an ACTIVE code - no name, no numbers. The code itself
-- is already public: streamers say it on air.
create or replace function public.affiliate_offer(p_code text)
returns json
language sql
stable
security definer
set search_path = public
as $$
    select json_build_object('code', a.code, 'discount_percent', a.discount_percent, 'discount_forever', a.discount_forever)
    from public.affiliates a
    where a.code = upper(trim(p_code)) and a.active;
$$;
grant execute on function public.affiliate_offer(text) to anon, authenticated;

-- ---------------------------------------------------------------------------
-- manager: overview, sales, changes, payouts
-- ---------------------------------------------------------------------------
create or replace function public.list_affiliates()
returns table (id bigint, code text, name text, contact text,
               discount_percent int, discount_forever boolean,
               commission_percent numeric, commission_recurring boolean,
               active boolean, created_at timestamptz,
               sales int, net_cents bigint, commission_cents bigint, paid_cents bigint)
language plpgsql
stable
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    return query
        select a.id, a.code, a.name, a.contact,
               a.discount_percent, a.discount_forever,
               a.commission_percent, a.commission_recurring,
               a.active, a.created_at,
               (select count(*)::int from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed),
               (select coalesce(sum(s.net_cents), 0) from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed),
               (select coalesce(sum(s.commission_cents), 0) from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed),
               (select coalesce(sum(p.amount_cents), 0) from public.affiliate_payouts p where p.affiliate_id = a.id)
        from public.affiliates a
        order by a.created_at desc;
end;
$$;
grant execute on function public.list_affiliates() to authenticated;

create or replace function public.list_affiliate_activity(p_affiliate bigint)
returns json
language plpgsql
stable
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    return json_build_object(
        'sales', coalesce((select json_agg(s order by s.created_at desc) from (
            select id, plan, kind, net_cents, commission_cents, reversed, created_at
            from public.affiliate_sales where affiliate_id = p_affiliate) s), '[]'::json),
        'payouts', coalesce((select json_agg(p order by p.paid_at desc) from (
            select id, amount_cents, note, paid_at
            from public.affiliate_payouts where affiliate_id = p_affiliate) p), '[]'::json));
end;
$$;
grant execute on function public.list_affiliate_activity(bigint) to authenticated;

-- Name, contact and commission can change at any time; they apply to sales
-- from now on. The discount lives in Stripe and is fixed with the code - for
-- a different discount, create a new code.
create or replace function public.update_affiliate(
    p_id bigint, p_name text, p_contact text,
    p_commission_percent numeric, p_commission_recurring boolean)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    update public.affiliates
       set name = coalesce(nullif(trim(p_name), ''), name),
           contact = nullif(trim(p_contact), ''),
           commission_percent = p_commission_percent,
           commission_recurring = p_commission_recurring
     where id = p_id;
end;
$$;
grant execute on function public.update_affiliate(bigint, text, text, numeric, boolean) to authenticated;

-- For refunds the webhook cannot tie to a sale (a refunded subscription
-- payment): take a sale out of the commission by hand, or put it back.
create or replace function public.set_affiliate_sale_reversed(p_sale text, p_reversed boolean)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    update public.affiliate_sales set reversed = p_reversed where id = p_sale;
end;
$$;
grant execute on function public.set_affiliate_sale_reversed(text, boolean) to authenticated;

create or replace function public.add_affiliate_payout(p_affiliate bigint, p_amount_cents int, p_note text)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    insert into public.affiliate_payouts (affiliate_id, amount_cents, note)
    values (p_affiliate, p_amount_cents, nullif(trim(p_note), ''));
end;
$$;
grant execute on function public.add_affiliate_payout(bigint, int, text) to authenticated;

create or replace function public.delete_affiliate_payout(p_payout bigint)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    delete from public.affiliate_payouts where id = p_payout;
end;
$$;
grant execute on function public.delete_affiliate_payout(bigint) to authenticated;
