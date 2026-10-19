/*
 * services/brain/craft.c - the Junkyard's crafting over the social socket (v10's crafting window:
 * CRAFT_DATA_REQUEST "recps" / CRAFT_REQUEST "crft", their responses "recps" / "crftr").
 *
 *   ["recps"]          -> ["recps", rows, junk]     rows: the recipes (below), junk: the account's
 *                                                   junk balance (junk_balance), null when logged out
 *                      -> ["recps", null, null]     logged out, or no recipe table
 *   ["crft", SID]      -> ["crftr", SID, ok, msg]   ok 1 when the result skin is granted
 *
 * A recipe row is v10's recps row: SID (recipe id), SOUT (the result skin's store index), SIN
 * ("skin,count,skin,count,..." the materials, up to 5 pairs), JUNK (the junk price, 2 decimals),
 * DUR (the craft time in seconds, shown only), CNT (copies left in the world; -1 = unlimited) and
 * DAT (the day it was added, ms: a NEW tag for two days). The table is assets/junkyard_recipes.json
 * ({"recipes": [...]} or the bare array; config accounts.junkyard_recipes overrides the path or
 * gives an inline array). A row is kept only when its result skin is craftable in skins.json
 * (store.c craftable), every material is a known skin, and the pairs and prices are sane.
 *
 * A craft: the recipe and the result skin are checked; a recipe that costs junk spends it through
 * junk_spend (one call site below); one transaction (P_CRAFT) then checks the world limit and the
 * materials, takes the materials and grants the result (skin_transactions source "craft", ref
 * "craft:<SID>"; the result is the only delta > 0 row of that ref, which is what CNT counts). A
 * failed craft refunds its junk through junk_credit. There is no crafting timer: the skin is granted
 * at the request (DUR is shown, not waited for). Owner: svc-accounts.
 */
#include "acct_internal.h"
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "craft"
#define CRAFT_MAX_MATS 5
#define P(i) (n > (i) ? p[i] : NULL)

/* ------------------------------------------------------------------ the junk wallet call site */

/* The wallet (doc/v10/junk.md) owns these; amounts are hundredths of a junk. The weak definitions
 * below answer "no junk" until the wallet links in: a recipe that costs junk is refused, a free one
 * still crafts. Keep every junk call in this block. */
typedef enum { JUNK_SPEND_OK = 0, JUNK_SPEND_INSUFFICIENT = 1 } JunkSpendResult;
int64_t junk_balance(int64_t account_id);
JunkSpendResult junk_spend(int64_t account_id, int64_t amount_hundredths, const char *reason);
void junk_credit(int64_t account_id, int64_t amount_hundredths, const char *reason);

__attribute__((weak)) int64_t junk_balance(int64_t account_id) {
    (void) account_id;
    return 0;
}
__attribute__((weak)) JunkSpendResult junk_spend(int64_t account_id, int64_t amount_hundredths, const char *reason) {
    (void) account_id;
    (void) amount_hundredths;
    (void) reason;
    return JUNK_SPEND_INSUFFICIENT;
}
__attribute__((weak)) void junk_credit(int64_t account_id, int64_t amount_hundredths, const char *reason) {
    (void) account_id;
    (void) amount_hundredths;
    (void) reason;
}

/* ------------------------------------------------------------------ the recipe table */

typedef struct {
    int64_t sid, sout;
    int n_mats;
    int64_t mat_skin[CRAFT_MAX_MATS], mat_count[CRAFT_MAX_MATS];
    double junk;     /* JUNK as written */
    int64_t junk_h;  /* hundredths, what the wallet takes */
    int64_t dur, cnt, dat;
    char sin[160];   /* SIN as written (sent back as is) */
} CraftRecipe;

static CraftRecipe *s_recipes;
static int s_recipe_count = -1; /* -1: not loaded yet */

static int64_t num_of(const cJSON *o, const char *k, int64_t def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (int64_t) v->valuedouble;
    if (cJSON_IsString(v)) return strtoll(v->valuestring, NULL, 10);
    return def;
}

/* "skin,count,..." -> the pairs; false when a pair is malformed, a skin unknown or too many */
static int parse_mats(CraftRecipe *r, const char *sin) {
    char buf[sizeof r->sin];
    snprintf(buf, sizeof buf, "%s", sin);
    r->n_mats = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    while (tok) {
        char *cnt = strtok_r(NULL, ",", &save);
        if (!cnt || r->n_mats >= CRAFT_MAX_MATS) return 0;
        const int64_t skin = strtoll(tok, NULL, 10), count = strtoll(cnt, NULL, 10);
        if (skin < 0 || count < 1 || !acct_skin(skin)) return 0;
        r->mat_skin[r->n_mats] = skin;
        r->mat_count[r->n_mats] = count;
        r->n_mats++;
        tok = strtok_r(NULL, ",", &save);
    }
    return r->n_mats > 0;
}

/* one recps row -> the recipe, or false (logged, skipped) */
static int load_row(const cJSON *it, CraftRecipe *r) {
    memset(r, 0, sizeof *r);
    r->sid = num_of(it, "SID", 0);
    r->sout = num_of(it, "SOUT", -1);
    const cJSON *sin = cJSON_GetObjectItemCaseSensitive(it, "SIN"), *junk = cJSON_GetObjectItemCaseSensitive(it, "JUNK");
    const char *sin_s = cJSON_IsString(sin) ? sin->valuestring : NULL;
    const double jv = cJSON_IsNumber(junk) ? junk->valuedouble : cJSON_IsString(junk) ? strtod(junk->valuestring, NULL) : 0;
    r->dur = num_of(it, "DUR", 0);
    r->cnt = num_of(it, "CNT", -1);
    r->dat = num_of(it, "DAT", 0);
    const SkinInfo *out = r->sout >= 0 ? acct_skin(r->sout) : NULL;
    if (r->sid <= 0 || !out || !out->craftable || !sin_s || jv < 0 || r->dur < 0 || r->cnt < -1 || strlen(sin_s) >= sizeof r->sin) {
        KN_WARN(MOD, "recipe %lld skipped: no craftable result, or a malformed row", (long long) r->sid);
        return 0;
    }
    snprintf(r->sin, sizeof r->sin, "%s", sin_s);
    if (!parse_mats(r, sin_s)) {
        KN_WARN(MOD, "recipe %lld skipped: its materials (SIN) are malformed or unknown", (long long) r->sid);
        return 0;
    }
    r->junk = jv;
    r->junk_h = (int64_t) (jv * 100.0 + 0.5);
    return 1;
}

static void recipes_load(void) {
    if (s_recipe_count >= 0 || !g_acct_brain) return;
    s_recipe_count = 0;
    const cJSON *cfg = kn_config_get(g_acct_brain->cfg, "accounts.junkyard_recipes");
    cJSON *doc = NULL;
    if (cJSON_IsArray(cfg) || cJSON_IsObject(cfg)) {
        doc = cJSON_Duplicate(cfg, 1);
    } else {
        char path[1024];
        const char *rel = kn_config_str(g_acct_brain->cfg, "accounts.junkyard_recipes", "assets/junkyard_recipes.json");
        if (!kn_config_resolve_path(g_acct_brain->cfg, rel, path, sizeof path)) return;
        FILE *fp = fopen(path, "rb");
        if (!fp) return;
        fseek(fp, 0, SEEK_END);
        const long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *buf = n > 0 ? malloc((size_t) n + 1) : NULL;
        if (buf && fread(buf, 1, (size_t) n, fp) == (size_t) n) {
            buf[n] = 0;
            doc = cJSON_Parse(buf);
        }
        free(buf);
        fclose(fp);
    }
    const cJSON *rows = cJSON_IsArray(doc) ? doc : cJSON_GetObjectItemCaseSensitive(doc, "recipes");
    const int count = cJSON_GetArraySize(rows);
    if (count > 0 && (s_recipes = calloc((size_t) count, sizeof *s_recipes)) != NULL) {
        const cJSON *it;
        cJSON_ArrayForEach(it, rows) {
            if (load_row(it, &s_recipes[s_recipe_count])) s_recipe_count++;
        }
    }
    cJSON_Delete(doc);
    if (s_recipe_count > 0) KN_INFO(MOD, "%d junkyard recipes loaded", s_recipe_count);
}

static const CraftRecipe *recipe_of(const int64_t sid) {
    recipes_load();
    for (int i = 0; i < s_recipe_count; i++)
        if (s_recipes[i].sid == sid) return &s_recipes[i];
    return NULL;
}

/* ------------------------------------------------------------------ the database */

/* (account, ref "craft:<SID>", result skin, materials "skin,count,...", limit) -> {skin}: the
 * world limit and the materials are checked, then taken, and the result granted. One transaction:
 * a refusal leaves no material taken. */
static kn_db_result *run_craft(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *acc_s = P(0), *ref = P(1), *sout_s = P(2), *mats_s = P(3), *limit_s = P(4);
    if (!acc_s || !ref || !sout_s || !mats_s || !limit_s) {
        dbx_refuse(&x, "Error");
        return dbx_finish(&x, NULL);
    }
    const int64_t acc = strtoll(acc_s, NULL, 10), sout = strtoll(sout_s, NULL, 10), limit = strtoll(limit_s, NULL, 10);
    char acc_t[24];
    snprintf(acc_t, sizeof acc_t, "%lld", (long long) acc);
    /* the players row first: concurrent crafts of one account queue behind it (like redeem) */
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_funds = player_funds WHERE player_id = $1"), KN_DB_ARGS(acc_t));
    if (limit >= 0 && dbx_int(&x, SQL(.any = "SELECT COUNT(*) FROM skin_transactions WHERE source = 'craft' AND ref = $1 AND delta > 0"),
                              KN_DB_ARGS(ref), 0) >= limit)
        dbx_refuse(&x, "Sold out");
    char mats[160], *save = NULL;
    snprintf(mats, sizeof mats, "%s", mats_s);
    for (char *tok = strtok_r(mats, ",", &save); tok && !dbx_failed(&x); tok = strtok_r(NULL, ",", &save)) {
        char *cnt = strtok_r(NULL, ",", &save);
        const int64_t skin = strtoll(tok, NULL, 10), want = cnt ? strtoll(cnt, NULL, 10) : 0;
        char skin_t[24];
        snprintf(skin_t, sizeof skin_t, "%lld", (long long) skin);
        const int64_t have = dbx_int(&x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"),
                                     KN_DB_ARGS(acc_t, skin_t), 0);
        if (!cnt || want < 1 || have < want) dbx_refuse(&x, "Missing materials");
        else db_add_skin(&x, acc, skin, -want, "craft", ref, NULL);
    }
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    db_add_skin(&x, acc, sout, 1, "craft", ref, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "skin", (double) sout);
    return dbx_json(&x, "craft", o);
}
static const kn_proc P_CRAFT = {"craft", NULL, run_craft};

/* the copies crafted so far, per recipe ref: [{ref, n}] */
static kn_db_result *run_craft_counts(kn_dbconn *c, int n, const char *const *p) {
    (void) n;
    (void) p;
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT ref, COUNT(*) AS n FROM skin_transactions WHERE source = 'craft' AND delta > 0 GROUP BY ref"),
                           0, NULL);
    return dbx_json(&x, "craft_counts", rows ? rows : cJSON_CreateArray());
}
static const kn_proc P_CRAFT_COUNTS = {"craft_counts", NULL, run_craft_counts};

/* ------------------------------------------------------------------ the frames */

static void push_account_cb(void *ud, const char *err, const cJSON *data) {
    (void) ud;
    if (!err && data) social_push_account_update(data);
}

typedef struct {
    int64_t sid, account, junk_h;
    int junk_spent;
} CraftCtx;

static void craft_refund(const CraftCtx *k) {
    if (k->junk_spent) junk_credit(k->account, k->junk_h, "craft_refund");
}

static void craft_done(SocialSession *s, kn_db_result *res, void *extra) {
    CraftCtx *k = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        craft_refund(k);
        social_send_n(s, "crftr", 3, cJSON_CreateNumber((double) k->sid), cJSON_CreateNumber(0),
                      cJSON_CreateString(acct_db_error(res, "craft")));
        return;
    }
    cJSON_Delete(v);
    social_send_n(s, "crftr", 3, cJSON_CreateNumber((double) k->sid), cJSON_CreateNumber(1), cJSON_CreateString("Crafted."));
    acct_get(s->account_id, push_account_cb, NULL);
}

static void refuse(SocialSession *s, const int64_t sid, const char *msg) {
    social_send_n(s, "crftr", 3, cJSON_CreateNumber((double) sid), cJSON_CreateNumber(0), cJSON_CreateString(msg));
}

/* ["crft", SID]: CRAFT_REQUEST */
void sh_craft(SocialSession *s, const cJSON *f) {
    const int64_t sid = fa_int(f, 1, -1);
    const CraftRecipe *r = s->account_id > 0 ? recipe_of(sid) : NULL;
    const SkinInfo *out = r ? acct_skin(r->sout) : NULL;
    if (s->account_id <= 0) {
        refuse(s, sid, "Must be logged in");
        return;
    }
    if (!out || !out->craftable) {
        refuse(s, sid, "Not craftable.");
        return;
    }
    CraftCtx k = {.sid = sid, .account = s->account_id, .junk_h = r->junk_h, .junk_spent = 0};
    if (r->junk_h > 0) {
        if (junk_spend(s->account_id, r->junk_h, "craft") != JUNK_SPEND_OK) {
            refuse(s, sid, "Not enough Junk");
            return;
        }
        k.junk_spent = 1;
    }
    char acc[24], ref[32], sout[24], limit[24], mats[160];
    acct_itoa(acc, s->account_id);
    snprintf(ref, sizeof ref, "craft:%lld", (long long) sid);
    snprintf(sout, sizeof sout, "%lld", (long long) r->sout);
    snprintf(limit, sizeof limit, "%lld", (long long) r->cnt);
    mats[0] = 0;
    for (int i = 0; i < r->n_mats; i++) {
        char pair[48];
        snprintf(pair, sizeof pair, "%s%lld,%lld", i ? "," : "", (long long) r->mat_skin[i], (long long) r->mat_count[i]);
        strncat(mats, pair, sizeof mats - strlen(mats) - 1);
    }
    CraftCtx *ctx = malloc(sizeof *ctx); /* social_query frees it after the reply */
    if (!ctx) {
        craft_refund(&k);
        refuse(s, sid, "Error");
        return;
    }
    *ctx = k;
    /* a refused submit frees ctx itself (social_db) */
    if (social_call(s, &P_CRAFT, KN_DB_ARGS(acc, ref, sout, mats, limit), craft_done, ctx) < 0) {
        craft_refund(&k);
        refuse(s, sid, "Error");
    }
}

/* ["recps"]: CRAFT_DATA_REQUEST */
static void recps_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *counts = res && kn_db_ok(res) ? acct_db_json(res) : NULL;
    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < s_recipe_count; i++) {
        const CraftRecipe *r = &s_recipes[i];
        int64_t cnt = r->cnt;
        if (cnt >= 0 && counts) {
            char ref[32];
            snprintf(ref, sizeof ref, "craft:%lld", (long long) r->sid);
            const cJSON *e;
            cJSON_ArrayForEach(e, counts) {
                const char *eref = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "ref"));
                if (eref && !strcmp(eref, ref)) cnt = r->cnt - num_of(e, "n", 0);
            }
            if (cnt < 0) cnt = 0;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "SID", (double) r->sid);
        cJSON_AddNumberToObject(o, "SOUT", (double) r->sout);
        cJSON_AddStringToObject(o, "SIN", r->sin);
        cJSON_AddNumberToObject(o, "JUNK", r->junk);
        cJSON_AddNumberToObject(o, "DUR", (double) r->dur);
        cJSON_AddNumberToObject(o, "CNT", (double) cnt);
        cJSON_AddNumberToObject(o, "DAT", (double) r->dat);
        cJSON_AddItemToArray(rows, o);
    }
    cJSON_Delete(counts);
    const int64_t junk = junk_balance(s->account_id);
    social_send_n(s, "recps", 2, rows, cJSON_CreateNumber((double) junk / 100.0));
}

void sh_craft_recipes(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        social_send_n(s, "recps", 2, cJSON_CreateNull(), cJSON_CreateNull());
        return;
    }
    recipes_load();
    int limited = 0;
    for (int i = 0; i < s_recipe_count; i++) limited += s_recipes[i].cnt >= 0;
    if (!limited) {
        recps_done(s, NULL, NULL);
        return;
    }
    if (social_call(s, &P_CRAFT_COUNTS, 0, NULL, recps_done, NULL) < 0) recps_done(s, NULL, NULL);
}
