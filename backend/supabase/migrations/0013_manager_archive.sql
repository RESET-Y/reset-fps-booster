-- MANAGER ARCHIVE
--
-- Archiving hides a partner, a premium code or an application from the
-- manager lists. Nothing is deleted: sales, payouts and redemptions stay as
-- they are (bookkeeping and customer codes), and the item can be restored.
-- A partner or a code is also switched off when archived; the manager page
-- does that first, through the same functions as before.

create table if not exists public.manager_archive (
    kind        text not null check (kind in ('partner', 'code', 'application')),
    ref         text not null,                 -- partner id, premium code, application id
    archived    boolean not null default true,
    changed_at  timestamptz not null default now(),
    primary key (kind, ref)
);
alter table public.manager_archive enable row level security;
-- No policies: only the functions below touch it.

create or replace function public.set_archived(p_kind text, p_ref text, p_archived boolean)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    if p_kind not in ('partner', 'code', 'application') then raise exception 'bad kind'; end if;
    insert into public.manager_archive (kind, ref, archived, changed_at)
        values (p_kind, p_ref, p_archived, now())
        on conflict (kind, ref) do update set archived = excluded.archived, changed_at = excluded.changed_at;
end;
$$;
revoke all on function public.set_archived(text, text, boolean) from public, anon;
grant execute on function public.set_archived(text, text, boolean) to authenticated;

create or replace function public.list_archived()
returns table (kind text, ref text)
language plpgsql
stable
security definer
set search_path = public
as $$
begin
    if not public.is_manager() then raise exception 'not allowed' using errcode = '42501'; end if;
    return query
        select a.kind, a.ref from public.manager_archive a where a.archived;
end;
$$;
revoke all on function public.list_archived() from public, anon;
grant execute on function public.list_archived() to authenticated;
