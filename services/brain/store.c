/*
 * services/brain/store.c - Krunker v10.0.2 store data and the prize wheels of the social
 * socket: "spin" (v10's store.wheels and item pools from gapi /data/spins, the TICKET items'
 * spinWheel / spinWheelId / spinWheelName, store.getSpnItems; the reply is index.js
 * io_spinResponse), the spin history "spinH" (io_saveSpinHistory), the Free KR wheel "fkr"
 * (store.freeKR, io_skinKR) and the daily reward "clm" (v3.1.1 claimRewardResponse, kept for
 * older clients). v10 spins over gapi POST /spins/execute {index, ticket, multi}; the social
 * socket frame carries the same three values. Skins: assets/skins.json, the store.skins array in
 * index order. Wheels: shared/include/store_spins.h (the compiled-in v10 set), replaced by the
 * store_spin_data row when there is one (0118_store_v10: rotate wheels without a release), served
 * as GET /api/data/spins. Charging and granting are one transaction (db_accounts.c
 * P_SPIN_MULTI, 0106_store.sql kn_daily_kr).
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <store_spins.h>
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "store"

/* store.freeKR[rarity].kr[0] */
static const int k_free_kr[] = {5, 10, 20, 500};
#define FREE_KR_COUNT ((int) (sizeof k_free_kr / sizeof k_free_kr[0]))

/* ------------------------------------------------------------------ wheels (gapi /data/spins) */

static StoreSpins s_db_spins;   /* the store_spin_data row, when there is one */
static int s_db_spins_set;
static char *s_db_spins_text;   /* its JSON, as stored */
static kn_timer_id s_spins_timer;

static const StoreSpins *spins(void) { return s_db_spins_set ? &s_db_spins : store_spins_builtin(); }

/* the season of a wheel without one: accounts.store_season, else the spin data's (v10: 10) */
static int store_season(void) {
    const int d = spins()->season > 0 ? spins()->season : acct_current_season();
    const int64_t v = g_acct_brain ? kn_config_int(g_acct_brain->cfg, "accounts.store_season", d) : d;
    return v > 0 && v < 1000 ? (int) v : d;
}

static void spins_row_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) return; /* no table yet (migration pending): the built-in wheels */
    const char *text = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
    if (!text || !*text) {
        if (s_db_spins_set) KN_INFO(MOD, "store_spin_data cleared: the built-in v10 wheels");
        store_spins_free(&s_db_spins);
        s_db_spins_set = 0;
        free(s_db_spins_text);
        s_db_spins_text = NULL;
        return;
    }
    if (s_db_spins_text && !strcmp(s_db_spins_text, text)) return;
    StoreSpins next;
    if (store_spins_parse(text, &next) != 0 || next.wheel_count <= 0) {
        KN_WARN(MOD, "store_spin_data: not spin data (kept the current wheels)");
        store_spins_free(&next);
        return;
    }
    store_spins_free(&s_db_spins);
    s_db_spins = next;
    s_db_spins_set = 1;
    free(s_db_spins_text);
    s_db_spins_text = strdup(text);
    KN_INFO(MOD, "store_spin_data: %d wheels, %d item pools", next.wheel_count, next.pool_count);
}

static void spins_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    static const kn_sql q = {.any = "SELECT data FROM store_spin_data WHERE id = 1"};
    if (g_acct_brain && g_acct_brain->db) kn_db_query_sql(g_acct_brain->db, &q, 0, NULL, spins_row_done, NULL);
}

/* GET /api/data/spins: the wheels this brain spins ({season, wheels, itemPools}, gapi /data/spins) */
static void h_data_spins(kn_http_req *req, void *ud) {
    (void) ud;
    brain_cors_headers(req);
    if (s_db_spins_set && s_db_spins_text) {
        kn_http_respond_json_str(req, 200, s_db_spins_text);
        return;
    }
    kn_http_respond_json_str(req, 200, g_store_spins_json);
}

void store_init(Brain *b) {
    kn_http_route(b->http, "GET", "/api/data/spins", h_data_spins, b);
    if (!b->db) return;
    const int64_t ms = kn_config_int(b->cfg, "accounts.store_reload_ms", 60000);
    s_spins_timer = kn_loop_add_timer(b->loop, 0, (uint64_t) (ms > 1000 ? ms : 60000), spins_tick, NULL);
    const StoreSpins *sp = store_spins_builtin();
    KN_INFO(MOD, "v10 wheels: %d built in (%d item pools), store season %d", sp->wheel_count, sp->pool_count, store_season());
}

void store_shutdown(Brain *b) {
    if (s_spins_timer) kn_loop_cancel_timer(b->loop, s_spins_timer);
    s_spins_timer = 0;
}

/* ------------------------------------------------------------------ skins */

static SkinInfo *s_skins;
static int s_skin_count; /* -1 = load failed */
static StoreWheel *s_ticket_wheels; /* the spinWheel objects of TICKET items (SkinInfo.spin_wheel) */
static int s_ticket_wheel_count, s_ticket_wheel_cap;

static int json_truthy(const cJSON *v) {
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0) || (cJSON_IsString(v) && v->valuestring[0]);
}

/* a TICKET item's spinWheel object -> s_ticket_wheels[], its index or -1 */
static int ticket_wheel_load(const cJSON *sw) {
    if (!cJSON_IsObject(sw)) return -1;
    if (s_ticket_wheel_count == s_ticket_wheel_cap) {
        const int cap = s_ticket_wheel_cap ? s_ticket_wheel_cap * 2 : 16;
        StoreWheel *n = realloc(s_ticket_wheels, (size_t) cap * sizeof *n);
        if (!n) return -1;
        s_ticket_wheels = n;
        s_ticket_wheel_cap = cap;
    }
    StoreWheel *w = &s_ticket_wheels[s_ticket_wheel_count];
    store_wheel_parse(sw, w);
    if (!w->name[0]) snprintf(w->name, sizeof w->name, "Ticket");
    w->ticket = -1; /* redeemed by the item itself */
    w->locked = 0;
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
                k->free_item = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "free"));
                k->item_shop = (unsigned char) (kn_json_int(it, "itemShop", 0) == 1);
                k->min_quick_sell = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "minQuickSell"));
                k->limited_spin = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "limitedSpin"));
                k->tradable = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "tradable"));
                k->premium_spin = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "premiumSpin"));
                k->craftable = (unsigned char) json_truthy(cJSON_GetObjectItemCaseSensitive(it, "craftable"));
                k->qs_kr = (int) kn_json_int(it, "qsKR", 0);
                k->funds = kn_json_int(it, "funds", 0);
                const cJSON *rq = cJSON_GetObjectItemCaseSensitive(it, "req"), *ra = cJSON_GetObjectItemCaseSensitive(it, "reqAmount");
                if (cJSON_IsString(rq)) snprintf(k->req_kind, sizeof k->req_kind, "%s", rq->valuestring);
                snprintf(k->req_var, sizeof k->req_var, "%s", kn_json_str(it, "reqVar", ""));
                k->req_amount = cJSON_IsNumber(ra) ? ra->valuedouble : cJSON_IsString(ra) ? strtod(ra->valuestring, NULL) : 0;
                k->spin_wheel = ticket_wheel_load(cJSON_GetObjectItemCaseSensitive(it, "spinWheel"));
                k->spin_wheel_id = (int) kn_json_int(it, "spinWheelId", -1);
                snprintf(k->spin_wheel_name, sizeof k->spin_wheel_name, "%s", kn_json_str(it, "spinWheelName", ""));
                snprintf(k->spin_tag, sizeof k->spin_tag, "%s", kn_json_str(it, "spinTag", ""));
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

/* config.rewardTime: 12 h in v7.1.4 (v3.1.1: 6 h) */
static int64_t reward_time_ms(void) { return kn_config_int(g_acct_brain->cfg, "accounts.reward_time_s", 43200) * 1000; }

/* store.getSpnItems(wheel, rarity, account, wheel.season || config.currentSeason, wheel.bigRar)
 * of a rarity wheel (v10.js:103197). bigRar is applied to rolls of that rarity and above (the
 * client calls it with the top rarity only; as written it would add the big rarities to every
 * roll). An item with the wheel's spinTag passes whatever its limT / limG / season. Items with a
 * `req` (checkReq: account age / stats / rank / owned skins) are left out: KrunkNative does not
 * track those requirements. */
static int spin_items(const StoreWheel *w, int rarity, int *out, int max) {
    int n = 0;
    const int season = w->season > 0 ? w->season : store_season();
    const int big = w->big_rar > 0 && rarity >= w->big_rar;
    for (int q = 0; q < acct_skin_count() && n < max; q++) {
        const SkinInfo *k = acct_skin(q);
        if (k->limited || k->blocked || (k->no_spin && !k->limited_spin) || k->black_m || k->opensea || k->illicit) continue;
        if (!(k->rarity == rarity || (big && k->rarity >= w->big_rar))) continue;
        const int filtered = (w->lim_type[0] ? strcmp(w->lim_type, k->lim_t) != 0 : k->lim_t[0] != 0) ||
                             (w->lim_group[0] ? strcmp(w->lim_group, k->lim_g) != 0 : k->lim_g[0] != 0) || k->req ||
                             (!k->m_seas && (k->seas ? k->seas : 1) != season);
        if (filtered && !(w->tag[0] && !strcmp(w->tag, k->spin_tag))) continue;
        out[n++] = q;
    }
    return n;
}

/* a pool wheel's slice: itemPools[pools[slice]].items (a Set), store items only */
static int pool_items(const StoreWheel *w, int slice, int *out, int max) {
    const StorePool *p = slice >= 0 && slice < w->pool_count ? store_spins_pool(spins(), w->pools[slice]) : NULL;
    int n = 0;
    for (int i = 0; p && i < p->count && n < max; i++)
        if (acct_skin(p->items[i])) out[n++] = p->items[i];
    return n;
}

/* a wheel's ticketIndex by name ("Starter" / "Elite" / "Heroic": the Daily Spin's tiers,
 * daily_spin.c), -1 none */
int store_wheel_ticket(const char *name) {
    const StoreSpins *sp = spins();
    const int i = store_spins_find(sp, name);
    return i >= 0 ? sp->wheels[i].ticket : -1;
}

/* ------------------------------------------------------------------ "spin" */

#define SPIN_MAX 16
#define OWNED_BITS 16384

typedef struct SpinCtx {
    StoreWheel w;   /* the wheel (a copy: the set can rotate while the query runs) */
    int wheel;      /* store.wheels index, -1 = a ticket's own spinWheel */
    int ticket;     /* the TICKET item that pays (one per roll), -1 none: KR */
    int count;      /* rolls */
    int price;      /* KR per roll */
    double pos[SPIN_MAX];
    int skin[SPIN_MAX];
    int64_t kr;
    int index;
    int has_owned;  /* itemsAreUnique / duplicateProtectedPools: owned = the account's items */
    unsigned char owned[OWNED_BITS / 8];
} SpinCtx; /* the extra of social_query / social_call: freed after each callback, chained as a copy */

static SpinCtx *ctx_copy(const SpinCtx *c) {
    SpinCtx *n = malloc(sizeof *n);
    if (n) *n = *c;
    return n;
}

static void spin_error(SocialSession *s, const char *err) { social_send_n(s, "spin", 1, cJSON_CreateString(err)); }

static int owned(const SpinCtx *c, int item) { return c->has_owned && item >= 0 && item < OWNED_BITS && (c->owned[item >> 3] >> (item & 7)) & 1; }

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

/* one roll: the pointer (in [0, 100), as the client's wheel turns) and an item of its slice */
static int roll_item(SpinCtx *c, int i) {
    static int items[8192];
    const StoreWheel *w = &c->w;
    if (store_wheel_total(w) <= 0) return -1;
    /* the slice the pointer lands on (index.js: 2PI * (randNum / 100 + 13) + PI / 2 over the
     * slices in order; odds summing to 99.99 leave the rest to the last slice, as drawn) */
    c->pos[i] = roll100();
    const int slice = store_wheel_slice(w, c->pos[i]);
    int n = w->pool_count ? pool_items(w, slice, items, (int) (sizeof items / sizeof items[0]))
                          : spin_items(w, slice, items, (int) (sizeof items / sizeof items[0]));
    if (c->has_owned && (w->items_unique || (w->pool_count && (w->dup_protected >> slice) & 1))) {
        int m = 0; /* no duplicates: neither an owned item nor one this spin already won */
        for (int k = 0; k < n; k++) {
            int again = owned(c, items[k]);
            for (int j = 0; j < i && !again; j++) again = c->skin[j] == items[k];
            if (!again) items[m++] = items[k];
        }
        n = m;
    }
    return n > 0 ? items[kn_random_u64() % (uint64_t) n] : -1;
}

static void spin_account(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    const StoreWheel *w = &c->w;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        if (w->kr) free_kr_error(s, acct_db_error(res, "spin"));
        else spin_error(s, acct_db_error(res, "spin"));
        return;
    }
    const int level = acct_level(kn_db_get_int64(res, 0, 0, 0), NULL);
    const int64_t funds = kn_db_get_int64(res, 0, 1, 0);
    char id[24], a1[24], a2[24], a3[24], ref[48];
    if (w->kr) {
        /* Free KR wheel (fkr): KR by rarity, once per config.rewardTime */
        if (level < w->min_lvl) {
            free_kr_error(s, "Level too low");
            return;
        }
        c->pos[0] = roll100();
        c->index = store_wheel_slice(w, c->pos[0]);
        c->kr = k_free_kr[c->index < FREE_KR_COUNT ? c->index : FREE_KR_COUNT - 1];
        SpinCtx *next = ctx_copy(c);
        if (next)
            social_call(s, &P_DAILY_KR,
                         KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(a1, c->kr), acct_itoa(a2, reward_time_ms()),
                                    acct_itoa(a3, kn_wall_ms())),
                         free_kr_done, next);
        return;
    }
    if (level < w->min_lvl) {
        spin_error(s, "Level too low");
        return;
    }
    if (c->ticket < 0 && funds < (int64_t) c->price * c->count) {
        spin_error(s, "Not enough KR");
        return;
    }
    /* the rolls: a slice per pointer (a rarity, or an item pool), then a random item of it */
    char list[SPIN_MAX * 12] = "";
    size_t used = 0;
    for (int i = 0; i < c->count; i++) {
        c->skin[i] = roll_item(c, i);
        if (c->skin[i] < 0) {
            spin_error(s, "Error generating item");
            return;
        }
        used += (size_t) snprintf(list + used, sizeof list - used, "%s%d", i ? "," : "", c->skin[i]);
    }
    if (c->ticket >= 0) snprintf(ref, sizeof ref, "t%d:0", c->ticket);
    else snprintf(ref, sizeof ref, "%d:%d", c->wheel, c->price);
    SpinCtx *next = ctx_copy(c);
    if (next)
        social_call(s, &P_SPIN_MULTI,
                     KN_DB_ARGS(acct_itoa(id, s->account_id), ref, acct_itoa(a1, c->ticket >= 0 ? 0 : c->price), list,
                                c->ticket >= 0 ? acct_itoa(a2, c->ticket) : NULL),
                     spin_done, next);
}

static void spin_player_row(SocialSession *s, SpinCtx *c) { /* takes c */
    char id[24];
    social_query(s, SQL(.any = "SELECT player_score, player_funds FROM players WHERE player_id = $1"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id)), spin_account, c);
}

static void spin_owned(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    if (!kn_db_ok(res)) {
        spin_error(s, acct_db_error(res, "spin"));
        return;
    }
    SpinCtx *next = ctx_copy(c);
    if (!next) return;
    next->has_owned = 1;
    for (int i = 0; i < kn_db_rows(res); i++) {
        const int64_t k = kn_db_get_int64(res, i, 0, -1);
        if (k >= 0 && k < OWNED_BITS) next->owned[k >> 3] |= (unsigned char) (1u << (k & 7));
    }
    spin_player_row(s, next);
}

static void start_spin(SocialSession *s, const StoreWheel *w, int wheel, int ticket, int count) {
    SpinCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->w = *w;
    c->wheel = wheel;
    c->ticket = ticket;
    c->count = count;
    c->price = ticket >= 0 ? 0 : w->price;
    if (w->items_unique || w->dup_protected) {
        char id[24];
        social_query(s, SQL(.any = "SELECT skin_id FROM inventory WHERE account_id = $1 AND count > 0"),
                     KN_DB_ARGS(acct_itoa(id, s->account_id)), spin_owned, c);
        return;
    }
    spin_player_row(s, c);
}

/* The wheel a TICKET item redeems: its own spinWheel, store.wheels[spinWheelId] (v10's
 * Universal tickets) or the wheel named spinWheelName; -2 none. Sets *out. */
static int ticket_wheel(const SkinInfo *t, StoreWheel *out) {
    const StoreSpins *sp = spins();
    if (t->spin_wheel >= 0) {
        *out = s_ticket_wheels[t->spin_wheel];
        return -1;
    }
    int i = t->spin_wheel_id >= 0 && t->spin_wheel_id < sp->wheel_count ? t->spin_wheel_id : store_spins_find(sp, t->spin_wheel_name);
    if (i < 0) return -2;
    *out = sp->wheels[i];
    return i;
}

/* ["spin", wheelIndex, ticketSkin, multi] (index.js spinButton.onclick: io.send("spin", i, skin,
 * !!multi); v10 POST /spins/execute {index, ticket, multi}): a store wheel paid in KR (Nx with
 * multi: wheel.multi rolls at price each) or with its ticketIndex ticket (one ticket per roll), or
 * a TICKET item's own wheel (wheelIndex null; one ticket per roll) ->
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
    const StoreSpins *sp = spins();
    const int64_t ticket = fa_int(f, 2, -1);
    const int64_t index = fa_int(f, 1, -1);
    const cJSON *multi = fa(f, 3);
    const int want_multi = cJSON_IsTrue(multi) || (cJSON_IsNumber(multi) && multi->valuedouble > 1);
    StoreWheel w;
    int wheel;
    if (ticket >= 0) {
        const SkinInfo *t = acct_skin(ticket);
        if (!t || t->type != 13) {
            spin_error(s, "Invalid spin");
            return;
        }
        if (index >= 0 && index < sp->wheel_count && sp->wheels[index].ticket == ticket) {
            wheel = (int) index; /* a store wheel, paid with its ticket */
            w = sp->wheels[index];
        } else if ((wheel = ticket_wheel(t, &w)) == -2) {
            spin_error(s, "Invalid spin");
            return;
        }
        if (w.kr || (wheel >= 0 && !store_wheel_open(&w, kn_wall_ms()))) {
            spin_error(s, "Wheel unavailable");
            return;
        }
    } else {
        if (index < 0 || index >= sp->wheel_count) {
            spin_error(s, "Invalid spin");
            return;
        }
        wheel = (int) index;
        w = sp->wheels[index];
        /* the Free KR wheel spins through "fkr"; ticketOnly wheels through their ticket */
        if (w.kr || w.ticket_only || !store_wheel_open(&w, kn_wall_ms())) {
            spin_error(s, "Wheel unavailable");
            return;
        }
    }
    const int many = want_multi && w.multi > 1;
    start_spin(s, &w, wheel, (int) ticket, many ? (w.multi < SPIN_MAX ? w.multi : SPIN_MAX) : 1);
}

/* ------------------------------------------------------------------ "spinH" (spin history) */

/* the KR a history row cost: ref "<wheel>:<price>" (v7 / v10 spins), "t<ticket>:0" (tickets) or
 * "<wheel>" (v3.1.1 spins: Starter / Elite / Heroic at 50 / 100 / 500 KR) */
static int64_t history_kr(const char *ref) {
    static const int k_v3_price[] = {50, 100, 500};
    if (!ref || !*ref) return 0;
    const char *colon = strchr(ref, ':');
    if (colon) return strtoll(colon + 1, NULL, 10);
    if (*ref == 't') return 0;
    const long w = strtol(ref, NULL, 10);
    return w >= 0 && w < 3 ? k_v3_price[w] : 0;
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
    const StoreSpins *sp = spins();
    for (int i = 0; i < sp->wheel_count; i++)
        if (sp->wheels[i].kr) {
            start_spin(s, &sp->wheels[i], i, -1, 1);
            return;
        }
    free_kr_error(s, "Wheel unavailable");
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
