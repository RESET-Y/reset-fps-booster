-- DISCORD REPORTS FOR THE FIX ROUTINE: what the digest bot read, kept so the
-- local Claude routine (12:30 and 20:30 on Lukas's PC) can work through it.
--
-- discord_reports        one row per Discord message from #bug-reports, #hilfe and
--                        #vorschläge (threads included). Scrubbed like the DM: no
--                        e-mail addresses, tokens, passwords or IBANs, and no author
--                        names or ids. Rows older than 60 days are deleted on the
--                        next collection.
-- discord_collect_state  one row: when the last collection started and ended, so the
--                        function collects at most every 10 minutes however often it
--                        is called, and the routine can see that a collection finished.
--
-- Row level security on, no policies: only the service role (the edge function)
-- and the database owner (the routine, through the Supabase connector) read or write.
create table if not exists public.discord_reports (
    id           text primary key,                -- Discord message id
    channel_id   text not null,                   -- the channel, or the thread the message is in
    kind         text not null check (kind in ('bugs', 'help', 'ideas')),
    thread       text,                            -- thread / forum post title, scrubbed
    posted_at    timestamptz not null,
    from_team    boolean not null default false,  -- Lukas or a bot
    reply_to     text,                            -- Discord message id it answers
    content      text not null default '',        -- scrubbed message text
    attachments  int not null default 0,          -- count only, never downloaded
    status       text not null default 'new'
                 check (status in ('new', 'seen', 'fix-prepared', 'needs-lukas', 'ignored')),
    note         text,                            -- what the routine did: branch, reason
    collected_at timestamptz not null default now(),
    updated_at   timestamptz not null default now()
);
create index if not exists discord_reports_status_idx on public.discord_reports (status, posted_at);
create index if not exists discord_reports_channel_idx on public.discord_reports (channel_id, posted_at);
alter table public.discord_reports enable row level security;
revoke all on public.discord_reports from anon, authenticated;

create table if not exists public.discord_collect_state (
    id          int primary key default 1 check (id = 1),
    started_at  timestamptz,
    finished_at timestamptz,
    stored      int,
    error       text
);
alter table public.discord_collect_state enable row level security;
revoke all on public.discord_collect_state from anon, authenticated;
insert into public.discord_collect_state (id) values (1) on conflict (id) do nothing;
