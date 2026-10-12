/*
 * services/brain/store.c - Krunker v7.1.4 store data and the prize wheels of the
 * social socket: "spin" (src/data/store.js wheels / TICKET items' spinWheel +
 * getSpnItems; the reply is index.js io_spinResponse), the spin history "spinH"
 * (io_saveSpinHistory), the Free KR wheel "fkr" (store.freeKR, io_skinKR) and the
 * daily reward "clm" (v3.1.1 claimRewardResponse, kept for older clients).
 * v7.1.4's server has no spin code (server/index.ts and socialSocket.ts handle none of
 * these packets): the contract is the one its client implies. Skins: assets/skins.json,
 * the store.skins array in index order. Charging and granting are one transaction
 * (db_accounts.c P_SPIN_MULTI, 0106_store.sql kn_daily_kr).
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

/* ------------------------------------------------------------------ wheels (store.js) */

#define WHEEL_RARITIES 7

typedef struct Wheel {
    char name[32];
    int price;       /* KR per spin */
    int multi;       /* the "Nx Spin" count (wheel.multi), 0 = single spins only */
    int min_level;
    int kr;          /* the Free KR wheel ("fkr") */
    int ticket_only; /* only through its ticket (Ultra) */
    int big_rar;     /* bigRar: a roll of this rarity or above draws from every rarity >= it */
    int season;      /* 0 = config.currentSeason */
    int ticket;      /* ticketIndex, -1 none */
    char lim_type[24], lim_group[32];
    double rarities[WHEEL_RARITIES];
} Wheel;

static const Wheel k_wheels[] = {
    {"Starter", 50, 10, 0, 0, 0, 0, 0, 6423, "", "", {75, 22, 3, 0, 0, 0}},
    {"Elite", 100, 10, 0, 0, 0, 0, 0, 6424, "", "", {50, 30, 15, 5, 0, 0}},
    {"Heroic", 500, 10, 0, 0, 0, 5, 0, 6425, "", "", {0, 48, 35, 14, 2.49, 0.5, 0.01}},
    {"Ultra", 0, 0, 0, 0, 1, 5, 0, 6426, "", "", {0, 28, 53, 13.5, 4.45, 1, 0.05}},
    {"Free KR", 0, 0, 1, 1, 0, 0, 0, -1, "", "", {50, 39, 10, 1, 0, 0}},
};
#define WHEEL_COUNT ((int) (sizeof k_wheels / sizeof k_wheels[0]))

/* store.freeKR[rarity].kr[0] */
static const int k_free_kr[] = {5, 10, 20, 500};
#define FREE_KR_COUNT ((int) (sizeof k_free_kr / sizeof k_free_kr[0]))

/* ------------------------------------------------------------------ skins */

static SkinInfo *s_skins;
static int s_skin_count; /* -1 = load failed */
static Wheel *s_ticket_wheels; /* the spinWheel of TICKET items (SkinInfo.spin_wheel) */
static int s_ticket_wheel_count;

static int json_truthy(const cJSON *v) {
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0) || (cJSON_IsString(v) && v->valuestring[0]);
}

/* a TICKET item's spinWheel object -> s_ticket_wheels[], its index or -1 */
static int ticket_wheel_load(const cJSON *sw, int count) {
    if (!cJSON_IsObject(sw)) return -1;
    if (!s_ticket_wheels && !(s_ticket_wheels = calloc((size_t) count, sizeof *s_ticket_wheels))) return -1;
    Wheel *w = &s_ticket_wheels[s_ticket_wheel_count];
    memset(w, 0, sizeof *w);
    snprintf(w->name, sizeof w->name, "%s", kn_json_str(sw, "name", "Ticket"));
    w->big_rar = (int) kn_json_int(sw, "bigRar", 0);
    w->season = (int) kn_json_int(sw, "season", 0);
    w->ticket = -1;
    snprintf(w->lim_type, sizeof w->lim_type, "%s", kn_json_str(sw, "limType", ""));
    snprintf(w->lim_group, sizeof w->lim_group, "%s", kn_json_str(sw, "limGroup", ""));
    const cJSON *rar = cJSON_GetObjectItemCaseSensitive(sw, "rarities");
    for (int r = 0; r < WHEEL_RARITIES; r++) {
        const cJSON *v = cJSON_GetArrayItem(rar, r);
        w->rarities[r] = cJSON_IsNumber(v) ? v->valuedouble : 0;
    }
    return s_ticket_wheel_count++;
}

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
                k->black_m = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "blackM"));
                k->opensea = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "opensea"));
                k->m_seas = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "mSeas"));
                k->req = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "req"));
                k->spin_wheel = ticket_wheel_load(cJSON_GetObjectItemCaseSensitive(it, "spinWheel"), count);
                const cJSON *lim = cJSON_GetObjectItemCaseSensitive(it, "limT");
                if (cJSON_IsString(lim)) snprintf(k->lim_t, sizeof k->lim_t, "%s", lim->valuestring);
                else if (cJSON_IsTrue(lim) || (cJSON_IsNumber(lim) && lim->valuedouble != 0)) snprintf(k->lim_t, sizeof k->lim_t, "1");
                snprintf(k->lim_g, sizeof k->lim_g, "%s", kn_json_str(it, "limG", ""));
                snprintf(k->name, sizeof k->name, "%s", kn_json_str(it, "name", ""));
            }
            s_skin_count = count;
        }
        cJSON_Delete(arr);
    }
    free(buf);
    fclose(fp);
    if (s_skin_count > 0) KN_INFO(MOD, "%d store skins (%d spin tickets) from %s", s_skin_count, s_ticket_wheel_count, path);
}

const SkinInfo *acct_skin(int64_t index) {
    skins_load();
    return index >= 0 && index < s_skin_count ? &s_skins[index] : NULL;
}

int acct_skin_count(void) {
    skins_load();
    return s_skin_count > 0 ? s_skin_count : 0;
}

/* ------------------------------------------------------------------ the roll */

static double roll100(void) { return (double) (kn_random_u64() % 1000000ULL) / 10000.0; }

/* index of the rarity a roll in [0,100) falls into: the slice the pointer lands on
 * (index.js: the pointer turns to 2PI * (randNum / 100 + 13) + PI / 2 over slices in order) */
static int roll_rarity(const double *rar, int n, double roll) {
    for (int i = 0; i < n; i++) {
        if (roll < rar[i]) return i;
        roll -= rar[i];
    }
    for (int i = n - 1; i >= 0; i--) /* rounding past the last slice: the last non-empty one */
        if (rar[i] > 0) return i;
    return 0;
}

static int current_season(void) { return acct_current_season(); }
/* config.rewardTime: 12 h in v7.1.4 (v3.1.1: 6 h) */
static int64_t reward_time_ms(void) { return kn_config_int(g_acct_brain->cfg, "accounts.reward_time_s", 43200) * 1000; }

/* store.getSpnItems(wheel, rarity, account, wheel.season || config.currentSeason, wheel.bigRar).
 * bigRar is applied to rolls of that rarity and above (the client calls it with the top rarity
 * only; as written it would add the big rarities to every roll). Items with a `req`
 * (checkReq: account age / stats / account values) are left out: KrunkNative does not
 * track those requirements. */
static int spin_items(const Wheel *w, int rarity, int *out, int max) {
    int n = 0;
    const int season = w->season > 0 ? w->season : current_season();
    const int big = w->big_rar > 0 && rarity >= w->big_rar;
    for (int q = 0; q < acct_skin_count() && n < max; q++) {
        const SkinInfo *k = acct_skin(q);
        if (k->limited || k->blocked || k->no_spin || k->black_m || k->opensea || k->illicit || k->req) continue;
        if (!(k->rarity == rarity || (big && k->rarity >= w->big_rar))) continue;
        if (w->lim_type[0] ? strcmp(w->lim_type, k->lim_t) != 0 : k->lim_t[0] != 0) continue;
        if (w->lim_group[0] ? strcmp(w->lim_group, k->lim_g) != 0 : k->lim_g[0] != 0) continue;
        if (!k->m_seas && (k->seas ? k->seas : 1) != season) continue;
        out[n++] = q;
    }
    return n;
}

/* ------------------------------------------------------------------ "spin" */

#define SPIN_MAX 16

typedef struct SpinCtx {
    int wheel;    /* k_wheels index, -1 = a ticket's spinWheel */
    int ticket;   /* the TICKET item redeemed, -1 none */
    int count;    /* rolls */
    int price;    /* KR per roll */
    double pos[SPIN_MAX];
    int skin[SPIN_MAX];
    int64_t kr;
    int index;
} SpinCtx;

static void spin_error(SocialSession *s, const char *err) { social_send_n(s, "spin", 1, cJSON_CreateString(err)); }

static const Wheel *ctx_wheel(const SpinCtx *c) {
    if (c->wheel >= 0) return &k_wheels[c->wheel];
    const SkinInfo *t = acct_skin(c->ticket);
    return t && t->spin_wheel >= 0 ? &s_ticket_wheels[t->spin_wheel] : NULL;
}

static void spin_done(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        spin_error(s, acct_db_error(res, "spin"));
        return;
    }
    /* io_spinResponse(error, randNums, itemIDs, funds, removedSkin): one spin answers with
     * numbers (as v3.1.1 did), a multi-spin with arrays */
    cJSON *pos, *skins;
    if (c->count == 1) {
        pos = cJSON_CreateNumber(c->pos[0]);
        skins = cJSON_CreateNumber(c->skin[0]);
    } else {
        pos = cJSON_CreateArray();
        skins = cJSON_CreateArray();
        for (int i = 0; i < c->count; i++) {
            cJSON_AddItemToArray(pos, cJSON_CreateNumber(c->pos[i]));
            cJSON_AddItemToArray(skins, cJSON_CreateNumber(c->skin[i]));
        }
    }
    social_send_n(s, "spin", 5, cJSON_CreateNull(), pos, skins, cJSON_CreateNumber((double) kn_json_int(v, "funds", 0)),
                  c->ticket >= 0 ? cJSON_CreateNumber(c->ticket) : cJSON_CreateNull());
    cJSON_Delete(v);
}

static void free_kr_error(SocialSession *s, const char *err) {
    /* io_skinKR(lastReward, randNum, freeKRIndex, funds, err) */
    social_send_n(s, "spinKR", 5, cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(),
                  cJSON_CreateString(err));
}

static void free_kr_done(SocialSession *s, kn_db_result *res, void *extra);

static void spin_account(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    const Wheel *w = ctx_wheel(c);
    if (!w || !kn_db_ok(res) || kn_db_rows(res) < 1) {
        if (w && w->kr) free_kr_error(s, acct_db_error(res, "spin"));
        else spin_error(s, acct_db_error(res, "spin"));
        return;
    }
    const int level = acct_level(kn_db_get_int64(res, 0, 0, 0), NULL);
    const int64_t funds = kn_db_get_int64(res, 0, 1, 0);
    char id[24], a1[24], a2[24], a3[24], ref[48];
    SpinCtx *next = malloc(sizeof *next);
    if (!next) return;
    *next = *c;
    if (w->kr) {
        /* Free KR wheel (fkr): KR by rarity, once per config.rewardTime */
        if (level < w->min_level) {
            free(next);
            free_kr_error(s, "Level too low");
            return;
        }
        next->pos[0] = roll100();
        next->index = roll_rarity(w->rarities, WHEEL_RARITIES, next->pos[0]);
        next->kr = k_free_kr[next->index < FREE_KR_COUNT ? next->index : FREE_KR_COUNT - 1];
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
    if (funds < (int64_t) next->price * next->count) {
        free(next);
        spin_error(s, "Not enough KR");
        return;
    }
    /* the rolls: a rarity per pointer, then a random item of it */
    static int items[8192];
    char list[SPIN_MAX * 12] = "";
    size_t used = 0;
    for (int i = 0; i < next->count; i++) {
        next->pos[i] = roll100();
        const int rarity = roll_rarity(w->rarities, WHEEL_RARITIES, next->pos[i]);
        const int n = spin_items(w, rarity, items, (int) (sizeof items / sizeof items[0]));
        if (n <= 0) {
            free(next);
            spin_error(s, "Error generating item");
            return;
        }
        next->skin[i] = items[kn_random_u64() % (uint64_t) n];
        used += (size_t) snprintf(list + used, sizeof list - used, "%s%d", i ? "," : "", next->skin[i]);
    }
    if (next->ticket >= 0) snprintf(ref, sizeof ref, "t%d:0", next->ticket);
    else snprintf(ref, sizeof ref, "%d:%d", next->wheel, next->price);
    social_call(s, &P_SPIN_MULTI,
                 KN_DB_ARGS(acct_itoa(id, s->account_id), ref, acct_itoa(a1, next->price), list,
                            next->ticket >= 0 ? acct_itoa(a2, next->ticket) : NULL),
                 spin_done, next);
}

static void start_spin(SocialSession *s, int wheel, int ticket, int count) {
    SpinCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->wheel = wheel;
    c->ticket = ticket;
    c->count = count;
    c->price = wheel >= 0 ? k_wheels[wheel].price : 0;
    char id[24];
    social_query(s, SQL(.any = "SELECT player_score, player_funds FROM players WHERE player_id = $1"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id)), spin_account, c);
}

/* ["spin", wheelIndex, ticketSkin, multi] (index.js spinButton.onclick: io.send("spin", i, skin, !!multi)):
 * a store wheel (Nx with multi: wheel.multi rolls at price each), or a TICKET item's
 * spinWheel (wheelIndex null; the ticket is consumed) ->
 * ["spin", null, randNum(s), itemID(s), funds, redeemedTicket|null] | ["spin", err] */
void sh_spin(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        spin_error(s, "Not logged in");
        return;
    }
    if (acct_skin_count() <= 0) {
        spin_error(s, "Error generating item");
        return;
    }
    const int64_t ticket = fa_int(f, 2, -1);
    if (ticket >= 0) {
        const SkinInfo *t = acct_skin(ticket);
        if (!t || t->spin_wheel < 0) {
            spin_error(s, "Invalid spin");
            return;
        }
        start_spin(s, -1, (int) ticket, 1);
        return;
    }
    const int64_t wheel = fa_int(f, 1, -1);
    if (wheel < 0 || wheel >= WHEEL_COUNT) {
        spin_error(s, "Invalid spin");
        return;
    }
    const Wheel *w = &k_wheels[wheel];
    if (w->kr || w->ticket_only) { /* the Free KR wheel spins through "fkr"; Ultra through its ticket */
        spin_error(s, "Wheel unavailable");
        return;
    }
    const cJSON *multi = fa(f, 3);
    const int many = w->multi > 1 && (cJSON_IsTrue(multi) || (cJSON_IsNumber(multi) && multi->valuedouble > 1));
    start_spin(s, (int) wheel, -1, many ? (w->multi < SPIN_MAX ? w->multi : SPIN_MAX) : 1);
}

/* ------------------------------------------------------------------ "spinH" (spin history) */

/* the KR a history row cost: ref "<wheel>:<price>" (v7 spins), "t<ticket>:0" (tickets) or
 * "<wheel>" (v3.1.1 spins: Starter / Elite / Heroic kept their indices and prices) */
static int64_t history_kr(const char *ref) {
    if (!ref || !*ref) return 0;
    const char *colon = strchr(ref, ':');
    if (colon) return strtoll(colon + 1, NULL, 10);
    if (*ref == 't') return 0;
    const long w = strtol(ref, NULL, 10);
    return w >= 0 && w < 3 ? k_wheels[w].price : 0;
}

static void spin_history_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        social_send_n(s, "spinH", 1, cJSON_CreateNull());
        return;
    }
    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < kn_db_rows(res); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ind", (double) kn_db_get_int64(res, i, 0, 0));
        cJSON_AddNumberToObject(o, "dte", kn_db_get_double(res, i, 1, 0)); /* epoch ms: new Date(dte) */
        cJSON_AddNumberToObject(o, "fds", (double) history_kr(kn_db_get(res, i, 2)));
        cJSON_AddItemToArray(rows, o);
    }
    social_send_n(s, "spinH", 1, rows);
}

/* ["spinH", offset] -> ["spinH", [{ind, dte, fds}]]: the account's won items, newest first,
 * 50 per page from `offset` (index.js getSpinHistory / io_saveSpinHistory) */
void sh_spin_history(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "spinH", 1, cJSON_CreateNull());
        return;
    }
    int64_t offset = fa_int(f, 1, 0);
    if (offset < 0) offset = 0;
    char id[24], off[24];
    social_query(s, SQL(.any = "SELECT skin_id, {epoch_ms:at} AS at_ms__f, ref FROM skin_transactions"
                              " WHERE account_id = $1 AND source = 'spin' AND delta > 0"
                              " ORDER BY at DESC, id DESC LIMIT 50 OFFSET {int:$2}"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(off, offset)), spin_history_done, NULL);
}

/* ------------------------------------------------------------------ "fkr" (Free KR wheel) */

static void free_kr_done(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        free_kr_error(s, acct_db_error(res, "fkr"));
        return;
    }
    /* io_skinKR(lastReward, randNum, freeKRIndex, funds, err) */
    social_send_n(s, "spinKR", 4, cJSON_CreateNumber((double) kn_json_int(v, "lastReward", 0)), cJSON_CreateNumber(c->pos[0]),
                  cJSON_CreateNumber(c->index), cJSON_CreateNumber((double) kn_json_int(v, "funds", 0)));
    cJSON_Delete(v);
}

/* ["fkr"] (v3.1.1: ["fkr", captcha]) -> ["spinKR", lastReward, randNum, freeKRIndex, funds] |
 * ["spinKR", null x4, err] */
void sh_free_kr(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        free_kr_error(s, "Not logged in");
        return;
    }
    for (int i = 0; i < WHEEL_COUNT; i++)
        if (k_wheels[i].kr) {
            start_spin(s, i, -1, 1);
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
