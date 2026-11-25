// The mode table's columns (mode_columns.h): data and cell text, no game code, so server/bridge links it alone.
#include <mode_columns.h>
#include <stddef.h>
#include <stdio.h>

// config.endStats (deob-10.0.7.js:18314): every mode without its own endStats
const char *const g_mode_end_stats_default[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL};
// endStats ("sid" first, as in the mode files; the end table skips it)
const char *const g_mode_end_stats_tdm[] = {"sid", "name", "score", "kills", "deaths", "assists", "reward", NULL};
const char *const g_mode_end_stats_point[] = {"sid", "name", "score", "kills", "deaths", "obj", "dmg", "reward", NULL};
const char *const g_mode_end_stats_obj[] = {"sid", "name", "score", "kills", "deaths", "obj", "reward", NULL};
const char *const g_mode_end_stats_ctf[] = {"sid", "name", "score", "kills", "deaths", "caps", "reward", NULL};
const char *const g_mode_end_stats_blitz[] = {"sid", "name", "score", "kills", "deaths", "caps", NULL};
const char *const g_mode_end_stats_bhop[] = {"sid", "name", "score", "deaths", NULL};
const char *const g_mode_end_stats_hide[] = {"sid", "name", "score", "found", "reward", NULL};
const char *const g_mode_end_stats_infect[] = {"sid", "name", "score", "kills", "infected", "reward", NULL};
const char *const g_mode_end_stats_race[] = {"sid", "name", "score", "time", NULL};
const char *const g_mode_end_stats_lms[] = {"sid", "name", "score", "kills", "deaths", "assists", NULL};
const char *const g_mode_end_stats_simon[] = {"sid", "name", "score", "reward", NULL};
const char *const g_mode_end_stats_gun[] = {"sid", "name", "weapon", "kills", "melees", "setbacks", "reward", NULL};
const char *const g_mode_end_stats_prop[] = {"sid", "name", "score", "kills", NULL};
const char *const g_mode_end_stats_stalk[] = {"sid", "name", "score", "reward", NULL};
const char *const g_mode_end_stats_oinc[] = {"sid", "name", "kills", "deaths", "melees", NULL};
const char *const g_mode_end_stats_trade[] = {"sid", "name", "score", NULL};
const char *const g_mode_end_stats_kc[] = {"sid", "name", "score", "kills", "deaths", "confirms", "denies", "reward", NULL};
const char *const g_mode_end_stats_def[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL};
const char *const g_mode_end_stats_raid[] = {"sid", "name", "score", "time", "dmg", "reward", NULL};
const char *const g_mode_end_stats_depo[] = {"sid", "name", "score", "kills", "deaths", "deposits", "denies", "reward", NULL};
const char *const g_mode_end_stats_depoffa[] = {"sid", "name", "score", "kills", "deaths", "deposits", "reward", NULL};
const char *const g_mode_end_stats_md[] = {"sid", "name", "score", "kills", "deaths", "pickups", "reward", NULL};
const char *const g_mode_end_stats_zom[] = {"sid", "name", "score", "zombies", "downs", "rounds", "reward", NULL};
const char *const g_mode_end_stats_candy[] = {"sid", "name", "candy", "reward", NULL};

// leaderStats; ["score"] is syncLeaders' default (deob-10.0.7.js:176110)
const char *const g_mode_leader_stats_score[] = {"score", NULL};
const char *const g_mode_leader_stats_skd[] = {"score", "kills", "deaths", NULL};
const char *const g_mode_leader_stats_sk[] = {"score", "kills", NULL};
const char *const g_mode_leader_stats_point[] = {"score", "objective", "kills", NULL};
const char *const g_mode_leader_stats_ctf[] = {"caps", "score", NULL};
const char *const g_mode_leader_stats_gun[] = {"weaponTier", "kills", NULL};
const char *const g_mode_leader_stats_depo[] = {"deposits", "kills", "denies", "score", NULL};
const char *const g_mode_leader_stats_oinc[] = {"lives", "kills", NULL};
const char *const g_mode_leader_stats_kc[] = {"confirms", "score", "denies", NULL};
const char *const g_mode_leader_stats_dom[] = {"score", "caps", NULL};
const char *const g_mode_leader_stats_obj[] = {"objective", "score", NULL};
const char *const g_mode_leader_stats_depoffa[] = {"deposits", "kills", "score", NULL};
const char *const g_mode_leader_stats_md[] = {"score", "kills", "pickups", NULL};
const char *const g_mode_leader_stats_zom[] = {"points", "zombies", "downs", "dmg", NULL};
const char *const g_mode_leader_stats_candy[] = {"candy", NULL};

static const ModeColumns g_mode_columns[MODE_COUNT] = {
    [MODE_ID_FFA] = {MODE_COLUMNS_FFA},
    [MODE_ID_TDM] = {MODE_COLUMNS_TDM},
    [MODE_ID_POINT] = {MODE_COLUMNS_POINT},
    [MODE_ID_CTF] = {MODE_COLUMNS_CTF},
    [MODE_ID_BHOP] = {MODE_COLUMNS_BHOP},
    [MODE_ID_HIDE] = {MODE_COLUMNS_HIDE},
    [MODE_ID_INFECT] = {MODE_COLUMNS_INFECT},
    [MODE_ID_RACE] = {MODE_COLUMNS_RACE},
    [MODE_ID_LMS] = {MODE_COLUMNS_LMS},
    [MODE_ID_SIMON] = {MODE_COLUMNS_SIMON},
    [MODE_ID_GUN] = {MODE_COLUMNS_GUN},
    [MODE_ID_PROP] = {MODE_COLUMNS_PROP},
    [MODE_ID_BOSS] = {MODE_COLUMNS_BOSS},
    [MODE_ID_CLAS] = {MODE_COLUMNS_CLAS},
    [MODE_ID_DEPO] = {MODE_COLUMNS_DEPO},
    [MODE_ID_STALK] = {MODE_COLUMNS_STALK},
    [MODE_ID_KING] = {MODE_COLUMNS_KING},
    [MODE_ID_OINC] = {MODE_COLUMNS_OINC},
    [MODE_ID_TRADE] = {MODE_COLUMNS_TRADE},
    [MODE_ID_KC] = {MODE_COLUMNS_KC},
    [MODE_ID_DEF] = {MODE_COLUMNS_DEF},
    [MODE_ID_SHRP] = {MODE_COLUMNS_SHRP},
    [MODE_ID_TRAI] = {MODE_COLUMNS_TRAI},
    [MODE_ID_RAID] = {MODE_COLUMNS_RAID},
    [MODE_ID_BLITZ] = {MODE_COLUMNS_BLITZ},
    [MODE_ID_DOM] = {MODE_COLUMNS_DOM},
    [MODE_ID_SDM] = {MODE_COLUMNS_SDM},
    [MODE_ID_KRANK] = {MODE_COLUMNS_KRANK},
    [MODE_ID_TDF] = {MODE_COLUMNS_TDF},
    [MODE_ID_DEPOFFA] = {MODE_COLUMNS_DEPOFFA},
    [MODE_ID_CAR] = {MODE_COLUMNS_CAR},
    [MODE_ID_AON] = {MODE_COLUMNS_AON},
    [MODE_ID_MD] = {MODE_COLUMNS_MD},
    [MODE_ID_CHS] = {MODE_COLUMNS_CHS},
    [MODE_ID_BHFFA] = {MODE_COLUMNS_BHFFA},
    [MODE_ID_ZOM] = {MODE_COLUMNS_ZOM},
    [MODE_ID_IMP] = {MODE_COLUMNS_IMP},
    [MODE_ID_CANDY] = {MODE_COLUMNS_CANDY},
};

const ModeColumns *mode_columns(const int id) {
    return id >= 0 && id < MODE_COUNT ? &g_mode_columns[id] : NULL;
}

const char *const *mode_columns_end_stats(const int id) {
    const ModeColumns *c = mode_columns(id);
    return !c ? NULL : c->end_stats ? c->end_stats : g_mode_end_stats_default;
}

const char *const *mode_columns_leader_stats(const int id) {
    const ModeColumns *c = mode_columns(id);
    return !c ? NULL : c->leader_stats ? c->leader_stats : g_mode_leader_stats_score;
}

int mode_columns_candy(const int score) {
    // C division truncates toward 0; Math.floor takes -15 / 10 down to -2
    return score >= 0 ? score / 10 : -((-score + 9) / 10);
}

void mode_columns_time_text(const int mode_id, const int time_stat, char *out, const size_t size) {
    if (!out || !size) return;
    if (time_stat <= 0) {
        snprintf(out, size, "DNF");
        return;
    }
    const int tenths = time_stat - 1;
    if (mode_id == MODE_ID_RAID) snprintf(out, size, "%02d:%02d", tenths / 600 % 60, tenths / 10 % 60);
    else snprintf(out, size, "%02d:%02d.%d", tenths / 600 % 60, tenths / 10 % 60, tenths % 10);
}
