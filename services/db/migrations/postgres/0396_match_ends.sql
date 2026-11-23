-- 0396_match_ends (PostgreSQL): the end table of every finished match, for Share Match Results. Krunker v10.0.7's end
-- table share button (deob-10.0.7.js:226776, shareResult / shareMatch 227048-227064: SHARE_MATCH_RESULT with the note)
-- becomes a MatchResult feed post (typ 8, deob-10.0.7.js:26213; the hub draws lnk = the mode index's endStats over
-- lnk2's flat rows, hub-10.0.7.js:4913-4936). The game server's final commit carries v10's endData.ed
-- (deob-10.0.7.js:195285-195292); kn_brain keeps it here per match (internal_ws.c), and "smr" posts the sharer's latest
-- one (db_social_v10.c run_share_match). Additive only.
CREATE TABLE match_ends (
    match_id    text PRIMARY KEY,
    mode        integer NOT NULL DEFAULT 0,
    ed          text NOT NULL,
    ended_at    timestamptz NOT NULL DEFAULT now()
);
