-- 0393_item_styles (PostgreSQL): Krunker v10.0.7's item styles (items.setStyle, POST /items/:id/style
-- {item_id, style}, deob-10.0.7.js:27102-27106; the style picker 108817-108850; an owned entry's m.style picks the
-- item's styles[n] thumbnail and the in-game texture suffix _s<n>, 108866-108870, 173602) behind kn_brain
-- (services/brain/db_economy.c run_item_style). Additive only.
--
-- item_styles: one row per styled unit of an account, keyed as item_locks (0364): a stack (copy_id 0: the item's
-- unpainted units) or one painted copy (copy_id: its skin_copies id). style is the index into the item's styles
-- (1..n-1; style 0, the default, has no row). The two styled items of v10.0.7 (KrunkCup Jerseys and the KrunkCup
-- Participation Band) are not tradable, sellable or quick-sellable, so a style never moves with a unit.
CREATE TABLE item_styles (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     integer NOT NULL,
    copy_id     bigint NOT NULL DEFAULT 0,
    style       integer NOT NULL CHECK (style > 0),
    styled_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, skin_id, copy_id)
);
