-- 0116_v10_rewards: Krunker v10.0.2's one-time signup perks and the v10 Battle Pass fields
-- (client/src/ui/v10_rewards.c, services/brain/rewards.c / db_rewards.c, db_season.c).
-- Owner: svc-accounts (v10-rewards).
--
-- Signup rewards: v10's "Get Signup Rewards" popup promises "Gain 14 Cosmetics + 1 Day premium for
-- free!" and its server answers a new account with POPUP_REGISTER_REWARDS ("prr", skinRewards,
-- premiumRewards: "Welcome to the Family!"). kn_brain grants them once per account (this table's
-- primary key) to accounts created after this migration ran (signup_rewards_epoch: older accounts
-- signed up before v10 had the perks). The cosmetics are accounts.signup_rewards.skins (default:
-- 14 common items, rewards.c); the premium is premium_ms added to players.player_premium (premiumT)
-- until premium_until, when kn_brain takes it back (premium_swept).
CREATE TABLE signup_rewards (
    account_id     integer PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    skins          text NOT NULL DEFAULT '',
    premium_ms     bigint NOT NULL DEFAULT 0 CHECK (premium_ms >= 0),
    premium_until  timestamptz,
    premium_swept  smallint NOT NULL DEFAULT 0,
    granted_at     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX signup_rewards_sweep_idx ON signup_rewards (premium_swept, premium_until);

CREATE TABLE signup_rewards_epoch (
    id     smallint PRIMARY KEY CHECK (id = 1),
    since  timestamptz NOT NULL DEFAULT now()
);
INSERT INTO signup_rewards_epoch (id) VALUES (1) ON CONFLICT (id) DO NOTHING;

-- v10's battle pass payload (windows[5].gen): leveldata "level:bpp,..." (a level's own BPP, else
-- bpp_perlevel), krdata / jnkdata "itemId:amount,..." (KR / junk rewards of a card). Empty for the
-- Season 7 pass: its levels are uniform and its cards are skins.
ALTER TABLE battlepasses ADD COLUMN leveldata text NOT NULL DEFAULT '';
ALTER TABLE battlepasses ADD COLUMN krdata text NOT NULL DEFAULT '';
ALTER TABLE battlepasses ADD COLUMN jnkdata text NOT NULL DEFAULT '';
