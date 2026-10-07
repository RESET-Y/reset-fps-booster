-- PARTNERS SEE THEIR OWN NUMBERS.
--
-- An affiliate (a streamer with a code) can be linked to one RESET account.
-- Signed in with that account, the partner page on the website shows that
-- affiliate's code, terms, sales, commission and payouts - and nothing else:
-- no other affiliate, and no buyer of any kind (no names, no e-mails).
--
-- The manager links the account by e-mail on the manager page. An account
-- may be linked to more than one code; a code to one account at most.

alter table public.affiliates
    add column if not exists user_id uuid references auth.users (id) on delete set null;
create index if not exists affiliates_user_idx on public.affiliates (user_id);

-- ---------------------------------------------------------------------------
-- manager: link / unlink a partner account, and see who is linked
-- ---------------------------------------------------------------------------
--   ok         linked
--   cleared    empty e-mail: the link was removed
--   not_found  no account with that e-mail
create or replace function public.set_affiliate_account(p_id bigint, p_email text)
returns text
language plpgsql
security definer
set search_path = public
as $$
declare
    uid uuid;
    wanted text := lower(trim(coalesce(p_email, '')));
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    if wanted = '' then
        update public.affiliates set user_id = null where id = p_id;
        return 'cleared';
    end if;
    select u.id into uid from auth.users u where lower(u.email) = wanted limit 1;
    if uid is null then return 'not_found'; end if;
    update public.affiliates set user_id = uid where id = p_id;
    return 'ok';
end;
$$;
revoke all on function public.set_affiliate_account(bigint, text) from public, anon;
grant execute on function public.set_affiliate_account(bigint, text) to authenticated;

-- The linked e-mail per affiliate, for the manager page only.
create or replace function public.list_affiliate_accounts()
returns table (affiliate_id bigint, email text)
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
        select a.id, u.email::text
        from public.affiliates a
        join auth.users u on u.id = a.user_id;
end;
$$;
revoke all on function public.list_affiliate_accounts() from public, anon;
grant execute on function public.list_affiliate_accounts() to authenticated;

-- ---------------------------------------------------------------------------
-- partner: my codes and my numbers
-- ---------------------------------------------------------------------------
-- Every affiliate linked to the caller, with totals and this month's share.
-- "This month" is the calendar month in Berlin time, where payouts are made.
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
    order by a.created_at;
$$;
revoke all on function public.my_affiliates() from public, anon;
grant execute on function public.my_affiliates() to authenticated;

-- One of MY affiliates' sales and payouts. Sales carry what the partner needs
-- to check the commission - date, plan, first or renewal, net, commission,
-- refunded - and nothing about the buyer. Not mine: an empty answer, the same
-- as no sales, so nobody can probe which codes exist.
create or replace function public.my_affiliate_activity(p_affiliate bigint)
returns json
language sql
stable
security definer
set search_path = public
as $$
    select case when exists (select 1 from public.affiliates a where a.id = p_affiliate and a.user_id = auth.uid())
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
revoke all on function public.my_affiliate_activity(bigint) from public, anon;
grant execute on function public.my_affiliate_activity(bigint) to authenticated;
