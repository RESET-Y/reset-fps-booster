-- PARTNER CONTRACT AND APPLICATIONS
--
-- 1. The partner contract. A partner accepts the current contract version
--    before the partner area shows any numbers. A new version means a new
--    acceptance. The accepted versions are kept as proof, with the time.
-- 2. Applications from the website (partner/bewerben.html). Anyone can send
--    one; the manager reads and answers them on the manager page. Each new
--    application is e-mailed to the owner by the partner-application-notify
--    function (called from the trigger at the bottom).

-- ---------------------------------------------------------------------------
-- The contract: which version is current, and who accepted which one
-- ---------------------------------------------------------------------------
-- Changing the contract text means changing this version string. Every partner
-- then sees the new text and has to accept it again.
create or replace function public.partner_contract_version()
returns text
language sql
immutable
as $$ select 'v1-2026-10'::text $$;

create table if not exists public.partner_contract_acceptances (
    user_id     uuid not null references auth.users (id) on delete cascade,
    version     text not null,
    accepted_at timestamptz not null default now(),
    primary key (user_id, version)
);
alter table public.partner_contract_acceptances enable row level security;
-- No policies: only the functions below touch it.

-- The caller's state: is an account linked to a code, which version is
-- current, and has the caller accepted it.
create or replace function public.partner_contract_state()
returns json
language sql
stable
security definer
set search_path = public
as $$
    select json_build_object(
        'linked', exists (select 1 from public.affiliates a where a.user_id = auth.uid()),
        'version', public.partner_contract_version(),
        'accepted', exists (select 1 from public.partner_contract_acceptances c
                            where c.user_id = auth.uid() and c.version = public.partner_contract_version()));
$$;
revoke all on function public.partner_contract_state() from public, anon;
grant execute on function public.partner_contract_state() to authenticated;

create or replace function public.accept_partner_contract(p_version text)
returns boolean
language plpgsql
security definer
set search_path = public
as $$
begin
    if auth.uid() is null then raise exception 'not allowed' using errcode = '42501'; end if;
    -- An old version (the text changed meanwhile) is not accepted.
    if p_version is distinct from public.partner_contract_version() then return false; end if;
    insert into public.partner_contract_acceptances (user_id, version)
        values (auth.uid(), p_version)
        on conflict do nothing;
    return true;
end;
$$;
revoke all on function public.accept_partner_contract(text) from public, anon;
grant execute on function public.accept_partner_contract(text) to authenticated;

-- ---------------------------------------------------------------------------
-- The numbers are only for partners who accepted the current contract.
-- Same functions as in migration 0010, with that condition added.
-- ---------------------------------------------------------------------------
create or replace function public.my_affiliates()
returns table (id bigint, code text, discount_percent int, discount_forever boolean,
               commission_percent numeric, commission_recurring boolean, active boolean,
               sales int, net_cents bigint, commission_cents bigint, paid_cents bigint,
               month_sales int, month_net_cents bigint, month_commission_cents bigint)
language sql
stable
security definer
set search_path = public
as $$
    select a.id, a.code, a.discount_percent, a.discount_forever,
           a.commission_percent, a.commission_recurring, a.active,
           (select count(*)::int from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed),
           (select coalesce(sum(s.net_cents), 0) from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed),
           (select coalesce(sum(s.commission_cents), 0) from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed),
           (select coalesce(sum(p.amount_cents), 0) from public.affiliate_payouts p where p.affiliate_id = a.id),
           (select count(*)::int from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed
                and s.created_at >= date_trunc('month', now() at time zone 'Europe/Berlin') at time zone 'Europe/Berlin'),
           (select coalesce(sum(s.net_cents), 0) from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed
                and s.created_at >= date_trunc('month', now() at time zone 'Europe/Berlin') at time zone 'Europe/Berlin'),
           (select coalesce(sum(s.commission_cents), 0) from public.affiliate_sales s where s.affiliate_id = a.id and not s.reversed
                and s.created_at >= date_trunc('month', now() at time zone 'Europe/Berlin') at time zone 'Europe/Berlin')
    from public.affiliates a
    where a.user_id = auth.uid()
      and exists (select 1 from public.partner_contract_acceptances c
                  where c.user_id = auth.uid() and c.version = public.partner_contract_version())
    order by a.created_at;
$$;

create or replace function public.my_affiliate_activity(p_affiliate bigint)
returns json
language sql
stable
security definer
set search_path = public
as $$
    select case when exists (
        select 1 from public.affiliates a
        where a.id = p_affiliate and a.user_id = auth.uid()
          and exists (select 1 from public.partner_contract_acceptances c
                      where c.user_id = auth.uid() and c.version = public.partner_contract_version()))
    then json_build_object(
        'sales', coalesce((select json_agg(s order by s.created_at desc) from (
            select plan, kind, net_cents, commission_cents, reversed, created_at
            from public.affiliate_sales where affiliate_id = p_affiliate
            order by created_at desc limit 200) s), '[]'::json),
        'payouts', coalesce((select json_agg(p order by p.paid_at desc) from (
            select amount_cents, note, paid_at
            from public.affiliate_payouts where affiliate_id = p_affiliate
            order by paid_at desc limit 100) p), '[]'::json))
    else json_build_object('sales', '[]'::json, 'payouts', '[]'::json)
    end;
$$;

-- ---------------------------------------------------------------------------
-- Applications from the website
-- ---------------------------------------------------------------------------
create table if not exists public.partner_applications (
    id          bigint generated always as identity primary key,
    name        text not null check (char_length(name) between 2 and 80),
    channel     text not null check (char_length(channel) between 4 and 300),  -- link to the channel
    audience    text check (char_length(audience) <= 120),                    -- reach, in words
    contact     text not null check (char_length(contact) between 3 and 200),  -- e-mail or Discord
    message     text check (char_length(message) <= 2000),
    status      text not null default 'new' check (status in ('new', 'accepted', 'rejected')),
    created_at  timestamptz not null default now(),
    notified_at timestamptz
);
alter table public.partner_applications enable row level security;
create index if not exists partner_applications_created_idx on public.partner_applications (created_at desc);
-- No policies: the functions below are the only way in and out.

-- Anyone may apply (no login). A hidden "website" field only bots fill in:
-- they get "ok" back and nothing is stored.
create or replace function public.submit_partner_application(
    p_name text, p_channel text, p_audience text, p_contact text, p_message text,
    p_consent boolean, p_website text default null)
returns text
language plpgsql
security definer
set search_path = public
as $$
begin
    if coalesce(p_website, '') <> '' then return 'ok'; end if;
    if not coalesce(p_consent, false) then return 'consent'; end if;
    if char_length(coalesce(trim(p_name), '')) < 2
       or char_length(coalesce(trim(p_channel), '')) < 4
       or char_length(coalesce(trim(p_contact), '')) < 3 then
        return 'missing';
    end if;
    -- A brake on floods: at most 30 applications a day, all visitors together.
    if (select count(*) from public.partner_applications where created_at > now() - interval '1 day') >= 30 then
        return 'busy';
    end if;
    insert into public.partner_applications (name, channel, audience, contact, message)
        values (trim(p_name), trim(p_channel), nullif(trim(coalesce(p_audience, '')), ''),
                trim(p_contact), nullif(trim(coalesce(p_message, '')), ''));
    return 'ok';
end;
$$;
revoke all on function public.submit_partner_application(text, text, text, text, text, boolean, text) from public;
grant execute on function public.submit_partner_application(text, text, text, text, text, boolean, text) to anon, authenticated;

-- Manager only: the list and the decision.
create or replace function public.list_partner_applications()
returns table (id bigint, name text, channel text, audience text, contact text, message text,
               status text, created_at timestamptz)
language plpgsql
stable
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    return query
        select a.id, a.name, a.channel, a.audience, a.contact, a.message, a.status, a.created_at
        from public.partner_applications a
        order by a.created_at desc
        limit 200;
end;
$$;
revoke all on function public.list_partner_applications() from public, anon;
grant execute on function public.list_partner_applications() to authenticated;

create or replace function public.set_partner_application_status(p_id bigint, p_status text)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    if p_status not in ('new', 'accepted', 'rejected') then raise exception 'bad status'; end if;
    update public.partner_applications set status = p_status where id = p_id;
end;
$$;
revoke all on function public.set_partner_application_status(bigint, text) from public, anon;
grant execute on function public.set_partner_application_status(bigint, text) to authenticated;

-- Every new application is handed to partner-application-notify, which
-- e-mails the owner once (it sets notified_at). pg_net is enabled in 0009.
create or replace function public.notify_partner_application()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
    perform net.http_post(
        url := 'https://sjwparnlbtuiyqmagyfg.supabase.co/functions/v1/partner-application-notify',
        headers := '{"Content-Type": "application/json"}'::jsonb,
        body := jsonb_build_object('id', new.id),
        timeout_milliseconds := 5000
    );
    return new;
end;
$$;

drop trigger if exists partner_application_notify on public.partner_applications;
create trigger partner_application_notify
    after insert on public.partner_applications
    for each row execute function public.notify_partner_application();
