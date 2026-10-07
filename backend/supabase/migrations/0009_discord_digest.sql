-- DISCORD DAILY DIGEST: a report for Lukas at 12:00 Berlin time, with reply drafts
-- he releases one by one.
--
-- discord_drafts      one reply draft per open question. The bot never posts a
--                     draft on its own: only Lukas pressing "Senden" (or sending
--                     an edited version) in his DM does, see discord-interactions.
-- discord_digest_runs one row per Berlin day, so the report goes out once a day
--                     however often the function is called.
--
-- Row level security on, no policies: only the service role (the edge
-- functions) reads or writes these tables.
create table if not exists public.discord_drafts (
    id               bigint generated always as identity primary key,
    created_at       timestamptz not null default now(),
    channel_id       text not null,          -- where the question was asked
    message_id       text not null,          -- the question; the reply references it
    question_summary text not null,
    draft            text not null,
    status           text not null default 'pending' check (status in ('pending', 'sent', 'discarded')),
    decided_at       timestamptz,
    sent_message_id  text
);
alter table public.discord_drafts enable row level security;

create table if not exists public.discord_digest_runs (
    day        date primary key,             -- the Berlin calendar day
    created_at timestamptz not null default now(),
    summary    jsonb                         -- the structured report, for later reference
);
alter table public.discord_digest_runs enable row level security;

-- 12:00 in Berlin is 10:00 UTC in summer and 11:00 UTC in winter. Both run;
-- the function only acts when it is noon in Berlin, and only once per day.
select cron.unschedule('discord-daily-digest')
where exists (select 1 from cron.job where jobname = 'discord-daily-digest');

select cron.schedule(
  'discord-daily-digest',
  '0 10,11 * * *',
  $$
  select net.http_post(
    url := 'https://sjwparnlbtuiyqmagyfg.supabase.co/functions/v1/discord-digest',
    headers := '{"Content-Type": "application/json"}'::jsonb,
    body := '{}'::jsonb,
    timeout_milliseconds := 5000
  );
  $$
);
