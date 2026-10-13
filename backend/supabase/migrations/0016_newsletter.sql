-- NEWSLETTER WITH DOUBLE OPT-IN.
--
-- E-mail advertising needs prior consent, and the consent has to be provable.
-- So an address is only mailed once its owner clicked the link in the
-- confirmation mail, and the row keeps what was agreed to and when.
--
--   newsletter_subscribers   one row per e-mail address
--     status pending       signed up, confirmation mail sent, not confirmed.
--                          Never receives a newsletter. Deleted after 30 days.
--     status confirmed     clicked the link: receives newsletters
--     status unsubscribed  left again. The row stays so the address is not
--                          mailed by mistake and the earlier consent can be
--                          shown; it is deleted on request.
--   newsletter_issues        one row per newsletter sent to everyone
--
-- confirm_hash is the SHA-256 of the token in the confirmation mail, so the
-- table alone cannot confirm anybody. unsubscribe_token is stored as it is:
-- every newsletter carries it in its unsubscribe link.
--
-- Row level security on, no policies: only the newsletter edge function
-- (service role) reads or writes these tables. Managers reach them through
-- that function, which checks user_roles first.

create table if not exists public.newsletter_subscribers (
    id                bigint generated always as identity primary key,
    email             text not null unique
                      check (char_length(email) between 5 and 200 and email ~ '^[^@\s]+@[^@\s]+\.[^@\s]+$'),
    lang              text not null default 'de' check (lang in ('de', 'en')),
    source            text not null default 'website' check (source in ('website', 'account', 'app')),
    status            text not null default 'pending' check (status in ('pending', 'confirmed', 'unsubscribed')),
    consent_text      text not null,                 -- the exact sentence the person ticked
    confirm_hash      text,                          -- null once confirmed
    unsubscribe_token text not null unique,
    requested_at      timestamptz not null default now(),
    confirm_sent_at   timestamptz,                   -- last confirmation mail, for the resend brake
    confirmed_at      timestamptz,
    unsubscribed_at   timestamptz
);
create index if not exists newsletter_subscribers_status_idx on public.newsletter_subscribers (status);
create index if not exists newsletter_subscribers_confirm_idx on public.newsletter_subscribers (confirm_hash);
alter table public.newsletter_subscribers enable row level security;

create table if not exists public.newsletter_issues (
    id          bigint generated always as identity primary key,
    subject     text not null,
    body        text not null,
    recipients  int not null default 0,
    failed      int not null default 0,
    sent_by     uuid references auth.users (id) on delete set null,
    sent_at     timestamptz not null default now()
);
alter table public.newsletter_issues enable row level security;
-- No policies on either table.
