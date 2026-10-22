-- 0354_season_10 (MySQL/MariaDB): same rows as postgres/0354_season_10 (see there for the sources).
INSERT IGNORE INTO seasons (season, name) VALUES (10, 'Season 10');

INSERT IGNORE INTO battlepasses (bp_id, season, name, descr, bpp_perlevel, itemcount, tier1_name, tier1_price_kr, skip_price_kr, itemdata, leveldata, krdata, jnkdata, startdate, enddate)
    VALUES (3, 10, 'Season 10 Battle Pass', 'Krunker Season 10', 400, 80, 'VIP', 1200, 200, '',
    '51:500,52:500,53:500,54:500,55:500,56:500,57:500,58:500,59:500,60:500,61:1400,62:1400,63:1400,64:1400,65:1400,66:1400,67:1400,68:1400,69:1400,70:1400,71:1600,72:1600,73:1600,74:1600,75:1600,76:1600,77:1600,78:1600,79:1600,80:1600',
    '', '', CURRENT_TIMESTAMP(3), CURRENT_TIMESTAMP(3) + INTERVAL 180 DAY);
