-- HOW LONG PREMIUM LASTS, for the signed-in user only.
--
-- Returns { "active": bool, "until": timestamptz | null }.
--   active false            no premium at all
--   active true, until null premium without an end (lifetime, or a code
--                           created as "forever")
--   active true, until ts   premium ends at ts
--
-- With several sources at once - a code AND a subscription, say - the one
-- that lasts longest wins, because that is how long the user actually has it.
-- security invoker: row level security on entitlements still applies, so a
-- user can only ever see their own.
create or replace function public.premium_status()
returns json
language sql
stable
security invoker
set search_path = public
as $$
    with live as (
        select valid_until
        from public.entitlements
        where user_id = auth.uid()
          and product = 'premium'
          and (valid_until is null or valid_until > now())
    )
    select json_build_object(
        'active', exists (select 1 from live),
        'until', case
                     when exists (select 1 from live where valid_until is null) then null
                     else (select max(valid_until) from live)
                 end
    );
$$;
grant execute on function public.premium_status() to authenticated;
