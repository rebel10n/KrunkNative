-- 0106_store: prize wheels ("spin"), the free KR wheel / daily reward ("fkr", "clm")
-- of Krunker v3.1.1 (src/data/store.js wheels / freeKR, index.js spinResponse /
-- freeKrSpinResponse / claimRewardResponse).  The skin roll happens in C
-- (services/brain/store.c, store.getSpnItems over assets/skins.json); these
-- functions charge and grant atomically.  Owner: svc-accounts.

ALTER TABLE players ADD COLUMN player_lastfreespin bigint NOT NULL DEFAULT 0;  -- ms, the "Free" wheel

-- Paid / free wheel spin: charge p_price, grant skin p_skin.  p_cooldown_ms > 0 limits
-- free wheels to one spin per cooldown.  -> {funds, skin, count}
CREATE OR REPLACE FUNCTION kn_spin(p_id integer, p_wheel integer, p_price bigint, p_skin integer,
    p_cooldown_ms bigint, p_now_ms bigint) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_funds bigint;
    v_count integer;
BEGIN
    IF p_cooldown_ms > 0 THEN
        UPDATE players SET player_lastfreespin = p_now_ms
        WHERE player_id = p_id AND player_lastfreespin <= p_now_ms - p_cooldown_ms;
        IF NOT FOUND THEN PERFORM kn_fail('Spin Error 5'); END IF;
    END IF;
    v_funds := kn_add_funds(p_id, -p_price, 'spin', p_wheel::text);
    v_count := kn_add_skin(p_id, p_skin, 1, 'spin', p_wheel::text);
    RETURN jsonb_build_object('funds', v_funds, 'skin', p_skin, 'count', v_count);
END
$fn$;

-- Free KR wheel / daily reward: once per p_cooldown_ms since player_lastreward.
-- -> {lastReward, funds}
CREATE OR REPLACE FUNCTION kn_daily_kr(p_id integer, p_kr bigint, p_cooldown_ms bigint, p_now_ms bigint,
    p_source text) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_funds bigint;
BEGIN
    UPDATE players SET player_lastreward = p_now_ms
    WHERE player_id = p_id AND player_lastreward <= p_now_ms - p_cooldown_ms;
    IF NOT FOUND THEN PERFORM kn_fail('Reward not ready yet'); END IF;
    v_funds := kn_add_funds(p_id, greatest(p_kr, 0), p_source, NULL);
    RETURN jsonb_build_object('lastReward', p_now_ms, 'funds', v_funds);
END
$fn$;
