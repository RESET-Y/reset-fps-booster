-- REVIEWS
--
-- Visitors can send a review from the homepage. A review stays private until
-- the manager approves it; only approved reviews are public (name or
-- pseudonym, text, stars). The contact field is never published and is only
-- shown to the manager.

create table if not exists public.reviews (
    id           bigint generated always as identity primary key,
    display_name text not null check (char_length(display_name) between 2 and 40),
    body         text not null check (char_length(body) between 10 and 600),
    stars        smallint check (stars between 1 and 5),
    contact      text check (char_length(contact) <= 200),
    status       text not null default 'new' check (status in ('new', 'approved', 'rejected')),
    created_at   timestamptz not null default now(),
    decided_at   timestamptz
);
alter table public.reviews enable row level security;
create index if not exists reviews_status_idx on public.reviews (status, created_at desc);
-- No policies: the functions below are the only way in and out.

-- Anyone may send a review (no login). A hidden "website" field only bots
-- fill in: they get "ok" back and nothing is stored. At most 20 a day.
create or replace function public.submit_review(
    p_name text, p_body text, p_stars int, p_contact text, p_consent boolean, p_website text default null)
returns text
language plpgsql
security definer
set search_path = public
as $$
begin
    if coalesce(p_website, '') <> '' then return 'ok'; end if;
    if not coalesce(p_consent, false) then return 'consent'; end if;
    if char_length(coalesce(trim(p_name), '')) < 2
       or char_length(coalesce(trim(p_body), '')) < 10 then
        return 'missing';
    end if;
    if (select count(*) from public.reviews where created_at > now() - interval '1 day') >= 20 then
        return 'busy';
    end if;
    insert into public.reviews (display_name, body, stars, contact)
        values (trim(p_name), trim(p_body),
                case when p_stars between 1 and 5 then p_stars else null end,
                nullif(trim(coalesce(p_contact, '')), ''));
    return 'ok';
end;
$$;
revoke all on function public.submit_review(text, text, int, text, boolean, text) from public;
grant execute on function public.submit_review(text, text, int, text, boolean, text) to anon, authenticated;

-- Public: approved reviews only. No contact, no status, no ids.
create or replace function public.public_reviews()
returns table (display_name text, body text, stars smallint, created_at timestamptz)
language sql
stable
security definer
set search_path = public
as $$
    select r.display_name, r.body, r.stars, r.created_at
    from public.reviews r
    where r.status = 'approved'
    order by r.created_at desc
    limit 30;
$$;
revoke all on function public.public_reviews() from public;
grant execute on function public.public_reviews() to anon, authenticated;

-- Manager only.
create or replace function public.list_reviews()
returns table (id bigint, display_name text, body text, stars smallint, contact text,
               status text, created_at timestamptz)
language plpgsql
stable
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    return query
        select r.id, r.display_name, r.body, r.stars, r.contact, r.status, r.created_at
        from public.reviews r
        order by r.created_at desc
        limit 200;
end;
$$;
revoke all on function public.list_reviews() from public, anon;
grant execute on function public.list_reviews() to authenticated;

create or replace function public.set_review_status(p_id bigint, p_status text)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    if p_status not in ('new', 'approved', 'rejected') then raise exception 'bad status'; end if;
    update public.reviews set status = p_status, decided_at = now() where id = p_id;
end;
$$;
revoke all on function public.set_review_status(bigint, text) from public, anon;
grant execute on function public.set_review_status(bigint, text) to authenticated;

create or replace function public.delete_review(p_id bigint)
returns text
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    delete from public.reviews where id = p_id;
    return case when found then 'ok' else 'not_found' end;
end;
$$;
revoke all on function public.delete_review(bigint) from public, anon;
grant execute on function public.delete_review(bigint) to authenticated;
