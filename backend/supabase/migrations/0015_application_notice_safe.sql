-- A NEW APPLICATION IS ALWAYS SAVED, EVEN IF THE NOTICE CANNOT BE SENT.
--
-- Migration 0011 mails every new partner application to the owner by calling the
-- partner-application-notify function from a trigger, through pg_net. Two ways
-- that call could take the whole application down with it:
--   - pg_net is switched on in 0007 (Discord), not where 0011's comment says. On
--     a database where 0007 never ran, "net.http_post" does not exist, the trigger
--     raises, and the INSERT of the application fails with it: the visitor sees
--     "That didn't work" and nothing is stored.
--   - Any other error in the HTTP call does the same.
-- The notice is a courtesy; the application is the data. So: make sure pg_net is
-- on, and catch whatever the call throws. A failed notice leaves notified_at empty,
-- and the application still shows up in the manager.

create extension if not exists pg_net;

create or replace function public.notify_partner_application()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
    begin
        perform net.http_post(
            url := 'https://sjwparnlbtuiyqmagyfg.supabase.co/functions/v1/partner-application-notify',
            headers := '{"Content-Type": "application/json"}'::jsonb,
            body := jsonb_build_object('id', new.id),
            timeout_milliseconds := 5000
        );
    exception when others then
        raise warning 'partner application % saved, notice not sent: %', new.id, sqlerrm;
    end;
    return new;
end;
$$;

-- the trigger itself is unchanged (0011); it picks up the new function body
