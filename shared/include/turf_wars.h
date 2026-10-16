#pragma once
// Krunker v10.0.2 Turf Wars ("territorialWars", v10's only clan war; doc/v10/ranked-impl.md):
// the rules shared by the game server (which kills count, server/src/accounts.c), the brain (the
// daily settlement, services/brain/turf.c) and the client (the Turf Wars window, turf_ui.c).
//
// v10 (bundle class territorialWars + its "How to Play" text): kills by clan members against
// players of another clan (or clanless players) on public FFA / TDM games (no host, or mimicPubs)
// of Burg (0), Sandstorm (2) and Subzero (3) count. Days start at 00:00 UTC, day 0 = 2026-03-17. At the end of
// a day the clan with the most kills on a map controls it the next day; only the top 20
// contributors per clan and map count; the day's kills on a map become a KR pool that the top 3
// clans split 60 / 30 / 10, each clan sharing its cut by its loot config. The pool size per kill
// is server-side in v10: KrunkNative pays TURF_KR_PER_KILL (brain.turf.kr_per_kill).

#include <stdint.h>

#define TURF_INTRO_UNIX 1773705600LL   // TERRITORY_WARS_INTRO_DATE "2026-03-17" 00:00 UTC
#define TURF_MAP_COUNT 3
#define TURF_TOP_CONTRIBUTORS 20       // "Only the top 20 contributors (by kills) per map per clan count"
#define TURF_SPLIT_COUNT 3
#define TURF_KR_PER_KILL 1             // KrunkNative default pool per counted kill
extern const int g_turf_maps[TURF_MAP_COUNT];          // 0 Burg, 2 Sandstorm, 3 Subzero
extern const double g_turf_split[TURF_SPLIT_COUNT];    // territorialWarsRewardSplit [0.6, 0.3, 0.1]
const char *turf_map_name(int map_index);              // "Burg", NULL when not a turf map

// getDayNumber: max(0, floor((now - intro) / 86400 s))
int turf_day_at(int64_t unix_s);
// isEligibleMap + isEligibleGame: a pub-type game (game_is_pub_type: no host, or mimicPubs; the
// caller passes it as pub_game), not a published map, not ranked, on a turf map in FFA (mode 0) or
// TDM (mode 1).
int turf_game_eligible(int map_index, int mode_id, int pub_game, int custom_map, int ranked);
// onPlayerDeath: the kill counts when the killer is a real player with an account and a clan, and
// the victim is not of that clan. Clans are compared case-insensitively ("" = none).
int turf_kill_counts(const char *killer_clan, int killer_is_bot, int killer_has_account, const char *victim_clan);
// cut of the place-th (0-based) clan of a pool, floor(pool * split)
int64_t turf_cut(int64_t pool, int place);

// Loot distribution modes (v10 clan settings "Turf Wars Loot Distribution")
enum { TURF_LOOT_CLAN_BANK, TURF_LOOT_EVEN, TURF_LOOT_TOP_WEIGHTED, TURF_LOOT_PERFORMANCE, TURF_LOOT_TOP_PERFORMANCE, TURF_LOOT_COUNT };
typedef struct {
    int mode;               // TURF_LOOT_*
    int clan_pct;           // clan treasury cut 0..100 (clan_bank: all of it)
    int top_player_count;   // 1..20 (20)
    int top_player_pct;     // 0..100 (80)
    int remainder_pct;      // 0..100 (20)
    int min_games;          // min_games_required (1)
} TurfLoot;
TurfLoot turf_loot_default(void);
int turf_loot_mode(const char *name);       // "even" -> TURF_LOOT_EVEN, -1 unknown
const char *turf_loot_mode_name(int mode);  // "clan_bank" ...
void turf_loot_clamp(TurfLoot *loot);

// Shares a clan's cut among its eligible members (sorted by kills, descending, already filtered by
// min_games): out[i] KR for member i; returns what goes to the clan bank. Totals always add up to cut.
int64_t turf_loot_split(const TurfLoot *loot, int64_t cut, const int *kills, int count, int64_t *out);
