-- MANAGER DELETES
--
-- Three functions for the manager page. Each one checks is_manager() itself,
-- and each refuses to delete what would break bookkeeping or a customer's code:
--
--   delete_partner_application  always allowed (spam, tests, rejected)
--   delete_affiliate            only when the partner is switched off and has
--                               no sales and no payouts. Sales are accounting
--                               records (German retention duty), so a partner
--                               with sales is kept and only switched off.
--   delete_premium_code         only when nobody has redeemed it and no website
--                               purchase used it. Otherwise switch it off.

create or replace function public.delete_partner_application(p_id bigint)
returns text
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    delete from public.partner_applications where id = p_id;
    return case when found then 'ok' else 'not_found' end;
end;
$$;
revoke all on function public.delete_partner_application(bigint) from public, anon;
grant execute on function public.delete_partner_application(bigint) to authenticated;

-- Answers: ok, not_found, active (switch off first), has_sales, has_payouts.
create or replace function public.delete_affiliate(p_id bigint)
returns text
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    if not exists (select 1 from public.affiliates where id = p_id) then return 'not_found'; end if;
    if exists (select 1 from public.affiliates where id = p_id and active) then return 'active'; end if;
    if exists (select 1 from public.affiliate_sales where affiliate_id = p_id) then return 'has_sales'; end if;
    if exists (select 1 from public.affiliate_payouts where affiliate_id = p_id) then return 'has_payouts'; end if;
    delete from public.affiliates where id = p_id;
    return 'ok';
end;
$$;
revoke all on function public.delete_affiliate(bigint) from public, anon;
grant execute on function public.delete_affiliate(bigint) to authenticated;

-- Answers: ok, not_found, in_use (redeemed or bought with it; switch off instead).
create or replace function public.delete_premium_code(p_code text)
returns text
language plpgsql
security definer
set search_path = public
as $$
declare
    c text := upper(trim(coalesce(p_code, '')));
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    if not exists (select 1 from public.premium_codes where code = c) then return 'not_found'; end if;
    if exists (select 1 from public.code_redemptions where code = c)
       or exists (select 1 from public.purchases where code = c) then
        return 'in_use';
    end if;
    delete from public.premium_codes where code = c;
    return 'ok';
end;
$$;
revoke all on function public.delete_premium_code(text) from public, anon;
grant execute on function public.delete_premium_code(text) to authenticated;
