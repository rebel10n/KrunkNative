#pragma once
// Krunker v10.0.4 Candy Heist: the Halloween event (live event 10, 19 Oct to 7 Nov 2026) and its mode "candy" (mode id
// MODE_ID_CANDY = 37, shared/include/modes.h). Bundle: deob-10.0.4.js:19486-19503 (the event), :105905-105929 (the
// mode), :106488 (the mode's place in the mode list).
//
// What this module owns (identical in every game, client and server):
//   - the mode's static config (the game clock, class, the candy score stat, what is switched off); the mode table
//     entry itself is modes.c's, built from these defines
//   - the candy count a score is worth (statForm.candy)
//   - the event's bundled defaults (LiveEvent fields), used only when the live feed has no event 10. The
//     feed is authoritative: a feed entry with the same id replaces these
//   - the event's drop tables (reward skins, the challenge skin, rarity weights, roll, eligible map)
//
// Where the rest lives:
//   - the Hollowmen, door knocks, candy spawns, the Cauldron bank and the Hollowman's Staff counter: map 46's
//     KrunkScript (assets/maps/official/46_candy_heist.json, fetched from gapi.svc.krunker.io/maps/46), run by the
//     KrunkScript engine. The bundle carries none of it.
//   - the challenges, their KR rewards, the titles "Trick-or-Treater" / "Candy Heist Master", the Lil' Cauldron pet:
//     server-side (account data, not in the bundle). Consumed from the official feed when present, never invented here.
#include <live_events.h>

#define CANDY_HEIST_EVENT_ID 10             // event: 0xa (deob-10.0.4.js:19486)
#define CANDY_HEIST_MODE_KEY "candy"        // mode id (:105906)
#define CANDY_HEIST_MODE_NAME "Candy Heist" // name (:105907)
#define CANDY_HEIST_MAP 46                  // eligibleMaps: [0x2e] (:19498)
#define CANDY_HEIST_CLASS 9                 // forceClass 0x9 (:105914)
#define CANDY_HEIST_GAME_TIME 360.0         // gameTime 0x57e40 ms = 360 s (:105913)
#define CANDY_HEIST_REWARD_CHANCE 5         // rewardChance (:19497), a percentage
#define CANDY_HEIST_MIN_SCORE 1             // minScore (:19498). The event's own gate, not a live-event field yet
#define CANDY_HEIST_SCORE_PER_CANDY 10      // statForm.candy = floor(score / 10) (:105922-105925)
#define CANDY_HEIST_LOOT_SLOTS 7            // the rarity weights are store.rarities (7 entries, see live_events.h)
#define CANDY_HEIST_SKIN_COUNT 6            // rewards.skins (:19499)
#define CANDY_HEIST_CHALLENGE_SKIN 0x1f89   // rewards.challengeSkins (:19501): the challenge's reward
#define CANDY_HEIST_START_MS 1792368000000.0 // "October 19, 2026 12:00 AM UTC" (:19491)
#define CANDY_HEIST_END_MS 1794095940000.0   // "November 7, 2026 11:59 PM UTC" (:19492)

// The mode's static config (the fields of the candy object, :105906-105929). Flags are the switches the
// engine reads: noPickups, noScrLimit, noScoreXP, noProfileStats, noWinner, preventCustomGames.
typedef struct {
    const char *key;               // "candy"
    const char *name;              // "Candy Heist"
    const char *alias;             // "candy"
    const char *description;       // description (:105928); the page's string is game.mode.candy.description
    const char *score_stat;        // scoreStat "candy" (:105926)
    const char *score_label;       // scoreLabel "Candy" (:105927)
    const char *leader_stats[1];   // leaderStats ["candy"] (:105920)
    const char *end_stats[4];      // endStats ["sid", "name", "candy", "reward"] (:105919)
    int end_stat_count;
    float game_time;               // seconds (CANDY_HEIST_GAME_TIME)
    int force_class;               // CANDY_HEIST_CLASS
    unsigned char no_pickups;      // noPickups (:105915)
    unsigned char no_score_limit;  // noScrLimit (:105916)
    unsigned char no_score_xp;     // noScoreXP (:105917)
    unsigned char no_profile_stats; // noProfileStats (:105918)
    unsigned char no_winner;       // noWinner (:105921)
    unsigned char prevent_custom;  // preventCustomGames (:105909)
    int event;                     // event: the live event this mode runs in (CANDY_HEIST_EVENT_ID)
} CandyHeistMode;

// The candy mode, as the bundle has it. Read-only.
const CandyHeistMode *candy_heist_mode(void);
// True for the mode key "candy" (the mode list's id, :105906)
int candy_heist_is_mode(const char *key);
// statForm.candy(score) (:105922-105925): floor(score / 10), a missing score counts 0 (score || 0)
int candy_heist_candy(double score);

// The event's bundled defaults, filled into `out` (LiveEvent fields named in live_events.h). The feed's
// event 10 replaces them. `out` is fully written.
void candy_heist_event_defaults(LiveEvent *out);
// The event's reward skins (the popup's tiles and the endGame pool): store indexes, in bundle order.
// Returns the count (CANDY_HEIST_SKIN_COUNT)
int candy_heist_reward_skins(int *out, int max);
// The event's rarity weights (rewards.weights, :19500): [0, 0, 55, 35, 10, 0, 0] by rarity
void candy_heist_reward_weights(double out[CANDY_HEIST_LOOT_SLOTS]);
