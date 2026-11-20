-- 0390_rented_servers (PostgreSQL): Krunker v10.0.7's Rent Dedicated Server on Box Office (services/brain/rentals.c,
-- doc/v10/rented-servers.md; the window deob-10.0.7.js:219853-219938, rdsrv :204642-204668, prices :18707-18708).
-- One row per pool slot (brain.rentals.pool, upserted at start by id): a server_bin that kn_rentd runs on `host`.
--   region       Krunker's rental region index (iíïííïî, :18709): the window's REG
--   svn, sid     the row's name SVN_SID (Ashburn_1)
--   owner_id     the renter (NULL: free); expires_s the rental's end (epoch seconds; TRM = expires_s - now)
--   conf         the hosted game as JSON {gid, mn, cr, key, typ} (Krunker's CONF), NULL while free
--   generation   + 1 on every rent / game change / restart / expiry: kn_rentd restarts the instance on a change
--   rent_gen, rented_s, paid   the startup guarantee: a fresh rental whose generation never ran within
--                brain.rentals.start_timeout_s gets `paid` back (paid = 0 once it ran)
--   running_gen, state, agent_seen_s   what kn_rentd last reported, and when it last polled
CREATE TABLE rented_servers (
    id           integer PRIMARY KEY,
    host         text NOT NULL,
    region       integer NOT NULL,
    svn          text NOT NULL,
    sid          integer NOT NULL,
    port         integer NOT NULL,
    public_host  text NOT NULL DEFAULT '',
    owner_id     integer REFERENCES accounts(id) ON DELETE SET NULL,
    expires_s    bigint NOT NULL DEFAULT 0,
    conf         text,
    generation   integer NOT NULL DEFAULT 0,
    rent_gen     integer NOT NULL DEFAULT 0,
    rented_s     bigint NOT NULL DEFAULT 0,
    paid         bigint NOT NULL DEFAULT 0,
    running_gen  integer NOT NULL DEFAULT -1,
    state        text,
    agent_seen_s bigint NOT NULL DEFAULT 0
);
CREATE INDEX rented_servers_owner_idx ON rented_servers (owner_id);
CREATE INDEX rented_servers_host_idx ON rented_servers (host);
