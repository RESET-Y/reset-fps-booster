-- PUBLIC NUMBERS for the live counter on the website.
--
-- Returns { "accounts": int, "premium": int, "at": timestamptz }.
--   accounts  every RESET account (one profiles row per user)
--   premium   accounts with Premium active right now, from any source
--             (Stripe, a bought code or a free code), each counted once
--
-- Counts only, never who: no ids, no e-mails. Anyone may call it, like
-- affiliate_offer. security definer, because row level security would
-- otherwise limit an anonymous caller to zero rows.
create or replace function public.public_stats()
returns json
language sql
stable
security definer
set search_path = public
as $$
    select json_build_object(
        'accounts', (select count(*) from public.profiles),
        'premium', (select count(distinct user_id)
                    from public.entitlements
                    where product = 'premium'
                      and (valid_until is null or valid_until > now())),
        'at', now()
    );
$$;
revoke all on function public.public_stats() from public;
grant execute on function public.public_stats() to anon, authenticated;
