#pragma once
// The table columns of each game mode: `teams`, `multiTeam`, `endStats` and `leaderStats` as v10.0.7's mode configs
// define them (deob-10.0.7.js:104764-106655; the v7.1.4 gameModes/*.js keys). This is data only, so a program that
// does not link the game (server/bridge, the v10 bridge) reads the same columns the server's g_mode_configs carries:
// each MODE_COLUMNS_<KEY> below is the designated-initializer list of both g_mode_configs[id] (shared/src/modes/modes.c)
// and g_mode_columns[id] (shared/src/modes/mode_columns.c), so the two cannot diverge.
#include <modes.h>

typedef struct {
    unsigned char teams:1;           // mode.teams
    int multi_team;                  // multiTeam (sdm 4), 0 = 2 teams when `teams`
    const char *const *end_stats;    // endStats ("sid" first), NULL = config.endStats (mode_columns_end_stats)
    const char *const *leader_stats; // leaderStats, NULL = ["score"] (mode_columns_leader_stats)
} ModeColumns;

// The columns of mode `id`, NULL outside 0 .. MODE_COUNT - 1.
const ModeColumns *mode_columns(int id);
// endStats with the default applied: config.endStats ["sid", "name", "score", "kills", "deaths", "reward"]
// (deob-10.0.7.js:18314) for a mode without its own; NULL-terminated. NULL for an unknown id.
const char *const *mode_columns_end_stats(int id);
// leaderStats with the default ["score"] (players.syncLeaders, deob-10.0.7.js:176110); NULL for an unknown id.
const char *const *mode_columns_leader_stats(int id);

// The cells the server, the KN client and the bridge all format, kept here so the three agree.
// Candy Heist's statForm.candy: Math.floor((score || 0) / 10) (deob-10.0.7.js:106114-106118).
int mode_columns_candy(int score);
// statForm.time: player.timer || "DNF" (:18319-18321) from MODE_END's time stat (tenths + 1, 0 none). Race's timer is the
// clock text when the player finished (lastTimer, getTime with showMS: "MM:SS.t", :104868); Raid's onEnd sets
// getTime(ms) with no fraction ("MM:SS", :105253; getTime :23928-23944).
void mode_columns_time_text(int mode_id, int time_stat, char *out, size_t size);

extern const char *const g_mode_end_stats_default[];
extern const char *const g_mode_end_stats_tdm[];
extern const char *const g_mode_end_stats_point[];
extern const char *const g_mode_end_stats_obj[];
extern const char *const g_mode_end_stats_ctf[];
extern const char *const g_mode_end_stats_blitz[];
extern const char *const g_mode_end_stats_bhop[];
extern const char *const g_mode_end_stats_hide[];
extern const char *const g_mode_end_stats_infect[];
extern const char *const g_mode_end_stats_race[];
extern const char *const g_mode_end_stats_lms[];
extern const char *const g_mode_end_stats_simon[];
extern const char *const g_mode_end_stats_gun[];
extern const char *const g_mode_end_stats_prop[];
extern const char *const g_mode_end_stats_stalk[];
extern const char *const g_mode_end_stats_oinc[];
extern const char *const g_mode_end_stats_trade[];
extern const char *const g_mode_end_stats_kc[];
extern const char *const g_mode_end_stats_def[];
extern const char *const g_mode_end_stats_raid[];
extern const char *const g_mode_end_stats_depo[];
extern const char *const g_mode_end_stats_depoffa[];
extern const char *const g_mode_end_stats_md[];
extern const char *const g_mode_end_stats_zom[];
extern const char *const g_mode_end_stats_candy[];

extern const char *const g_mode_leader_stats_score[];
extern const char *const g_mode_leader_stats_skd[];
extern const char *const g_mode_leader_stats_sk[];
extern const char *const g_mode_leader_stats_point[];
extern const char *const g_mode_leader_stats_ctf[];
extern const char *const g_mode_leader_stats_gun[];
extern const char *const g_mode_leader_stats_depo[];
extern const char *const g_mode_leader_stats_oinc[];
extern const char *const g_mode_leader_stats_kc[];
extern const char *const g_mode_leader_stats_dom[];
extern const char *const g_mode_leader_stats_obj[];
extern const char *const g_mode_leader_stats_depoffa[];
extern const char *const g_mode_leader_stats_md[];
extern const char *const g_mode_leader_stats_zom[];
extern const char *const g_mode_leader_stats_candy[];

// One per mode, in GameModeId order (the v10 line of the mode's config).
#define MODE_COLUMNS_FFA .leader_stats = g_mode_leader_stats_skd                                                       // :106125
#define MODE_COLUMNS_TDM .teams = 1, .end_stats = g_mode_end_stats_tdm, .leader_stats = g_mode_leader_stats_skd        // :106132
#define MODE_COLUMNS_POINT .teams = 1, .end_stats = g_mode_end_stats_point, .leader_stats = g_mode_leader_stats_point  // :104764
#define MODE_COLUMNS_CTF .teams = 1, .end_stats = g_mode_end_stats_ctf, .leader_stats = g_mode_leader_stats_ctf        // :106141
#define MODE_COLUMNS_BHOP .end_stats = g_mode_end_stats_bhop                                                           // :104816
#define MODE_COLUMNS_HIDE .teams = 1, .end_stats = g_mode_end_stats_hide                                               // :106161
#define MODE_COLUMNS_INFECT .teams = 1, .end_stats = g_mode_end_stats_infect                                           // :106206
#define MODE_COLUMNS_RACE .end_stats = g_mode_end_stats_race                                                           // :104843
#define MODE_COLUMNS_LMS .teams = 1, .end_stats = g_mode_end_stats_lms                                                 // :106249
#define MODE_COLUMNS_SIMON .teams = 1, .end_stats = g_mode_end_stats_simon                                             // :106268
#define MODE_COLUMNS_GUN .end_stats = g_mode_end_stats_gun, .leader_stats = g_mode_leader_stats_gun                    // :106292
#define MODE_COLUMNS_PROP .teams = 1, .end_stats = g_mode_end_stats_prop                                               // :104900
#define MODE_COLUMNS_BOSS .teams = 1                                                                                   // :106336
#define MODE_COLUMNS_CLAS .leader_stats = g_mode_leader_stats_skd                                                      // :106363
#define MODE_COLUMNS_DEPO .teams = 1, .end_stats = g_mode_end_stats_depo, .leader_stats = g_mode_leader_stats_depo     // :104942
#define MODE_COLUMNS_STALK .teams = 1, .end_stats = g_mode_end_stats_stalk                                             // :105035
#define MODE_COLUMNS_KING .end_stats = g_mode_end_stats_obj                                                            // :105123
#define MODE_COLUMNS_OINC .end_stats = g_mode_end_stats_oinc, .leader_stats = g_mode_leader_stats_oinc                 // :105162
#define MODE_COLUMNS_TRADE .end_stats = g_mode_end_stats_trade                                                         // :106378
#define MODE_COLUMNS_KC .teams = 1, .end_stats = g_mode_end_stats_kc, .leader_stats = g_mode_leader_stats_kc           // :106393
#define MODE_COLUMNS_DEF .teams = 1, .end_stats = g_mode_end_stats_def                                                 // :106430
#define MODE_COLUMNS_SHRP .end_stats = g_mode_end_stats_def, .leader_stats = g_mode_leader_stats_skd                   // :106495
#define MODE_COLUMNS_TRAI .teams = 1                                                                                   // :106514
#define MODE_COLUMNS_RAID .end_stats = g_mode_end_stats_raid                                                           // :105221
#define MODE_COLUMNS_BLITZ .teams = 1, .end_stats = g_mode_end_stats_blitz                                             // :106556
#define MODE_COLUMNS_DOM .teams = 1, .end_stats = g_mode_end_stats_ctf, .leader_stats = g_mode_leader_stats_dom        // :105299
#define MODE_COLUMNS_SDM .teams = 1, .multi_team = 4, .end_stats = g_mode_end_stats_tdm                                // :106568
#define MODE_COLUMNS_KRANK .end_stats = g_mode_end_stats_def, .leader_stats = g_mode_leader_stats_skd                  // :105325
#define MODE_COLUMNS_TDF .teams = 1, .end_stats = g_mode_end_stats_obj, .leader_stats = g_mode_leader_stats_obj        // :106575
#define MODE_COLUMNS_DEPOFFA .end_stats = g_mode_end_stats_depoffa, .leader_stats = g_mode_leader_stats_depoffa        // :105353
#define MODE_COLUMNS_CAR .end_stats = g_mode_end_stats_obj, .leader_stats = g_mode_leader_stats_obj                    // :106598
#define MODE_COLUMNS_AON .end_stats = g_mode_end_stats_def, .leader_stats = g_mode_leader_stats_sk                     // :105437
#define MODE_COLUMNS_MD .end_stats = g_mode_end_stats_md, .leader_stats = g_mode_leader_stats_md                       // :105477
#define MODE_COLUMNS_CHS .end_stats = g_mode_end_stats_def, .leader_stats = g_mode_leader_stats_sk                     // :106621
#define MODE_COLUMNS_BHFFA .leader_stats = g_mode_leader_stats_skd                                                     // :105516
#define MODE_COLUMNS_ZOM .end_stats = g_mode_end_stats_zom, .leader_stats = g_mode_leader_stats_zom                    // :105888
#define MODE_COLUMNS_IMP .end_stats = g_mode_end_stats_def, .leader_stats = g_mode_leader_stats_sk                     // :106649
#define MODE_COLUMNS_CANDY .end_stats = g_mode_end_stats_candy, .leader_stats = g_mode_leader_stats_candy              // :106098
