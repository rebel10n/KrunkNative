-- 0104_social_functions: feed / chat / gifts / clans / market / trades operations
-- of the social socket (services/brain/social_data.c).  Owner: svc-accounts.
-- Every function raises SQLSTATE P0001 with the user-facing MESSAGE on refusal;
-- user text is stored raw and HTML-escaped (kn_html) when served, because the
-- v3.1.1 social page inserts it with innerHTML.

CREATE OR REPLACE FUNCTION kn_html(t text) RETURNS text
LANGUAGE sql IMMUTABLE AS $fn$
    SELECT replace(replace(replace(replace(replace(coalesce(t, ''), '&', '&amp;'), '<', '&lt;'), '>', '&gt;'),
                   '"', '&quot;'), '''', '&#39;')
$fn$;

CREATE OR REPLACE FUNCTION kn_fail(msg text) RETURNS void
LANGUAGE plpgsql AS $fn$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = msg;
END
$fn$;

CREATE OR REPLACE FUNCTION kn_level(score bigint, rank_var double precision DEFAULT 0.03) RETURNS integer
LANGUAGE sql IMMUTABLE AS $fn$
    SELECT greatest(1, floor(rank_var * sqrt(greatest(score, 0)))::integer)
$fn$;

CREATE OR REPLACE FUNCTION kn_account_id(p_name text) RETURNS integer
LANGUAGE sql STABLE AS $fn$
    SELECT id FROM accounts WHERE lower(username) = lower(p_name)
$fn$;

-- ---------------------------------------------------------------- feed

-- Post object of the social page's Je() renderer.
CREATE OR REPLACE FUNCTION kn_post_json(p_post bigint, p_viewer integer) RETURNS jsonb
LANGUAGE sql STABLE AS $fn$
    SELECT jsonb_build_object(
        'pid', fp.id, 'pln', r.player_name, 'pli', fp.author_id,
        'prm', r.player_premium, 'ftr', r.player_featured,
        'cln', r.player_clan, 'clr', r.clan_rank, 'flw', r.player_followed,
        'txt', kn_html(fp.body), 'typ', fp.type,
        'lnk', kn_html(fp.link), 'lnk2', kn_html(fp.link2),
        'tim', floor(extract(epoch FROM now() - fp.created_at) * 1000)::bigint,
        'vot', (SELECT count(*) FROM feed_likes l WHERE l.post_id = fp.id),
        'cnt', (SELECT count(*) FROM feed_comments c WHERE c.post_id = fp.id),
        'dlk', CASE WHEN EXISTS (SELECT 1 FROM feed_likes l WHERE l.post_id = fp.id AND l.account_id = p_viewer)
                    THEN 1 ELSE 0 END)
    FROM feed_posts fp JOIN player_rows r ON r.player_id = fp.author_id
    WHERE fp.id = p_post AND fp.deleted_at IS NULL
$fn$;

-- Feed page: posts of followed players and the viewer (newest first); everyone's
-- for guests or when that is empty. p_author restricts to one author ("guf").
CREATE OR REPLACE FUNCTION kn_feed(p_viewer integer, p_author integer, p_limit integer, p_offset integer)
RETURNS jsonb LANGUAGE plpgsql STABLE AS $fn$
DECLARE
    v jsonb;
BEGIN
    IF p_author IS NOT NULL AND p_author > 0 THEN
        SELECT coalesce(jsonb_agg(kn_post_json(id, p_viewer) ORDER BY id DESC), '[]'::jsonb) INTO v FROM (
            SELECT id FROM feed_posts WHERE author_id = p_author AND deleted_at IS NULL
            ORDER BY id DESC LIMIT p_limit OFFSET p_offset) q;
        RETURN v;
    END IF;
    IF p_viewer > 0 THEN
        SELECT coalesce(jsonb_agg(kn_post_json(id, p_viewer) ORDER BY id DESC), '[]'::jsonb) INTO v FROM (
            SELECT fp.id FROM feed_posts fp
            WHERE fp.deleted_at IS NULL
              AND (fp.author_id = p_viewer OR fp.author_id IN (SELECT followee_id FROM follows WHERE follower_id = p_viewer))
            ORDER BY fp.id DESC LIMIT p_limit OFFSET p_offset) q;
        IF jsonb_array_length(v) > 0 THEN
            RETURN v;
        END IF;
    END IF;
    SELECT coalesce(jsonb_agg(kn_post_json(id, p_viewer) ORDER BY id DESC), '[]'::jsonb) INTO v FROM (
        SELECT id FROM feed_posts WHERE deleted_at IS NULL ORDER BY id DESC LIMIT p_limit OFFSET p_offset) q;
    RETURN v;
END
$fn$;

-- "gps": {p: post, c: comments[]} (comments oldest first, same object shape).
CREATE OR REPLACE FUNCTION kn_post_full(p_post bigint, p_viewer integer) RETURNS jsonb
LANGUAGE sql STABLE AS $fn$
    SELECT CASE WHEN kn_post_json(p_post, p_viewer) IS NULL THEN NULL ELSE jsonb_build_object(
        'p', kn_post_json(p_post, p_viewer),
        'c', coalesce((SELECT jsonb_agg(jsonb_build_object(
                    'pid', c.post_id, 'cid', c.id, 'pln', r.player_name, 'pli', c.author_id,
                    'prm', r.player_premium, 'ftr', r.player_featured, 'cln', r.player_clan,
                    'clr', r.clan_rank, 'flw', r.player_followed, 'txt', kn_html(c.body),
                    'tim', floor(extract(epoch FROM now() - c.created_at) * 1000)::bigint) ORDER BY c.id)
                FROM feed_comments c JOIN player_rows r ON r.player_id = c.author_id
                WHERE c.post_id = p_post), '[]'::jsonb)) END
$fn$;

-- ---------------------------------------------------------------- KR gifts ("kr")

CREATE OR REPLACE FUNCTION kn_gift(p_sender integer, p_receiver text, p_amount bigint, p_fee_percent integer,
    p_min_level integer, p_message text) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_to integer := kn_account_id(p_receiver);
    v_fee bigint := floor(greatest(p_amount, 0) * greatest(p_fee_percent, 0) / 100.0);
    v_funds bigint;
BEGIN
    IF v_to IS NULL THEN PERFORM kn_fail('Player not found'); END IF;
    IF v_to = p_sender THEN PERFORM kn_fail('Cant gift yourself'); END IF;
    IF p_amount IS NULL OR p_amount <= 0 THEN PERFORM kn_fail('Invalid amount'); END IF;
    IF kn_level((SELECT player_score FROM players WHERE player_id = p_sender)) < p_min_level THEN
        PERFORM kn_fail('Must be level ' || p_min_level);
    END IF;
    v_funds := kn_add_funds(p_sender, -(p_amount + v_fee), 'gift-out', v_to::text);
    PERFORM kn_add_funds(v_to, p_amount, 'gift-in', p_sender::text);
    INSERT INTO mail (account_id, sender, subject, body, kr)
    SELECT v_to, a.username, 'KR Gift', coalesce(left(p_message, 300), ''), 0 FROM accounts a WHERE a.id = p_sender;
    RETURN jsonb_build_object('funds', v_funds, 'to', v_to, 'amount', p_amount, 'fee', v_fee);
END
$fn$;

-- ---------------------------------------------------------------- clans

-- Menu clan window data (index.js windows[12].clanData).
CREATE OR REPLACE FUNCTION kn_clan_data(p_clan integer) RETURNS jsonb
LANGUAGE sql STABLE AS $fn$
    SELECT jsonb_build_object(
        'clan_id', c.clan_id, 'clan_name', c.clan_name, 'clan_playerid', c.clan_playerid,
        'clan_funds', c.clan_funds, 'clan_score', c.clan_score, 'clan_rank', c.clan_rank,
        'clan_motd', kn_html(c.clan_motd), 'clan_contracts', 0,
        'creatorname', (SELECT username FROM accounts WHERE id = c.clan_playerid),
        'members', coalesce((SELECT jsonb_agg(jsonb_build_object(
                'i', m.account_id, 'p', r.player_name, 'r', m.rank, 's', m.score,
                'su', CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END, 've', r.player_featured,
                'h', r.player_hack, 'k', m.kills, 'de', m.deaths) ORDER BY m.score DESC)
            FROM clan_members m JOIN player_rows r ON r.player_id = m.account_id
            WHERE m.clan_id = c.clan_id), '[]'::jsonb),
        'requests', coalesce((SELECT jsonb_agg(jsonb_build_object(
                'player_name', a.username, 'cr_playerid', q.account_id) ORDER BY q.created_at)
            FROM clan_requests q JOIN accounts a ON a.id = q.account_id
            WHERE q.clan_id = c.clan_id), '[]'::jsonb))
    FROM clans c WHERE c.clan_id = p_clan
$fn$;

CREATE OR REPLACE FUNCTION kn_clan_of(p_player integer) RETURNS integer
LANGUAGE sql STABLE AS $fn$
    SELECT clan_id FROM clan_members WHERE account_id = p_player
$fn$;

-- Leader (creator) or a member with perms (Captain 2 / Commander 3).
CREATE OR REPLACE FUNCTION kn_clan_officer(p_player integer, p_clan integer) RETURNS boolean
LANGUAGE sql STABLE AS $fn$
    SELECT EXISTS (SELECT 1 FROM clans WHERE clan_id = p_clan AND clan_playerid = p_player)
        OR EXISTS (SELECT 1 FROM clan_members WHERE clan_id = p_clan AND account_id = p_player AND rank >= 2)
$fn$;

CREATE OR REPLACE FUNCTION kn_clan_create(p_player integer, p_tag text, p_tag_max integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer;
BEGIN
    p_tag := btrim(coalesce(p_tag, ''));
    IF length(p_tag) < 1 OR length(p_tag) > p_tag_max OR p_tag !~ '^[A-Za-z0-9]+$' THEN
        PERFORM kn_fail('Invalid Clan Name');
    END IF;
    IF kn_clan_of(p_player) IS NOT NULL THEN PERFORM kn_fail('Already in a Clan'); END IF;
    IF EXISTS (SELECT 1 FROM clans WHERE lower(clan_name) = lower(p_tag)) THEN PERFORM kn_fail('Clan Name Taken'); END IF;
    INSERT INTO clans (clan_name, clan_playerid) VALUES (p_tag, p_player) RETURNING clan_id INTO v_clan;
    INSERT INTO clan_members (account_id, clan_id, rank) VALUES (p_player, v_clan, 3);
    DELETE FROM clan_requests WHERE account_id = p_player;
    RETURN kn_clan_data(v_clan);
END
$fn$;

CREATE OR REPLACE FUNCTION kn_clan_request(p_player integer, p_tag text) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer;
BEGIN
    IF kn_clan_of(p_player) IS NOT NULL THEN PERFORM kn_fail('Already in a Clan'); END IF;
    SELECT clan_id INTO v_clan FROM clans WHERE lower(clan_name) = lower(btrim(coalesce(p_tag, '')));
    IF v_clan IS NULL THEN PERFORM kn_fail('Clan not found'); END IF;
    INSERT INTO clan_requests (clan_id, account_id) VALUES (v_clan, p_player) ON CONFLICT DO NOTHING;
    RETURN jsonb_build_object('clan_id', v_clan,
        'leader', (SELECT clan_playerid FROM clans WHERE clan_id = v_clan));
END
$fn$;

CREATE OR REPLACE FUNCTION kn_clan_answer(p_actor integer, p_player integer, p_accept boolean, p_max integer)
RETURNS jsonb LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer := kn_clan_of(p_actor);
BEGIN
    IF v_clan IS NULL OR NOT kn_clan_officer(p_actor, v_clan) THEN PERFORM kn_fail('No Permission'); END IF;
    DELETE FROM clan_requests WHERE clan_id = v_clan AND account_id = p_player;
    IF NOT FOUND THEN PERFORM kn_fail('Request not found'); END IF;
    IF p_accept THEN
        IF kn_clan_of(p_player) IS NOT NULL THEN PERFORM kn_fail('Player already in a Clan'); END IF;
        IF p_max > 0 AND (SELECT count(*) FROM clan_members WHERE clan_id = v_clan) >= p_max THEN
            PERFORM kn_fail('Clan is full');
        END IF;
        INSERT INTO clan_members (account_id, clan_id, rank) VALUES (p_player, v_clan, 0);
        DELETE FROM clan_requests WHERE account_id = p_player;
    END IF;
    RETURN kn_clan_data(v_clan);
END
$fn$;

-- Leave; the leader leaving deletes the clan (index.js confDelClan).
CREATE OR REPLACE FUNCTION kn_clan_leave(p_player integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer := kn_clan_of(p_player);
BEGIN
    IF v_clan IS NULL THEN PERFORM kn_fail('Not in a Clan'); END IF;
    IF EXISTS (SELECT 1 FROM clans WHERE clan_id = v_clan AND clan_playerid = p_player) THEN
        DELETE FROM clans WHERE clan_id = v_clan;
        RETURN jsonb_build_object('deleted', 1);
    END IF;
    DELETE FROM clan_members WHERE account_id = p_player;
    RETURN jsonb_build_object('deleted', 0);
END
$fn$;

CREATE OR REPLACE FUNCTION kn_clan_kick(p_actor integer, p_target integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer := kn_clan_of(p_actor);
BEGIN
    IF v_clan IS NULL OR NOT kn_clan_officer(p_actor, v_clan) THEN PERFORM kn_fail('No Permission'); END IF;
    IF p_target = p_actor OR EXISTS (SELECT 1 FROM clans WHERE clan_id = v_clan AND clan_playerid = p_target) THEN
        PERFORM kn_fail('Cant kick this member');
    END IF;
    DELETE FROM clan_members WHERE account_id = p_target AND clan_id = v_clan;
    RETURN kn_clan_data(v_clan);
END
$fn$;

CREATE OR REPLACE FUNCTION kn_clan_role(p_actor integer, p_target integer, p_rank integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer := kn_clan_of(p_actor);
BEGIN
    IF v_clan IS NULL OR NOT EXISTS (SELECT 1 FROM clans WHERE clan_id = v_clan AND clan_playerid = p_actor) THEN
        PERFORM kn_fail('No Permission');
    END IF;
    IF p_rank < 0 OR p_rank > 3 THEN PERFORM kn_fail('Invalid Role'); END IF;
    UPDATE clan_members SET rank = p_rank WHERE account_id = p_target AND clan_id = v_clan;
    IF NOT FOUND THEN PERFORM kn_fail('Member not found'); END IF;
    RETURN jsonb_build_object('i', p_target, 'r', p_rank);
END
$fn$;

-- Deposit (10% fee, index.js calcFee) / withdraw (leader only).
CREATE OR REPLACE FUNCTION kn_clan_funds(p_actor integer, p_amount bigint, p_withdraw boolean) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_clan integer := kn_clan_of(p_actor);
    v_pf bigint;
    v_cf bigint;
BEGIN
    IF v_clan IS NULL THEN PERFORM kn_fail('Not in a Clan'); END IF;
    IF p_amount IS NULL OR p_amount <= 0 THEN PERFORM kn_fail('Invalid amount'); END IF;
    IF p_withdraw THEN
        IF NOT EXISTS (SELECT 1 FROM clans WHERE clan_id = v_clan AND clan_playerid = p_actor) THEN
            PERFORM kn_fail('No Permission');
        END IF;
        UPDATE clans SET clan_funds = clan_funds - p_amount WHERE clan_id = v_clan AND clan_funds >= p_amount
        RETURNING clan_funds INTO v_cf;
        IF NOT FOUND THEN PERFORM kn_fail('Not enough Clan Funds'); END IF;
        v_pf := kn_add_funds(p_actor, p_amount, 'clan-withdraw', v_clan::text);
    ELSE
        v_pf := kn_add_funds(p_actor, -p_amount, 'clan-deposit', v_clan::text);
        UPDATE clans SET clan_funds = clan_funds + (p_amount - floor(p_amount * 0.1)) WHERE clan_id = v_clan
        RETURNING clan_funds INTO v_cf;
    END IF;
    RETURN jsonb_build_object('pf', v_pf, 'cf', v_cf);
END
$fn$;

-- Social page "clan" (src/social.js io_0 "clan").
CREATE OR REPLACE FUNCTION kn_clan_page(p_tag text) RETURNS jsonb
LANGUAGE sql STABLE AS $fn$
    SELECT jsonb_build_object(
        'clan_name', c.clan_name, 'clan_rank', c.clan_rank, 'clan_score', c.clan_score,
        'creatorname', (SELECT username FROM accounts WHERE id = c.clan_playerid),
        'members', coalesce((SELECT jsonb_agg(jsonb_build_object(
                'p', r.player_name, 'r', m.rank, 's', m.score,
                'su', CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END, 've', r.player_featured,
                'h', r.player_hack) ORDER BY m.score DESC)
            FROM clan_members m JOIN player_rows r ON r.player_id = m.account_id
            WHERE m.clan_id = c.clan_id), '[]'::jsonb))
    FROM clans c WHERE lower(c.clan_name) = lower(p_tag)
$fn$;

-- ---------------------------------------------------------------- market

-- "li": list one item; fee = floor(price * pct/100) unless price <= bypass.
CREATE OR REPLACE FUNCTION kn_market_list(p_seller integer, p_skin integer, p_price bigint, p_fee_percent integer,
    p_fee_bypass bigint, p_sale_max bigint, p_max_listings integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_fee bigint;
    v_funds bigint;
    v_cnt integer;
BEGIN
    IF p_price IS NULL OR p_price <= 0 OR p_price > p_sale_max THEN PERFORM kn_fail('Invalid price'); END IF;
    IF p_max_listings > 0 AND (SELECT count(*) FROM market_listings WHERE seller_id = p_seller
                               AND sold_at IS NULL AND cancelled_at IS NULL) >= p_max_listings THEN
        RETURN jsonb_build_object('response', 'MAX');
    END IF;
    v_fee := CASE WHEN p_price <= p_fee_bypass THEN 0 ELSE floor(p_price * greatest(p_fee_percent, 0) / 100.0) END;
    v_cnt := kn_add_skin(p_seller, p_skin, -1, 'market', 'list');
    v_funds := kn_add_funds(p_seller, -v_fee, 'market-fee', p_skin::text);
    INSERT INTO market_listings (seller_id, skin_id, price) VALUES (p_seller, p_skin, p_price);
    RETURN jsonb_build_object('cnt', v_cnt, 'funds', v_funds);
END
$fn$;

-- "uli": 'SOLD' | 'SUCCESS'
CREATE OR REPLACE FUNCTION kn_market_unlist(p_seller integer, p_listing bigint) RETURNS text
LANGUAGE plpgsql AS $fn$
DECLARE
    v_skin integer;
BEGIN
    IF EXISTS (SELECT 1 FROM market_listings WHERE id = p_listing AND seller_id = p_seller AND sold_at IS NOT NULL) THEN
        RETURN 'SOLD';
    END IF;
    UPDATE market_listings SET cancelled_at = now()
    WHERE id = p_listing AND seller_id = p_seller AND sold_at IS NULL AND cancelled_at IS NULL
    RETURNING skin_id INTO v_skin;
    IF NOT FOUND THEN PERFORM kn_fail('Listing not found'); END IF;
    PERFORM kn_add_skin(p_seller, v_skin, 1, 'market', 'unlist:' || p_listing);
    RETURN 'SUCCESS';
END
$fn$;

-- "pi": buy a listing -> {f: buyer funds, i: skin}
CREATE OR REPLACE FUNCTION kn_market_buy(p_buyer integer, p_listing bigint) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    l market_listings%ROWTYPE;
    v_funds bigint;
BEGIN
    SELECT * INTO l FROM market_listings WHERE id = p_listing FOR UPDATE;
    IF NOT FOUND OR l.sold_at IS NOT NULL OR l.cancelled_at IS NOT NULL THEN PERFORM kn_fail('Item Unavailable'); END IF;
    IF l.seller_id = p_buyer THEN PERFORM kn_fail('Cant buy your own item'); END IF;
    v_funds := kn_add_funds(p_buyer, -l.price, 'market-buy', p_listing::text);
    PERFORM kn_add_funds(l.seller_id, l.price, 'market-sale', p_listing::text);
    PERFORM kn_add_skin(p_buyer, l.skin_id, 1, 'market', 'buy:' || p_listing);
    UPDATE market_listings SET buyer_id = p_buyer, sold_at = now() WHERE id = p_listing;
    RETURN jsonb_build_object('f', v_funds, 'i', l.skin_id, 'm', p_listing);
END
$fn$;

-- "qsi": quick sell count items for kr_each KR each -> {f: funds, c: remaining, i: skin}
CREATE OR REPLACE FUNCTION kn_quick_sell(p_player integer, p_skin integer, p_count integer, p_kr_each integer)
RETURNS jsonb LANGUAGE plpgsql AS $fn$
DECLARE
    v_cnt integer;
    v_funds bigint;
BEGIN
    IF p_count IS NULL OR p_count < 1 THEN p_count := 1; END IF;
    v_cnt := kn_add_skin(p_player, p_skin, -p_count, 'quicksell', NULL);
    v_funds := kn_add_funds(p_player, p_count::bigint * greatest(p_kr_each, 0), 'quicksell', p_skin::text);
    RETURN jsonb_build_object('f', v_funds, 'c', v_cnt, 'i', p_skin);
END
$fn$;

-- ---------------------------------------------------------------- trades

-- "ctr": items as skin index arrays (duplicates = count).
CREATE OR REPLACE FUNCTION kn_trade_create(p_sender integer, p_receiver integer, p_give integer[],
    p_take integer[], p_message text, p_max_items integer, p_min_level integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_trade bigint;
BEGIN
    IF p_receiver IS NULL OR NOT EXISTS (SELECT 1 FROM accounts WHERE id = p_receiver) THEN
        PERFORM kn_fail('Recipient not found');
    END IF;
    IF p_receiver = p_sender THEN PERFORM kn_fail('Cant trade with yourself'); END IF;
    IF coalesce(cardinality(p_give), 0) + coalesce(cardinality(p_take), 0) = 0 THEN PERFORM kn_fail('No Items Selected'); END IF;
    IF coalesce(cardinality(p_give), 0) > p_max_items OR coalesce(cardinality(p_take), 0) > p_max_items THEN
        PERFORM kn_fail(p_max_items || ' items per user');
    END IF;
    IF kn_level((SELECT player_score FROM players WHERE player_id = p_sender)) < p_min_level THEN
        PERFORM kn_fail('Must be level ' || p_min_level);
    END IF;
    -- the sender must own what they offer (checked again on accept)
    IF EXISTS (SELECT 1 FROM (SELECT s, count(*) n FROM unnest(p_give) s GROUP BY s) g
               LEFT JOIN inventory i ON i.account_id = p_sender AND i.skin_id = g.s
               WHERE coalesce(i.count, 0) < g.n) THEN
        PERFORM kn_fail('Item not owned');
    END IF;
    INSERT INTO trades (sender_id, receiver_id, message) VALUES (p_sender, p_receiver, left(coalesce(p_message, ''), 200))
    RETURNING id INTO v_trade;
    INSERT INTO trade_items (trade_id, side, skin_id, count)
    SELECT v_trade, 's', s, count(*) FROM unnest(p_give) s GROUP BY s;
    INSERT INTO trade_items (trade_id, side, skin_id, count)
    SELECT v_trade, 'r', s, count(*) FROM unnest(p_take) s GROUP BY s;
    RETURN jsonb_build_object('trid', v_trade, 'receiver', p_receiver);
END
$fn$;

CREATE OR REPLACE FUNCTION kn_trade_accept(p_player integer, p_trade bigint) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    t trades%ROWTYPE;
    it record;
BEGIN
    SELECT * INTO t FROM trades WHERE id = p_trade FOR UPDATE;
    IF NOT FOUND OR t.receiver_id <> p_player THEN PERFORM kn_fail('Trade not found'); END IF;
    IF t.status <> 'open' THEN PERFORM kn_fail('Trade no longer available'); END IF;
    FOR it IN SELECT * FROM trade_items WHERE trade_id = p_trade LOOP
        IF it.side = 's' THEN
            PERFORM kn_add_skin(t.sender_id, it.skin_id, -it.count, 'trade', p_trade::text);
            PERFORM kn_add_skin(t.receiver_id, it.skin_id, it.count, 'trade', p_trade::text);
        ELSE
            PERFORM kn_add_skin(t.receiver_id, it.skin_id, -it.count, 'trade', p_trade::text);
            PERFORM kn_add_skin(t.sender_id, it.skin_id, it.count, 'trade', p_trade::text);
        END IF;
    END LOOP;
    UPDATE trades SET status = 'accepted', closed_at = now() WHERE id = p_trade;
    RETURN jsonb_build_object('trid', p_trade, 'sender', t.sender_id,
        'funds', (SELECT player_funds FROM players WHERE player_id = p_player));
END
$fn$;

-- "trC": receiver declines or sender cancels.
CREATE OR REPLACE FUNCTION kn_trade_close(p_player integer, p_trade bigint) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    t trades%ROWTYPE;
BEGIN
    SELECT * INTO t FROM trades WHERE id = p_trade FOR UPDATE;
    IF NOT FOUND OR (t.receiver_id <> p_player AND t.sender_id <> p_player) THEN PERFORM kn_fail('Trade not found'); END IF;
    IF t.status <> 'open' THEN PERFORM kn_fail('Trade no longer available'); END IF;
    UPDATE trades SET status = CASE WHEN t.receiver_id = p_player THEN 'declined' ELSE 'cancelled' END,
                      closed_at = now() WHERE id = p_trade;
    RETURN jsonb_build_object('trid', p_trade, 'peer', CASE WHEN t.receiver_id = p_player THEN t.sender_id ELSE t.receiver_id END);
END
$fn$;

-- Market "trades" page: [incoming+outgoing open offers, history] rows with
-- {trid, pl, player, buyer, sksell, skbuy, message} (social.js io_0 "trades").
CREATE OR REPLACE FUNCTION kn_trade_rows(p_player integer, p_open boolean) RETURNS jsonb
LANGUAGE sql STABLE AS $fn$
    SELECT coalesce(jsonb_agg(jsonb_build_object(
        'trid', t.id, 'pl', t.sender_id,
        'player', (SELECT username FROM accounts WHERE id = t.sender_id),
        'buyer', (SELECT username FROM accounts WHERE id = t.receiver_id),
        'sksell', coalesce((SELECT string_agg(repeat(i.skin_id || ',', i.count), '') FROM trade_items i
                            WHERE i.trade_id = t.id AND i.side = 's'), ''),
        'skbuy', coalesce((SELECT string_agg(repeat(i.skin_id || ',', i.count), '') FROM trade_items i
                           WHERE i.trade_id = t.id AND i.side = 'r'), ''),
        'message', kn_html(t.message), 'status', t.status) ORDER BY t.id DESC), '[]'::jsonb)
    FROM (SELECT * FROM trades
          WHERE (sender_id = p_player OR receiver_id = p_player)
            AND (CASE WHEN p_open THEN status = 'open' ELSE status <> 'open' END)
          ORDER BY id DESC LIMIT 100) t
$fn$;
