-- 0363_event_eggs (PostgreSQL): the Easter egg hunt's finds (Krunker v10.0.3 live event 3, doc/v10/events.md). A shot
-- or hit on an EVENT object asks game.foundEventItem's proc 68 whether the account found that egg before
-- (deob-10.0.3.js:220700-220728); Box Office keeps the finds here (services/brain/db_eggs.c "egg_found"). One row per
-- (account, egg): `egg_key` is mapIndex-uid, the egg's official map and GameObject uid. Additive only.
CREATE TABLE event_eggs (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    egg_key     TEXT NOT NULL,
    found_at    timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, egg_key)
);
