-- A MANAGER ROLE, AND CODES THAT GIVE PREMIUM FOR FREE.
--
-- Everything that matters is decided HERE, in the database, not in the app.
-- The app can be taken apart, patched and replayed; the database cannot. So:
--
--   user_roles         who is a manager. No policy lets anyone write it -
--                      a manager is made by hand, in the SQL editor.
--   premium_codes      the codes. Unreadable and unwritable from outside
--                      except through the functions below.
--   code_redemptions   who redeemed what, so a user cannot redeem the same
--                      code twice and every free grant can be traced.
--
-- All access goes through SECURITY DEFINER functions that check the caller
-- first. They run as their owner and so can write entitlements - which no
-- user can - but only after the check has passed.

-- ---------------------------------------------------------------------------
-- roles
-- ---------------------------------------------------------------------------
create table if not exists public.user_roles (
    user_id    uuid not null references auth.users (id) on delete cascade,
    role       text not null check (role in ('manager')),
    created_at timestamptz not null default now(),
    primary key (user_id, role)
);
alter table public.user_roles enable row level security;
-- No policies: nobody reads or writes this directly.

create or replace function public.is_manager()
returns boolean
language sql
stable
security definer
set search_path = public
as $$
    select exists (
        select 1 from public.user_roles
        where user_id = auth.uid() and role = 'manager'
    );
$$;
grant execute on function public.is_manager() to authenticated;

-- ---------------------------------------------------------------------------
-- codes and redemptions
-- ---------------------------------------------------------------------------
create table if not exists public.premium_codes (
    code          text primary key,
    created_by    uuid not null references auth.users (id),
    created_at    timestamptz not null default now(),
    duration_days int check (duration_days is null or duration_days > 0),  -- null = does not expire
    max_uses      int not null default 1 check (max_uses > 0),
    uses          int not null default 0,
    active        boolean not null default true,
    note          text
);
alter table public.premium_codes enable row level security;

create table if not exists public.code_redemptions (
    code        text not null references public.premium_codes (code) on delete cascade,
    user_id     uuid not null references auth.users (id) on delete cascade,
    redeemed_at timestamptz not null default now(),
    primary key (code, user_id)
);
alter table public.code_redemptions enable row level security;
-- No policies on either: only the functions below touch them.

-- 12 characters from a 31-letter alphabet without look-alikes (no 0/O, 1/I/L)
-- is about 59 bits - some 10^17 to 10^18 possibilities, far beyond guessing. Shown in groups
-- of four so it can be read out and typed without errors.
create or replace function public.new_code_text()
returns text
language plpgsql
volatile
set search_path = public, extensions
as $$
declare
    alphabet constant text := 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
    bytes bytea := extensions.gen_random_bytes(12);
    raw text := '';
    i int;
begin
    for i in 0..11 loop
        raw := raw || substr(alphabet, (get_byte(bytes, i) % length(alphabet)) + 1, 1);
    end loop;
    return 'RFB-' || substr(raw, 1, 4) || '-' || substr(raw, 5, 4) || '-' || substr(raw, 9, 4);
end;
$$;
revoke all on function public.new_code_text() from public, anon, authenticated;

-- ---------------------------------------------------------------------------
-- manager: create, list, deactivate
-- ---------------------------------------------------------------------------
create or replace function public.create_premium_code(
    p_duration_days int,
    p_max_uses int default 1,
    p_note text default null)
returns text
language plpgsql
security definer
set search_path = public
as $$
declare
    c text;
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    if p_duration_days is not null and p_duration_days <= 0 then
        raise exception 'duration must be positive or null';
    end if;
    if p_max_uses is null or p_max_uses <= 0 then
        raise exception 'max uses must be positive';
    end if;

    -- A clash in 10^18 is not a practical worry, but it costs nothing to
    -- try again rather than fail.
    loop
        c := public.new_code_text();
        begin
            insert into public.premium_codes (code, created_by, duration_days, max_uses, note)
            values (c, auth.uid(), p_duration_days, p_max_uses, nullif(trim(p_note), ''));
            return c;
        exception when unique_violation then
            -- next attempt
        end;
    end loop;
end;
$$;
grant execute on function public.create_premium_code(int, int, text) to authenticated;

create or replace function public.list_premium_codes()
returns table (code text, created_at timestamptz, duration_days int,
               max_uses int, uses int, active boolean, note text)
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
        select pc.code, pc.created_at, pc.duration_days, pc.max_uses, pc.uses, pc.active, pc.note
        from public.premium_codes pc
        order by pc.created_at desc;
end;
$$;
grant execute on function public.list_premium_codes() to authenticated;

create or replace function public.set_premium_code_active(p_code text, p_active boolean)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then
        raise exception 'not allowed' using errcode = '42501';
    end if;
    update public.premium_codes set active = p_active where code = upper(trim(p_code));
end;
$$;
grant execute on function public.set_premium_code_active(text, boolean) to authenticated;

-- ---------------------------------------------------------------------------
-- anyone signed in: redeem
-- ---------------------------------------------------------------------------
-- Returns a status word, not an exception, for the cases a user can cause, so
-- the app can say exactly what went wrong in the user's language:
--   ok, not_found, inactive, used_up, already_redeemed
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
    if c.uses >= c.max_uses then return 'used_up'; end if;
    if exists (select 1 from public.code_redemptions where code = c.code and user_id = uid) then
        return 'already_redeemed';
    end if;

    insert into public.code_redemptions (code, user_id) values (c.code, uid);
    update public.premium_codes set uses = uses + 1 where code = c.code;

    insert into public.entitlements (user_id, product, source, external_ref, valid_until)
    values (uid, 'premium', 'code', c.code,
            case when c.duration_days is null then null
                 else now() + make_interval(days => c.duration_days) end)
    on conflict (user_id, product, source, external_ref) do nothing;

    return 'ok';
end;
$$;
grant execute on function public.redeem_premium_code(text) to authenticated;
