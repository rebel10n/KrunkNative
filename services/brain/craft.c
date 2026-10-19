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
 * A craft: the recipe and the result skin are checked; then one transaction (P_CRAFT) checks the world
 * limit, spends the recipe's junk through junk_spend (the wallet's ledger row is in the same transaction),
 * checks and takes the materials and grants the result (skin_transactions source "craft", ref
 * "craft:<SID>"; the result is the only delta > 0 row of that ref, which is what CNT counts). A refused
 * craft spends no junk, so there is no refund. There is no crafting timer: the skin is granted
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

/* A recipe that costs junk spends it inside P_CRAFT's transaction, through db_procs.h's junk_spend (the
 * wallet's API, amounts in hundredths): a refused craft spends nothing, so there is no refund. */

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

/* (account, ref "craft:<SID>", result skin, materials "skin,count,...", limit, junk in hundredths) -> {skin}:
 * the world limit is checked, the junk spent (junk_spend), the materials checked and taken, and the result
 * granted. One transaction: a refusal leaves no junk spent and no material taken. */
static kn_db_result *run_craft(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *acc_s = P(0), *ref = P(1), *sout_s = P(2), *mats_s = P(3), *limit_s = P(4), *junk_s = P(5);
    if (!acc_s || !ref || !sout_s || !mats_s || !limit_s || !junk_s) {
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
    const int64_t junk_h = strtoll(junk_s, NULL, 10);
    if (junk_h > 0 && !dbx_failed(&x)) {
        int64_t balance = 0;
        // JUNK_INSUFFICIENT writes nothing and is not an error of the wallet: the craft is refused here
        if (junk_spend(&x, acc, junk_h, "craft", &balance) == JUNK_INSUFFICIENT) dbx_refuse(&x, "Not enough Junk");
    }
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

/* $0 account -> {counts, junk}: counts = the copies crafted so far per recipe ref ([{ref, n}]), junk = the
 * account's balance in hundredths */
static kn_db_result *run_craft_counts(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *acc_s = P(0);
    const int64_t acc = acc_s ? strtoll(acc_s, NULL, 10) : 0;
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT ref, COUNT(*) AS n FROM skin_transactions WHERE source = 'craft' AND delta > 0 GROUP BY ref"),
                           0, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "counts", rows ? rows : cJSON_CreateArray());
    cJSON_AddNumberToObject(o, "junk", (double) junk_balance(&x, acc));
    return dbx_json(&x, "craft_counts", o);
}
static const kn_proc P_CRAFT_COUNTS = {"craft_counts", NULL, run_craft_counts};

/* ------------------------------------------------------------------ the frames */

static void push_account_cb(void *ud, const char *err, const cJSON *data) {
    (void) ud;
    if (!err && data) social_push_account_update(data);
}

typedef struct {
    int64_t sid;
} CraftCtx;

static void craft_done(SocialSession *s, kn_db_result *res, void *extra) {
    CraftCtx *k = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
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
    CraftCtx k = {.sid = sid};
    char acc[24], ref[32], sout[24], limit[24], junk[24], mats[160];
    acct_itoa(acc, s->account_id);
    snprintf(ref, sizeof ref, "craft:%lld", (long long) sid);
    snprintf(sout, sizeof sout, "%lld", (long long) r->sout);
    snprintf(limit, sizeof limit, "%lld", (long long) r->cnt);
    snprintf(junk, sizeof junk, "%lld", (long long) r->junk_h);
    mats[0] = 0;
    for (int i = 0; i < r->n_mats; i++) {
        char pair[48];
        snprintf(pair, sizeof pair, "%s%lld,%lld", i ? "," : "", (long long) r->mat_skin[i], (long long) r->mat_count[i]);
        strncat(mats, pair, sizeof mats - strlen(mats) - 1);
    }
    CraftCtx *ctx = malloc(sizeof *ctx); /* social_query frees it after the reply */
    if (!ctx) {
        refuse(s, sid, "Error");
        return;
    }
    *ctx = k;
    /* a refused submit frees ctx itself (social_db) */
    if (social_call(s, &P_CRAFT, KN_DB_ARGS(acc, ref, sout, mats, limit, junk), craft_done, ctx) < 0) refuse(s, sid, "Error");
}

/* ["recps"]: CRAFT_DATA_REQUEST */
static void recps_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *doc = res && kn_db_ok(res) ? acct_db_json(res) : NULL;
    const cJSON *counts = doc ? cJSON_GetObjectItemCaseSensitive(doc, "counts") : NULL;
    const cJSON *junk_j = doc ? cJSON_GetObjectItemCaseSensitive(doc, "junk") : NULL;
    const int64_t junk = cJSON_IsNumber(junk_j) ? (int64_t) junk_j->valuedouble : 0;
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
    cJSON_Delete(doc);
    social_send_n(s, "recps", 2, rows, cJSON_CreateNumber((double) junk / 100.0));
}

void sh_craft_recipes(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        social_send_n(s, "recps", 2, cJSON_CreateNull(), cJSON_CreateNull());
        return;
    }
    recipes_load();
    char acc[24];
    acct_itoa(acc, s->account_id);
    if (social_call(s, &P_CRAFT_COUNTS, KN_DB_ARGS(acc), recps_done, NULL) < 0) recps_done(s, NULL, NULL);
}
