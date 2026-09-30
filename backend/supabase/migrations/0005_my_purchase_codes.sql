-- A BOUGHT CODE CAN BE FOUND AGAIN.
--
-- A code bought on the website used to live only on the thank-you page:
-- close the tab and it was gone. Now the buyer's e-mail from Stripe is kept
-- with the purchase, and a signed-in user sees every code bought with their
-- e-mail - on the website's account page - and can redeem it there.
--
-- Only a CONFIRMED e-mail counts. Otherwise anyone could sign up with a
-- buyer's address, never confirm it, and read that buyer's codes.

alter table public.purchases add column if not exists buyer_email text;
create index if not exists purchases_buyer_email_idx on public.purchases (lower(buyer_email));

-- Same as in 0004, plus the buyer's e-mail. Replaced rather than overloaded,
-- so there is only ever one function for the webhook to call.
drop function if exists public.issue_purchase_code(text, text, text, timestamptz, text);

create or replace function public.issue_purchase_code(
    p_session     text,
    p_plan        text,
    p_stripe_ref  text,
    p_valid_until timestamptz,
    p_note        text,
    p_email       text)
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

    insert into public.purchases (session_id, plan, code, buyer_email)
    values (p_session, p_plan, c, nullif(lower(trim(p_email)), ''));
    return c;
end;
$$;
revoke all on function public.issue_purchase_code(text, text, text, timestamptz, text, text) from public, anon, authenticated;
grant execute on function public.issue_purchase_code(text, text, text, timestamptz, text, text) to service_role;

-- The signed-in user's bought codes, newest first.
--   state: open       not redeemed yet, still valid
--          redeemed   used (by this account or another)
--          ended      refunded, or its subscription has ended
create or replace function public.my_purchase_codes()
returns table (code text, plan text, bought_at timestamptz, state text)
language sql
stable
security definer
set search_path = public
as $$
    select p.code, p.plan, p.created_at,
           case
               when pc.uses >= pc.max_uses then 'redeemed'
               when not pc.active or (pc.valid_until is not null and pc.valid_until <= now()) then 'ended'
               else 'open'
           end
    from public.purchases p
    join public.premium_codes pc on pc.code = p.code
    where p.buyer_email is not null
      and p.buyer_email = (
          select lower(u.email) from auth.users u
          where u.id = auth.uid() and u.email_confirmed_at is not null)
    order by p.created_at desc;
$$;
revoke all on function public.my_purchase_codes() from public, anon;
grant execute on function public.my_purchase_codes() to authenticated;
