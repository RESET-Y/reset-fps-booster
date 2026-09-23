-- ACCOUNTS AND WHAT THEY ARE ENTITLED TO.
--
-- Two tables, and the split between them is the security model:
--
--   profiles       what a user may see and edit about themselves
--   entitlements   what they have PAID for - readable by the user, writable
--                  only by the server (the payment webhook, via the
--                  service_role key, which never ships in the app)
--
-- If entitlements were editable by the user, anyone could grant themselves
-- premium with one request using the public anon key. Row level security is
-- what stops that, so every table here has it on and nothing is readable or
-- writable except through an explicit policy.

-- ---------------------------------------------------------------------------
-- profiles: one row per user, created automatically at sign-up
-- ---------------------------------------------------------------------------
create table if not exists public.profiles (
    id           uuid primary key references auth.users (id) on delete cascade,
    display_name text,
    created_at   timestamptz not null default now()
);

alter table public.profiles enable row level security;

create policy "profiles: read own"
    on public.profiles for select
    using (auth.uid() = id);

create policy "profiles: update own"
    on public.profiles for update
    using (auth.uid() = id)
    with check (auth.uid() = id);

-- No insert or delete policy for users. Rows are created by the trigger below
-- and removed with the auth user (on delete cascade).

-- ---------------------------------------------------------------------------
-- entitlements: what a user has bought. Written ONLY by the server.
-- ---------------------------------------------------------------------------
create table if not exists public.entitlements (
    id           bigint generated always as identity primary key,
    user_id      uuid not null references auth.users (id) on delete cascade,
    product      text not null,                 -- e.g. 'premium'
    source       text not null,                 -- e.g. 'lemonsqueezy', 'manual'
    external_ref text,                          -- the payment provider's order/subscription id
    valid_until  timestamptz,                   -- null = does not expire
    created_at   timestamptz not null default now(),
    unique (user_id, product, source, external_ref)
);

create index if not exists entitlements_user_idx on public.entitlements (user_id);

alter table public.entitlements enable row level security;

create policy "entitlements: read own"
    on public.entitlements for select
    using (auth.uid() = user_id);

-- Deliberately NO insert / update / delete policy. With RLS on and no policy,
-- the anon and authenticated roles cannot write here at all. Only the
-- service_role - used by the payment webhook, never by the app - bypasses RLS.

-- ---------------------------------------------------------------------------
-- is_premium(): the one question the app asks
-- ---------------------------------------------------------------------------
-- security invoker, so it runs as the caller and RLS still applies: a user can
-- only ever ask about themselves.
create or replace function public.is_premium()
returns boolean
language sql
stable
security invoker
set search_path = public
as $$
    select exists (
        select 1
        from public.entitlements
        where user_id = auth.uid()
          and product = 'premium'
          and (valid_until is null or valid_until > now())
    );
$$;

grant execute on function public.is_premium() to authenticated;

-- ---------------------------------------------------------------------------
-- a profile row for every new user
-- ---------------------------------------------------------------------------
create or replace function public.handle_new_user()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
    insert into public.profiles (id, display_name)
    values (
        new.id,
        coalesce(new.raw_user_meta_data ->> 'full_name',
                 new.raw_user_meta_data ->> 'name',
                 split_part(new.email, '@', 1))
    )
    on conflict (id) do nothing;
    return new;
end;
$$;

drop trigger if exists on_auth_user_created on auth.users;
create trigger on_auth_user_created
    after insert on auth.users
    for each row execute function public.handle_new_user();
