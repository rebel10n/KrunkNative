-- 0201_ranked: v3.1.1 ranked matchmaking queue (parties, members, matches,
-- abandon cooldowns). Owner: svc-matchmaker (services/brain/ranked.c).
-- Ratings themselves live in players.player_elo/elo2/elo4 (svc-accounts) and are
-- written through the accounts "elo" op. Shared by every brain: any brain serves
-- /mm/queue/*, and the brain that wins pg_try_advisory_xact_lock(kn_rq_lock())
-- on its tick forms the matches.
--
-- Client protocol (ClientManager.QueueManager in v3.1.1): doc/backend.md 6.5.

CREATE TABLE mm_ranked_parties (
    party_id      text PRIMARY KEY,
    region        text NOT NULL,
    leader_client text NOT NULL,
    queue_ids     text[] NOT NULL DEFAULT '{}',
    status        text NOT NULL DEFAULT 'Queued' CHECK (status IN ('Queued', 'CreatingGame', 'Join')),
    status_c      jsonb NOT NULL DEFAULT '{}'::jsonb,
    match_id      text,
    ready_since   timestamptz,                    -- all members ready (+ queues) since
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE mm_ranked_members (
    client_id     text PRIMARY KEY,
    client_key    text NOT NULL UNIQUE,
    party_id      text NOT NULL REFERENCES mm_ranked_parties (party_id) ON DELETE CASCADE,
    account_id    bigint NOT NULL UNIQUE,         -- one queue entry per account
    username      text NOT NULL,
    account_data  jsonb NOT NULL DEFAULT '{}'::jsonb,  -- {c, cR, elo, elo2, elo4, level}
    class_index   integer NOT NULL DEFAULT 0,
    is_ready      boolean NOT NULL DEFAULT false,
    join_ticket   text,                           -- seek-style ticket once the match exists
    joined_at     timestamptz NOT NULL DEFAULT clock_timestamp(),
    last_poll     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX mm_ranked_members_party_idx ON mm_ranked_members (party_id);

CREATE TABLE mm_ranked_matches (
    match_id      text PRIMARY KEY,
    queue_id      text NOT NULL,
    region        text NOT NULL,
    brain_id      text NOT NULL,                  -- brain that formed it
    game_id       text,
    status        text NOT NULL DEFAULT 'creating' CHECK (status IN ('creating', 'live', 'done', 'aborted')),
    result        jsonb,
    created_at    timestamptz NOT NULL DEFAULT now(),
    started_at    timestamptz,
    finished_at   timestamptz
);
CREATE INDEX mm_ranked_matches_status_idx ON mm_ranked_matches (status, created_at);

CREATE TABLE mm_ranked_match_players (
    match_id      text NOT NULL REFERENCES mm_ranked_matches (match_id) ON DELETE CASCADE,
    account_id    bigint NOT NULL,
    username      text NOT NULL,
    team          integer NOT NULL,
    rating_key    text NOT NULL,                  -- elo | elo2 | elo4
    rating_before integer NOT NULL,
    rating_after  integer,
    PRIMARY KEY (match_id, account_id)
);
CREATE INDEX mm_ranked_match_players_account_idx ON mm_ranked_match_players (account_id);

CREATE TABLE mm_ranked_bans (
    account_id    bigint PRIMARY KEY,
    until         timestamptz NOT NULL,
    reason        text NOT NULL DEFAULT ''
);

CREATE OR REPLACE FUNCTION kn_rq_lock() RETURNS bigint LANGUAGE sql IMMUTABLE AS $$ SELECT 74610201::bigint $$;

-- queue id -> rating key / team size (v3.1.1 config.queues)
CREATE OR REPLACE FUNCTION kn_rq_key(q text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE q WHEN 'r1v1' THEN 'elo' WHEN 'r2v2' THEN 'elo2' WHEN 'r4v4' THEN 'elo4' END
$$;
CREATE OR REPLACE FUNCTION kn_rq_team_size(q text) RETURNS integer LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE q WHEN 'r1v1' THEN 1 WHEN 'r2v2' THEN 2 WHEN 'r4v4' THEN 4 END
$$;

-- recompute ready_since after any party change
CREATE OR REPLACE FUNCTION kn_rq_touch(p_party text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    v_all boolean;
BEGIN
    SELECT bool_and(is_ready) AND count(*) > 0 INTO v_all FROM mm_ranked_members WHERE party_id = p_party;
    UPDATE mm_ranked_parties
       SET ready_since = CASE WHEN coalesce(v_all, false) AND cardinality(queue_ids) > 0
                              THEN coalesce(ready_since, now()) ELSE NULL END,
           updated_at = now()
     WHERE party_id = p_party;
END $$;

-- join (or create) a party. Returns {"status": null|"<error>", "partyId", "cooldownMs"?}
CREATE OR REPLACE FUNCTION kn_rq_join(p_account bigint, p_username text, p_data jsonb, p_region text,
                                      p_queues text[], p_party text, p_ready boolean, p_class integer,
                                      p_client_id text, p_client_key text, p_new_party text,
                                      p_max_party integer)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
    v_until timestamptz;
    v_party mm_ranked_parties%ROWTYPE;
    v_pid text;
    v_count integer;
BEGIN
    SELECT until INTO v_until FROM mm_ranked_bans WHERE account_id = p_account AND until > now();
    IF FOUND THEN
        RETURN jsonb_build_object('status', 'RankedCooldown',
                                  'cooldownMs', floor(extract(epoch FROM v_until - now()) * 1000));
    END IF;
    IF EXISTS (SELECT 1 FROM mm_ranked_members WHERE account_id = p_account)
       OR EXISTS (SELECT 1 FROM mm_ranked_match_players mp JOIN mm_ranked_matches m USING (match_id)
                   WHERE mp.account_id = p_account AND m.status IN ('creating', 'live')) THEN
        RETURN jsonb_build_object('status', 'AccountAlreadyPlaying');
    END IF;
    IF coalesce(p_party, '') <> '' THEN
        SELECT * INTO v_party FROM mm_ranked_parties WHERE party_id = p_party FOR UPDATE;
        IF NOT FOUND OR v_party.status <> 'Queued' THEN
            RETURN jsonb_build_object('status', 'PartyNotFound');
        END IF;
        SELECT count(*) INTO v_count FROM mm_ranked_members WHERE party_id = p_party;
        IF v_count >= p_max_party THEN
            RETURN jsonb_build_object('status', 'PartyFull');
        END IF;
        v_pid := p_party;
    ELSE
        INSERT INTO mm_ranked_parties (party_id, region, leader_client, queue_ids)
        VALUES (p_new_party, p_region, p_client_id, coalesce(p_queues, '{}'));
        v_pid := p_new_party;
    END IF;
    BEGIN
        INSERT INTO mm_ranked_members (client_id, client_key, party_id, account_id, username, account_data,
                                       class_index, is_ready)
        VALUES (p_client_id, p_client_key, v_pid, p_account, p_username, coalesce(p_data, '{}'::jsonb),
                coalesce(p_class, 0), coalesce(p_ready, false));
    EXCEPTION WHEN unique_violation THEN
        RETURN jsonb_build_object('status', 'AccountAlreadyPlaying');
    END;
    PERFORM kn_rq_touch(v_pid);
    RETURN jsonb_build_object('status', NULL, 'partyId', v_pid);
END $$;

-- v3.1.1 /queue/poll response for a client key (NULL when the key is unknown)
CREATE OR REPLACE FUNCTION kn_rq_state(p_key text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
    v_m mm_ranked_members%ROWTYPE;
    v_p mm_ranked_parties%ROWTYPE;
    v_c jsonb;
    v_key text;
BEGIN
    UPDATE mm_ranked_members SET last_poll = now() WHERE client_key = p_key RETURNING * INTO v_m;
    IF NOT FOUND THEN RETURN NULL; END IF;
    SELECT * INTO v_p FROM mm_ranked_parties WHERE party_id = v_m.party_id;
    IF NOT FOUND THEN RETURN NULL; END IF;
    v_key := coalesce(kn_rq_key(v_p.queue_ids[1]), 'elo');
    IF v_p.status = 'Join' THEN
        v_c := v_p.status_c || jsonb_build_object('clientId', v_m.join_ticket);
    ELSIF v_p.status = 'Queued' THEN
        v_c := jsonb_build_object('queueSize', (
            SELECT count(*) FROM mm_ranked_members m JOIN mm_ranked_parties p USING (party_id)
             WHERE p.status = 'Queued' AND p.region = v_p.region AND p.ready_since IS NOT NULL
               AND p.queue_ids && v_p.queue_ids));
    ELSE
        v_c := '{}'::jsonb;
    END IF;
    RETURN jsonb_build_object(
        'status', jsonb_build_object('t', v_p.status, 'c', v_c),
        'partyId', v_p.party_id,
        'partyLeaderId', v_p.leader_client,
        'partyMembers', coalesce((
            SELECT jsonb_agg(jsonb_build_object(
                       'clientId', m.client_id, 'accountId', m.username,
                       'accountData', jsonb_build_object('c', m.account_data -> 'c', 'cR', m.account_data -> 'cR',
                                                         'e', coalesce(m.account_data -> v_key, '0'::jsonb))::text,
                       'classIndex', m.class_index, 'isReady', m.is_ready) ORDER BY m.joined_at)
              FROM mm_ranked_members m WHERE m.party_id = v_p.party_id), '[]'::jsonb),
        'queueIds', to_jsonb(v_p.queue_ids),
        'region', v_p.region);
END $$;

-- party actions: ready | queues | class | kick | promote | leave.
-- Returns the new state, NULL for an unknown key, {"left":true} after leave.
CREATE OR REPLACE FUNCTION kn_rq_action(p_key text, p_action text, p_arg text, p_valid_queues text[])
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
    v_m mm_ranked_members%ROWTYPE;
    v_p mm_ranked_parties%ROWTYPE;
    v_leader boolean;
BEGIN
    SELECT * INTO v_m FROM mm_ranked_members WHERE client_key = p_key;
    IF NOT FOUND THEN RETURN NULL; END IF;
    SELECT * INTO v_p FROM mm_ranked_parties WHERE party_id = v_m.party_id FOR UPDATE;
    v_leader := v_p.leader_client = v_m.client_id;
    IF p_action = 'leave' THEN
        DELETE FROM mm_ranked_members WHERE client_id = v_m.client_id;
        IF NOT EXISTS (SELECT 1 FROM mm_ranked_members WHERE party_id = v_p.party_id) THEN
            DELETE FROM mm_ranked_parties WHERE party_id = v_p.party_id;
        ELSE
            IF v_leader THEN
                UPDATE mm_ranked_parties SET leader_client = (
                    SELECT client_id FROM mm_ranked_members WHERE party_id = v_p.party_id ORDER BY joined_at LIMIT 1)
                 WHERE party_id = v_p.party_id;
            END IF;
            PERFORM kn_rq_touch(v_p.party_id);
        END IF;
        RETURN jsonb_build_object('left', true);
    END IF;
    IF v_p.status = 'Queued' THEN
        IF p_action = 'ready' THEN
            UPDATE mm_ranked_members SET is_ready = (p_arg IN ('true', '1')) WHERE client_id = v_m.client_id;
        ELSIF p_action = 'class' THEN
            UPDATE mm_ranked_members SET class_index = greatest(0, coalesce(nullif(p_arg, '')::integer, 0))
             WHERE client_id = v_m.client_id;
        ELSIF p_action = 'queues' AND v_leader THEN
            UPDATE mm_ranked_parties SET queue_ids = coalesce((
                SELECT array_agg(DISTINCT q) FROM jsonb_array_elements_text(p_arg::jsonb) q
                 WHERE q = ANY (p_valid_queues)), '{}') WHERE party_id = v_p.party_id;
        ELSIF p_action = 'kick' AND v_leader AND p_arg <> v_m.client_id THEN
            DELETE FROM mm_ranked_members WHERE client_id = p_arg AND party_id = v_p.party_id;
        ELSIF p_action = 'promote' AND v_leader THEN
            UPDATE mm_ranked_parties SET leader_client = p_arg
             WHERE party_id = v_p.party_id
               AND EXISTS (SELECT 1 FROM mm_ranked_members WHERE client_id = p_arg AND party_id = v_p.party_id);
        END IF;
        PERFORM kn_rq_touch(v_p.party_id);
    END IF;
    RETURN kn_rq_state(p_key);
END $$;

-- housekeeping (any brain, idempotent): drop members that stopped polling,
-- fix leaders, drop empty parties, unstick CreatingGame, time out matches.
CREATE OR REPLACE FUNCTION kn_rq_expire(p_poll_s integer, p_creating_s integer, p_match_s integer)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM mm_ranked_members WHERE last_poll < now() - make_interval(secs => p_poll_s);
    DELETE FROM mm_ranked_parties p
     WHERE NOT EXISTS (SELECT 1 FROM mm_ranked_members m WHERE m.party_id = p.party_id);
    UPDATE mm_ranked_parties p SET leader_client = (
        SELECT client_id FROM mm_ranked_members m WHERE m.party_id = p.party_id ORDER BY joined_at LIMIT 1)
     WHERE NOT EXISTS (SELECT 1 FROM mm_ranked_members m WHERE m.client_id = p.leader_client);
    UPDATE mm_ranked_matches SET status = 'aborted', finished_at = now(), result = '{"reason":"host timeout"}'
     WHERE status = 'creating' AND created_at < now() - make_interval(secs => p_creating_s);
    UPDATE mm_ranked_parties p SET status = 'Queued', status_c = '{}', match_id = NULL, updated_at = now()
     WHERE status = 'CreatingGame'
       AND NOT EXISTS (SELECT 1 FROM mm_ranked_matches x WHERE x.match_id = p.match_id AND x.status = 'creating');
    UPDATE mm_ranked_matches SET status = 'aborted', finished_at = now(), result = '{"reason":"no result"}'
     WHERE status = 'live' AND started_at < now() - make_interval(secs => p_match_s);
    DELETE FROM mm_ranked_bans WHERE until < now();
END $$;

-- matcher: ready parties (call inside the lock transaction)
CREATE OR REPLACE VIEW mm_ranked_candidates AS
    SELECT p.party_id, p.region, p.queue_ids,
           extract(epoch FROM now() - p.ready_since)::double precision AS wait_s,
           jsonb_agg(jsonb_build_object('a', m.account_id, 'n', m.username, 'c', m.client_id,
                                        'elo', coalesce((m.account_data ->> 'elo')::integer, 0),
                                        'elo2', coalesce((m.account_data ->> 'elo2')::integer, 0),
                                        'elo4', coalesce((m.account_data ->> 'elo4')::integer, 0))
                     ORDER BY m.joined_at) AS members
      FROM mm_ranked_parties p JOIN mm_ranked_members m USING (party_id)
     WHERE p.status = 'Queued' AND p.ready_since IS NOT NULL AND p.ready_since <= now()
     GROUP BY p.party_id;

-- reserve parties for a new match. p_teams = [[party ids of team 0], [team 1]].
-- Returns false (nothing written) when a party changed meanwhile.
CREATE OR REPLACE FUNCTION kn_rq_match_create(p_match text, p_queue text, p_region text, p_brain text,
                                              p_teams jsonb) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
    v_parties text[];
    v_n integer;
BEGIN
    SELECT array_agg(x) INTO v_parties
      FROM jsonb_array_elements(p_teams) t, jsonb_array_elements_text(t) x;
    SELECT count(*) INTO v_n FROM mm_ranked_parties
     WHERE party_id = ANY (v_parties) AND status = 'Queued' AND ready_since IS NOT NULL AND ready_since <= now();
    IF v_n <> cardinality(v_parties) THEN RETURN false; END IF;
    INSERT INTO mm_ranked_matches (match_id, queue_id, region, brain_id) VALUES (p_match, p_queue, p_region, p_brain);
    INSERT INTO mm_ranked_match_players (match_id, account_id, username, team, rating_key, rating_before)
    SELECT p_match, m.account_id, m.username, t.idx - 1, kn_rq_key(p_queue),
           coalesce((m.account_data ->> kn_rq_key(p_queue))::integer, 0)
      FROM jsonb_array_elements(p_teams) WITH ORDINALITY AS t(parties, idx),
           jsonb_array_elements_text(t.parties) pid
      JOIN mm_ranked_members m ON m.party_id = pid;
    UPDATE mm_ranked_parties SET status = 'CreatingGame', status_c = '{}', match_id = p_match, updated_at = now()
     WHERE party_id = ANY (v_parties);
    RETURN true;
END $$;

-- the lobby exists: parties -> Join (status_c = {gameId, host, port, webUrl}),
-- p_tickets = {"<account id>": "<ticket>"}
CREATE OR REPLACE FUNCTION kn_rq_match_ready(p_match text, p_game text, p_status_c jsonb, p_tickets jsonb)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    UPDATE mm_ranked_matches SET status = 'live', game_id = p_game, started_at = now()
     WHERE match_id = p_match AND status = 'creating';
    UPDATE mm_ranked_parties SET status = 'Join', status_c = p_status_c, updated_at = now() WHERE match_id = p_match;
    UPDATE mm_ranked_members m SET join_ticket = p_tickets ->> m.account_id::text
      FROM mm_ranked_parties p WHERE p.party_id = m.party_id AND p.match_id = p_match;
END $$;

CREATE OR REPLACE FUNCTION kn_rq_match_failed(p_match text, p_reason text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    UPDATE mm_ranked_matches SET status = 'aborted', finished_at = now(),
           result = jsonb_build_object('reason', p_reason)
     WHERE match_id = p_match AND status IN ('creating', 'live');
    -- back in the queue, but not matched again for 10 s (no game server free: no hot loop)
    UPDATE mm_ranked_parties SET status = 'Queued', status_c = '{}', match_id = NULL, updated_at = now(),
           ready_since = CASE WHEN ready_since IS NULL THEN NULL ELSE now() + interval '10 seconds' END
     WHERE match_id = p_match;
END $$;

-- close a live match once (idempotent): returns its players, [] when already closed
CREATE OR REPLACE FUNCTION kn_rq_match_finish(p_match text, p_result jsonb) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE
    v_queue text;
BEGIN
    UPDATE mm_ranked_matches SET status = 'done', finished_at = now(), result = p_result
     WHERE match_id = p_match AND status = 'live' RETURNING queue_id INTO v_queue;
    IF NOT FOUND THEN RETURN '[]'::jsonb; END IF;
    RETURN coalesce((SELECT jsonb_agg(jsonb_build_object('accountId', account_id, 'name', username, 'team', team,
                                                         'key', rating_key, 'before', rating_before))
                       FROM mm_ranked_match_players WHERE match_id = p_match), '[]'::jsonb);
END $$;
