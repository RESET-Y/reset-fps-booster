-- DISCORD DAILY DIGEST: a report for Lukas at 12:00 Berlin time.
--
-- discord_digest_runs one row per Berlin day, so the report goes out once a day
--                     however often the function is called.
--
-- Row level security on, no policies: only the service role (the edge
-- function) reads or writes this table.
create table if not exists public.discord_digest_runs (
    day        date primary key,             -- the Berlin calendar day
    created_at timestamptz not null default now()
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
