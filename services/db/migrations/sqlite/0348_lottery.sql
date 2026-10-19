-- 0348_lottery (SQLite): Krunker v10.0.2's lottery / raffle (v10 lottery window, GET /lottery/<id>,
-- POST /lottery/<id>/participate; client/src/ui/v10_misc_popups.c). Owner: services/brain/db_lottery.c,
-- lottery.c. Additive only. Windows end at k * frequency_h hours + offset_s (UTC), a draw settles
-- when its window has ended, and the seed rows are the three lotteries of the captured v10 API
-- (pool skin lists are the skins those draws paid; v10's full pool lists are not public).
CREATE TABLE lotteries (
    lottery_id   INTEGER PRIMARY KEY,
    title        TEXT NOT NULL,
    theme        TEXT NOT NULL DEFAULT 'neutral',
    frequency_h  INTEGER NOT NULL CHECK (frequency_h > 0),
    offset_s     INTEGER NOT NULL DEFAULT 0 CHECK (offset_s >= 0 AND offset_s < frequency_h * 3600),
    active       INTEGER NOT NULL DEFAULT 0 CHECK (active IN (0, 1))
);

-- one entry bundle: cost_quantity of the cost (skins; KR; Junk hundredths; RP) gives num_entries
CREATE TABLE lottery_costs (
    lottery_id    INTEGER NOT NULL REFERENCES lotteries(lottery_id) ON DELETE CASCADE,
    ord           INTEGER NOT NULL,
    cost_type     TEXT NOT NULL CHECK (cost_type IN ('skin', 'currency')),
    cost_ref      TEXT NOT NULL,
    cost_quantity INTEGER NOT NULL CHECK (cost_quantity > 0),
    num_entries   INTEGER NOT NULL CHECK (num_entries > 0),
    PRIMARY KEY (lottery_id, ord)
);

-- a pool pays `winners` prizes per draw, each a skin of the pool (lottery_pool_skins)
CREATE TABLE lottery_pools (
    lottery_id   INTEGER NOT NULL REFERENCES lotteries(lottery_id) ON DELETE CASCADE,
    pool_id      INTEGER NOT NULL,
    winners      INTEGER NOT NULL CHECK (winners > 0),
    PRIMARY KEY (lottery_id, pool_id)
);

CREATE TABLE lottery_pool_skins (
    lottery_id   INTEGER NOT NULL,
    pool_id      INTEGER NOT NULL,
    skin_id      INTEGER NOT NULL,
    PRIMARY KEY (lottery_id, pool_id, skin_id),
    FOREIGN KEY (lottery_id, pool_id) REFERENCES lottery_pools(lottery_id, pool_id) ON DELETE CASCADE
);

-- one row per window that someone entered (or that a test opened); drawn_s is set once, by the draw
CREATE TABLE lottery_draws (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    lottery_id    INTEGER NOT NULL REFERENCES lotteries(lottery_id),
    starts_s      INTEGER NOT NULL,
    ends_s        INTEGER NOT NULL,
    drawn_s       INTEGER,
    total_entries INTEGER NOT NULL DEFAULT 0 CHECK (total_entries >= 0),
    UNIQUE (lottery_id, ends_s)
);
CREATE INDEX lottery_draws_due_idx ON lottery_draws (drawn_s, ends_s);

CREATE TABLE lottery_entries (
    draw_id      INTEGER NOT NULL REFERENCES lottery_draws(id) ON DELETE CASCADE,
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    entries      INTEGER NOT NULL CHECK (entries > 0),
    PRIMARY KEY (draw_id, account_id)
);

CREATE TABLE lottery_prizes (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    draw_id        INTEGER NOT NULL REFERENCES lottery_draws(id) ON DELETE CASCADE,
    pool_id        INTEGER NOT NULL,
    prize_type     TEXT NOT NULL,
    prize_ref      TEXT NOT NULL,
    prize_quantity INTEGER NOT NULL CHECK (prize_quantity > 0),
    winner_id      INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    won_by_name    TEXT
);
CREATE INDEX lottery_prizes_draw_idx ON lottery_prizes (draw_id, id);

-- the seed: the three lotteries of the captured v10 API (no event is open: active = 0)
INSERT INTO lotteries (lottery_id, title, theme, frequency_h, offset_s, active) VALUES (1, 'Christmas', 'neutral', 8, 3600, 0);
INSERT INTO lotteries (lottery_id, title, theme, frequency_h, offset_s, active) VALUES (2, 'Easter', 'neutral', 3, 7200, 0);
INSERT INTO lotteries (lottery_id, title, theme, frequency_h, offset_s, active) VALUES (3, 'Anniversary', 'neutral', 12, 3600, 0);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (1, 0, 'skin', '4389', 1, 1);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (1, 1, 'skin', '4391', 1, 4);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (1, 2, 'skin', '4390', 1, 32);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (1, 3, 'skin', '4388', 1, 150);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (2, 0, 'skin', '4787', 1, 1);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (2, 1, 'skin', '4810', 1, 5);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (2, 2, 'skin', '7765', 1, 20);
INSERT INTO lottery_costs (lottery_id, ord, cost_type, cost_ref, cost_quantity, num_entries) VALUES (3, 0, 'skin', '7782', 1, 1);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (1, 1, 1);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 1, 2592);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 1, 2695);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 1, 7554);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 1, 7559);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (1, 2, 10);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 2602);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 2632);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 2673);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 2694);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 2697);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7549);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7550);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7555);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7556);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7557);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7558);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7561);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7576);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 2, 7577);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (1, 3, 40);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2581);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2582);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2612);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2622);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2660);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2661);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2671);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2672);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2689);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2691);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2692);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 2693);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7547);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7548);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7563);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7564);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7565);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7566);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7568);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7569);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7570);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7571);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7572);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7573);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7574);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (1, 3, 7575);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (2, 4, 1);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 4, 7767);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 4, 7771);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 4, 7773);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 4, 7775);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (2, 5, 10);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 5, 7766);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 5, 7769);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (2, 6, 40);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 6, 7768);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (2, 6, 7770);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (3, 7, 1);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 418);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 1005);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 1070);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 1406);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 1861);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 2108);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 2437);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 4051);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 5024);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 7, 6836);
INSERT INTO lottery_pools (lottery_id, pool_id, winners) VALUES (3, 8, 9);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 110);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 118);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 272);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 273);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 284);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 285);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 289);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 396);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 448);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 807);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 810);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 1084);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 1961);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 2421);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 3368);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 3445);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 3551);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 3559);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 3623);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 4812);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 4826);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 4886);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 4889);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 4992);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 5009);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 5137);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 5787);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 6140);
INSERT INTO lottery_pool_skins (lottery_id, pool_id, skin_id) VALUES (3, 8, 6150);
