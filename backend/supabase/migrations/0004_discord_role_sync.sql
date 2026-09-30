-- DISCORD PREMIUM ROLE: an hourly full sync.
--
-- The app syncs a user's role itself when they link Discord, redeem a code or
-- start the app. This catches everything else: premium that ran out, a Stripe
-- purchase, or someone who joined the server after linking.
--
-- pg_cron schedules, pg_net makes the HTTP call. The call carries no secret;
-- the function only ever applies what the database already says.
create extension if not exists pg_cron;
create extension if not exists pg_net;

select cron.unschedule('discord-premium-sync')
where exists (select 1 from cron.job where jobname = 'discord-premium-sync');

select cron.schedule(
  'discord-premium-sync',
  '7 * * * *',                      -- every hour at :07
  $$
  select net.http_post(
    url := 'https://sjwparnlbtuiyqmagyfg.supabase.co/functions/v1/discord-sync',
    headers := '{"Content-Type": "application/json"}'::jsonb,
    body := '{}'::jsonb
  );
  $$
);
