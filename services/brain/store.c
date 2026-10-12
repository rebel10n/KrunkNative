/*
 * services/brain/store.c - Krunker v3.1.1 store data and the prize wheels of the
 * social socket: "spin" (src/data/store.js wheels + getSpnItems, packetHandlers.ts
 * io_spin), the Free KR wheel "fkr" (store.freeKR, index.js freeKrSpinResponse)
 * and the daily reward "clm" (index.js claimRewardResponse, config.rewardTime /
 * rewardMinLvl). Skins: assets/skins.json, the store.skins array in index order.
 * Charging and granting are atomic in SQL (0106_store.sql kn_spin / kn_daily_kr).
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "store"

/* ------------------------------------------------------------------ skins */

static SkinInfo *s_skins;
static int s_skin_count; /* -1 = load failed */

static void skins_load(void) {
    if (s_skins || s_skin_count < 0 || !g_acct_brain) return;
    char path[1024];
    const char *rel = kn_config_str(g_acct_brain->cfg, "accounts.skins_file", "assets/skins.json");
    s_skin_count = -1;
    if (!kn_config_resolve_path(g_acct_brain->cfg, rel, path, sizeof path)) return;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        KN_WARN(MOD, "skins: %s not readable (wheels, quick sell and market filters disabled)", path);
        return;
    }
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, fp) == (size_t) n) {
        buf[n] = 0;
        cJSON *arr = cJSON_Parse(buf);
        const int count = cJSON_GetArraySize(arr);
        if (cJSON_IsArray(arr) && count > 0 && (s_skins = calloc((size_t) count, sizeof *s_skins)) != NULL) {
            /* the array position is the store index (the "id" field is a per-type id) */
            int i = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                SkinInfo *k = &s_skins[i++];
                k->rarity = (int) kn_json_int(it, "rarity", 0);
                k->weapon = (int) kn_json_int(it, "weapon", 0);
                k->type = (int) kn_json_int(it, "type", 0);
                k->seas = (int) kn_json_int(it, "seas", 1);
                k->min_rec = (int) kn_json_int(it, "minRec", 0);
                k->limited = (unsigned char) kn_json_bool(it, "limited", 0);
                k->blocked = (unsigned char) kn_json_bool(it, "blocked", 0);
                k->no_spin = (unsigned char) kn_json_bool(it, "noSpin", 0);
                k->illicit = (unsigned char) kn_json_bool(it, "illicit", 0);
                k->no_sale = (unsigned char) kn_json_bool(it, "noSale", 0);
                k->no_quick_sell = (unsigned char) kn_json_bool(it, "noQuickSell", 0);
                const cJSON *lim = cJSON_GetObjectItemCaseSensitive(it, "limT");
                if (cJSON_IsString(lim)) snprintf(k->lim_t, sizeof k->lim_t, "%s", lim->valuestring);
                else if (cJSON_IsTrue(lim) || (cJSON_IsNumber(lim) && lim->valuedouble != 0)) snprintf(k->lim_t, sizeof k->lim_t, "1");
                snprintf(k->name, sizeof k->name, "%s", kn_json_str(it, "name", ""));
            }
            s_skin_count = count;
        }
        cJSON_Delete(arr);
    }
    free(buf);
    fclose(fp);
    if (s_skin_count > 0) KN_INFO(MOD, "%d store skins from %s", s_skin_count, path);
}

const SkinInfo *acct_skin(int64_t index) {
    skins_load();
    return index >= 0 && index < s_skin_count ? &s_skins[index] : NULL;
}

int acct_skin_count(void) {
    skins_load();
    return s_skin_count > 0 ? s_skin_count : 0;
}

/* ------------------------------------------------------------------ wheels (store.js) */

typedef struct Wheel {
    const char *name;
    int price;
    int min_level;
    int locked;      /* not spinnable (Valour) */
    int free;        /* free wheel: once per cooldown */
    int kr;          /* the Free KR wheel ("fkr") */
    int twitch;
    const char *lim_type;
    double rarities[7];
} Wheel;

static const Wheel k_wheels[] = {
    {"Starter", 50, 0, 0, 0, 0, 0, NULL, {75, 22, 3, 0, 0, 0}},
    {"Elite", 100, 0, 0, 0, 0, 0, NULL, {50, 30, 15, 5, 0, 0}},
    {"Heroic", 500, 0, 0, 0, 0, 0, NULL, {0, 48, 35, 14, 2.5, 0.5}},
    {"Valour", 0, 0, 1, 0, 0, 0, NULL, {43, 33, 16, 6, 2, 0}},
    {"Free", 0, 5, 0, 1, 0, 0, NULL, {0, 48, 35, 14, 3, 0}},
    {"Free KR", 0, 1, 0, 1, 1, 0, NULL, {51, 19, 15, 8, 5, 1.9, 0.1}},
    {"Twitch", 0, 0, 0, 0, 0, 1, "Charity", {0, 0, 50, 35, 15, 0}},
};
#define WHEEL_COUNT ((int) (sizeof k_wheels / sizeof k_wheels[0]))

/* store.freeKR[i].kr[level >= 20] */
static const int k_free_kr[7][2] = {{15, 35}, {25, 60}, {40, 120}, {100, 200}, {160, 450}, {1000, 2000}, {4000, 10000}};

static double roll100(void) { return (double) (kn_random_u64() % 1000000ULL) / 10000.0; }

/* index of the rarity a roll in [0,100) falls into (packetHandlers io_spin) */
static int roll_rarity(const double *rar, int n, double roll) {
    for (int i = 0; i < n; i++) {
        if (roll < rar[i]) return i;
        roll -= rar[i];
    }
    return 0;
}

static int current_season(void) { return acct_current_season(); }
static int64_t reward_time_ms(void) { return kn_config_int(g_acct_brain->cfg, "accounts.reward_time_s", 21600) * 1000; }

/* store.getSpnItems(wheel, rarity, account, config) */
static int spin_items(const Wheel *w, int rarity, int level, int *out, int max) {
    int n = 0;
    const int season = current_season();
    for (int q = 0; q < acct_skin_count() && n < max; q++) {
        const SkinInfo *k = acct_skin(q);
        if (k->limited || k->blocked || k->no_spin || k->illicit || k->rarity != rarity) continue;
        if (w->lim_type ? strcmp(w->lim_type, k->lim_t) != 0 : k->lim_t[0] != 0) continue;
        if (k->min_rec && !(level < k->min_rec)) continue;
        if ((k->seas ? k->seas : 1) != season) continue;
        out[n++] = q;
    }
    return n;
}

/* ------------------------------------------------------------------ "spin" */

typedef struct SpinCtx {
    int wheel;
    double pos;
    int skin;
    int64_t kr;
    int index;
} SpinCtx;

static void spin_error(SocialSession *s, const char *err) { social_send_n(s, "spin", 1, cJSON_CreateString(err)); }

static void spin_done(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        spin_error(s, acct_db_error(res, "spin"));
        return;
    }
    /* spinResponse(error, randNum, itemID, funds, follow) */
    social_send_n(s, "spin", 5, cJSON_CreateNull(), cJSON_CreateNumber(c->pos), cJSON_CreateNumber(c->skin),
                  cJSON_CreateNumber((double) kn_json_int(v, "funds", 0)), cJSON_CreateNumber(0));
    cJSON_Delete(v);
}

static void free_kr_done(SocialSession *s, kn_db_result *res, void *extra);

static void spin_account(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    const Wheel *w = &k_wheels[c->wheel];
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        if (w->kr) social_send_n(s, "spinKR", 5, cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(),
                                 cJSON_CreateString(acct_db_error(res, "spin")));
        else spin_error(s, acct_db_error(res, "spin"));
        return;
    }
    const int level = acct_level(kn_db_get_int64(res, 0, 0, 0), NULL);
    const int64_t funds = kn_db_get_int64(res, 0, 1, 0);
    char id[24], a1[24], a2[24], a3[24], a4[24], a5[24];
    SpinCtx *next = malloc(sizeof *next);
    if (!next) return;
    *next = *c;
    next->pos = roll100();
    if (w->kr) {
        /* Free KR wheel (fkr): KR by rarity, once per config.rewardTime */
        if (level < w->min_level) {
            free(next);
            social_send_n(s, "spinKR", 5, cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(),
                          cJSON_CreateString("Level too low"));
            return;
        }
        next->index = roll_rarity(w->rarities, 7, next->pos);
        next->kr = k_free_kr[next->index][level >= 20];
        social_call(s, &P_DAILY_KR,
                     KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(a1, next->kr), acct_itoa(a2, reward_time_ms()),
                                acct_itoa(a3, kn_wall_ms())),
                     free_kr_done, next);
        return;
    }
    if (level < w->min_level) {
        free(next);
        spin_error(s, "Level too low");
        return;
    }
    if (funds < w->price) {
        free(next);
        spin_error(s, "Not enough KR");
        return;
    }
    const int rarity = roll_rarity(w->rarities, 7, next->pos);
    static int items[4096];
    const int n = spin_items(w, rarity, level, items, 4096);
    if (n <= 0) {
        free(next);
        spin_error(s, "Error generating item");
        return;
    }
    next->skin = items[kn_random_u64() % (uint64_t) n];
    const int64_t cooldown = w->free ? kn_config_int(g_acct_brain->cfg, "accounts.free_wheel_cooldown_s", 21600) * 1000 : 0;
    social_call(s, &P_SPIN,
                 KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(a1, next->wheel), acct_itoa(a2, w->price),
                            acct_itoa(a3, next->skin), acct_itoa(a4, cooldown), acct_itoa(a5, kn_wall_ms())),
                 spin_done, next);
}

static void start_spin(SocialSession *s, int wheel) {
    SpinCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->wheel = wheel;
    char id[24];
    social_query(s, SQL(.any = "SELECT player_score, player_funds FROM players WHERE player_id = $1"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id)), spin_account, c);
}

/* ["spin", wheelIndex, twitchToken?] -> ["spin", null, randNum, skinIndex, funds, followInd] | ["spin", err] */
void sh_spin(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        spin_error(s, "Not logged in");
        return;
    }
    const int64_t wheel = fa_int(f, 1, -1);
    if (wheel < 0 || wheel >= WHEEL_COUNT) {
        spin_error(s, "Invalid spin");
        return;
    }
    const Wheel *w = &k_wheels[wheel];
    if (w->locked) {
        spin_error(s, "Wheel unavailable");
        return;
    }
    if (w->twitch) {
        spin_error(s, "Twitch is not linked on this server");
        return;
    }
    if (acct_skin_count() <= 0) {
        spin_error(s, "Error generating item");
        return;
    }
    start_spin(s, (int) wheel);
}

/* ------------------------------------------------------------------ "fkr" (Free KR wheel) */

static void free_kr_done(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "spinKR", 5, cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(),
                      cJSON_CreateString(acct_db_error(res, "fkr")));
        return;
    }
    /* freeKrSpinResponse(lastReward, randNum, rewardIndex, funds, error) */
    social_send_n(s, "spinKR", 4, cJSON_CreateNumber((double) kn_json_int(v, "lastReward", 0)), cJSON_CreateNumber(c->pos),
                  cJSON_CreateNumber(c->index), cJSON_CreateNumber((double) kn_json_int(v, "funds", 0)));
    cJSON_Delete(v);
}

/* ["fkr", captcha] -> ["spinKR", lastReward, randNum, rewardIndex, funds] | ["spinKR", null x4, err] */
void sh_free_kr(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        social_send_n(s, "spinKR", 5, cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(),
                      cJSON_CreateString("Not logged in"));
        return;
    }
    for (int i = 0; i < WHEEL_COUNT; i++)
        if (k_wheels[i].kr) {
            start_spin(s, i);
            return;
        }
}

/* ------------------------------------------------------------------ "clm" (daily reward) */

static void claim_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "clm")));
        return;
    }
    /* claimRewardResponse(lastReward, funds) */
    social_send_n(s, "clm", 2, cJSON_CreateNumber((double) kn_json_int(v, "lastReward", 0)),
                  cJSON_CreateNumber((double) kn_json_int(v, "funds", 0)));
    cJSON_Delete(v);
}

/* ["clm"] (level >= config.rewardMinLvl, once per config.rewardTime) -> ["clm", lastReward, funds] */
void sh_claim(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString("Must be logged in"));
        return;
    }
    char id[24], kr[24], cd[24], now[24], lvl[24], rv[32];
    snprintf(rv, sizeof rv, "%.17g", g_acct.rank_var);
    social_call(s, &P_CLAIM_DAILY,
                 KN_DB_ARGS(acct_itoa(id, s->account_id),
                            acct_itoa(kr, kn_config_int(g_acct_brain->cfg, "accounts.daily_reward_kr", 50)),
                            acct_itoa(cd, reward_time_ms()), acct_itoa(now, kn_wall_ms()),
                            acct_itoa(lvl, kn_config_int(g_acct_brain->cfg, "accounts.reward_min_level", 5)),
                            rv),
                 claim_done, NULL);
}

void sh_noop(SocialSession *s, const cJSON *f) {
    (void) s;
    (void) f;
}
