/*
 * services/brain/strike_econ.c - Krunker Strike's economy and progression (strike_econ.h).
 *
 * FRVR's Strike client talks to its gateway (gateway.us-svc.krunker.frvr.com, tools/strike/
 * strike_data.json "gateway.routes"); KrunkNative's Strike client has an authenticated social
 * socket, so the same paths travel through the gapi tunnel with a /strike prefix and the session
 * is the auth (social_gapi.c). Shapes follow the bundle's zod schemas (strike_data.json
 * "economy.schemas"): item definitions {_id, type, weapon, name, rarity, id, default}, wheels
 * {_id, name, nameKey, type, price {amount, currency}, rewards [{weight, poolId, poolName, color,
 * guaranteedIfSpinCountIs}]}, spin results {rewards [{reward, poolId, poolWeight, rand}],
 * heroicSpinsTriggered, goldenSpinCount}, missions {dailyMissions {missions [{details, progress,
 * completed}], updatedAt}, missions}.
 *
 *   GET  /strike/v1/item-definitions/all | size | :id | default/:type | name/:name
 *   POST /strike/v1/item-definitions/itemList {itemIDs}
 *   GET  /strike/v2/spin-wheels/list, /strike/v1/spin-wheels/:id, /strike/v2/spin-wheels/firstSpinDate
 *   POST /strike/v2/spin-wheels/details {ids?}            the wheels with their pools' items
 *   POST /strike/v2/spin-wheels/:id/spin {count, golden}   -> spin result + wallet
 *   GET  /strike/v1/shop                                   products (+ today's deals), sections and front.sections (golden spin)
 *   POST /strike/v1/shop/purchase {productId, priceId, quantity}   virtual currency / free only
 *   POST /strike/v1/shop/validate                          refused: KrunkNative takes no real money
 *   GET  /strike/v1/missions/me, POST /strike/v1/missions/claim/:id
 *   GET  /strike/v1/players/me, /players/me/items, /players/me/classes
 *   POST /strike/v1/players/me/equip {itemId} | {slot, itemId: null}      (KrunkNative: the loadout)
 *   POST /strike/v1/players/me/claim/progression {slotId, choice}
 *   GET  /strike/v1/game-config                            accountProgression, spinWheels, missions, xp
 *   GET  /strike/v1/leaderboards/:board?limit&offset        xp | kills | wins | score | games
 *
 * The catalogue (items, pools, wheels, products, missions, track, config) is migration 0320's
 * tables, read at start and every accounts.store_reload_ms (rotate wheels / products / missions
 * without a release). Rolls happen here (kn_random_u64); charging and granting are one transaction
 * (db_strike.c). Owner: strike-economy.
 */
#include "strike_econ.h"

#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <strike_economy.h> // the bundle's XP curve (strike_level_for_xp), the roll's slice rule
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "strike"

/* ------------------------------------------------------------------ the catalogue */

typedef struct StrikeCatalog {
    cJSON *items;     /* FRVR item definitions */
    cJSON *pools;     /* {poolId: {poolId, poolName, color, rarity, items: [itemId]}} */
    cJSON *wheels;    /* FRVR wheels (+ "countsGolden") */
    cJSON *products;  /* FRVR products */
    cJSON *missions;  /* FRVR mission details */
    cJSON *track;     /* [{level, slotId, isPlayerChoice, items}] */
    cJSON *config;    /* {key: value} */
} StrikeCatalog;

static StrikeCatalog s_cat;
static int s_loaded;
static kn_timer_id s_reload_timer;
static Brain *s_brain;

static void catalog_free(StrikeCatalog *c) {
    cJSON_Delete(c->items);
    cJSON_Delete(c->pools);
    cJSON_Delete(c->wheels);
    cJSON_Delete(c->products);
    cJSON_Delete(c->missions);
    cJSON_Delete(c->track);
    cJSON_Delete(c->config);
    memset(c, 0, sizeof *c);
}

static cJSON *parse_or(const char *text, cJSON *def) {
    cJSON *v = text && *text ? cJSON_Parse(text) : NULL;
    if (v) {
        cJSON_Delete(def);
        return v;
    }
    return def;
}

static int64_t cfg_int(const char *key, int64_t def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(s_cat.config, key);
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : def;
}

static const cJSON *cfg(const char *key) { return cJSON_GetObjectItemCaseSensitive(s_cat.config, key); }

static cJSON *item_json(const cJSON *row) {
    cJSON *o = cJSON_CreateObject();
    const int type = (int) kn_json_int(row, "item_type", 0);
    cJSON_AddStringToObject(o, "_id", kn_json_str(row, "id", ""));
    cJSON_AddNumberToObject(o, "type", type);
    if (kn_json_int(row, "weapon", -1) >= 0) cJSON_AddNumberToObject(o, "weapon", (double) kn_json_int(row, "weapon", -1));
    cJSON_AddStringToObject(o, "name", kn_json_str(row, "name", ""));
    if (kn_json_int(row, "rarity", -1) >= 0) cJSON_AddNumberToObject(o, "rarity", (double) kn_json_int(row, "rarity", -1));
    if (kn_json_int(row, "skin_index", -1) >= 0) cJSON_AddNumberToObject(o, "id", (double) kn_json_int(row, "skin_index", -1));
    if (kn_json_int(row, "class_id", -1) >= 0) cJSON_AddNumberToObject(o, "classId", (double) kn_json_int(row, "class_id", -1));
    cJSON_AddBoolToObject(o, "default", kn_json_int(row, "is_default", 0) != 0);
    cJSON_AddBoolToObject(o, "spinnable", kn_json_int(row, "spinnable", 0) != 0);
    if (*kn_json_str(row, "origin", "")) cJSON_AddStringToObject(o, "unlockOrigin", kn_json_str(row, "origin", ""));
    cJSON_AddItemToObject(o, "data", cJSON_CreateObject()); /* FRVR's render data: KrunkNative renders store.skins[id] */
    return o;
}

static void catalog_done(kn_db_result *res, void *ud) {
    (void) ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        if (!s_loaded) KN_WARN(MOD, "Strike catalogue unavailable (migration 0320 pending?): %s", kn_db_ok(res) ? "empty" : kn_db_error(res));
        return;
    }
    StrikeCatalog c = {0};
    c.items = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "items")) cJSON_AddItemToArray(c.items, item_json(r));
    c.pools = cJSON_CreateObject();
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "pools")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "poolId", kn_json_str(r, "id", ""));
        cJSON_AddStringToObject(p, "poolName", kn_json_str(r, "name", ""));
        cJSON_AddStringToObject(p, "color", kn_json_str(r, "color", ""));
        cJSON_AddNumberToObject(p, "rarity", (double) kn_json_int(r, "rarity", 0));
        cJSON_AddItemToObject(p, "items", cJSON_CreateArray());
        cJSON_AddItemToObject(c.pools, kn_json_str(r, "id", ""), p);
    }
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "pool_items")) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(c.pools, kn_json_str(r, "pool_id", ""));
        if (p) cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(p, "items"), cJSON_CreateString(kn_json_str(r, "item_id", "")));
    }
    c.wheels = cJSON_CreateArray();
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "wheels")) {
        cJSON *w = cJSON_CreateObject();
        const char *id = kn_json_str(r, "id", "");
        cJSON_AddStringToObject(w, "_id", id);
        cJSON_AddStringToObject(w, "name", kn_json_str(r, "name", ""));
        cJSON_AddStringToObject(w, "nameKey", kn_json_str(r, "name_key", ""));
        cJSON_AddStringToObject(w, "type", kn_json_str(r, "wheel_type", "default"));
        if (*kn_json_str(r, "price_currency", "")) {
            cJSON *price = cJSON_AddObjectToObject(w, "price");
            cJSON_AddNumberToObject(price, "amount", (double) kn_json_int(r, "price_amount", 0));
            cJSON_AddStringToObject(price, "currency", kn_json_str(r, "price_currency", ""));
        }
        if (kn_json_int(r, "time_between_s", 0) > 0) {
            cJSON_AddNumberToObject(w, "timeBetweenSpins", (double) kn_json_int(r, "time_between_s", 0));
            cJSON_AddNumberToObject(w, "maxAccumulatedSpins", (double) kn_json_int(r, "max_accumulated", 1));
        }
        if (kn_json_num(r, "heroic_chance", 0) > 0) cJSON_AddNumberToObject(w, "heroicSpinTriggerChance", kn_json_num(r, "heroic_chance", 0));
        cJSON_AddBoolToObject(w, "hideInShop", kn_json_int(r, "hide_in_shop", 0) != 0);
        cJSON_AddBoolToObject(w, "countsGolden", kn_json_int(r, "counts_golden", 0) != 0);
        cJSON_AddStringToObject(w, "imageUrl", "");
        cJSON_AddStringToObject(w, "buttonColor", kn_json_str(r, "button_color", ""));
        cJSON_AddStringToObject(w, "category", kn_json_str(r, "category", ""));
        cJSON *rewards = cJSON_AddArrayToObject(w, "rewards");
        const cJSON *sl;
        cJSON_ArrayForEach(sl, cJSON_GetObjectItemCaseSensitive(v, "slices")) {
            if (strcmp(kn_json_str(sl, "wheel_id", ""), id)) continue;
            const cJSON *pool = cJSON_GetObjectItemCaseSensitive(c.pools, kn_json_str(sl, "pool_id", ""));
            cJSON *s = cJSON_CreateObject();
            cJSON_AddNumberToObject(s, "weight", kn_json_num(sl, "weight", 0));
            cJSON_AddStringToObject(s, "poolId", kn_json_str(sl, "pool_id", ""));
            cJSON_AddStringToObject(s, "poolName", kn_json_str(pool, "poolName", ""));
            cJSON_AddStringToObject(s, "color", kn_json_str(pool, "color", ""));
            if (kn_json_int(sl, "guaranteed_if", 0) > 0) cJSON_AddNumberToObject(s, "guaranteedIfSpinCountIs", (double) kn_json_int(sl, "guaranteed_if", 0));
            cJSON_AddItemToArray(rewards, s);
        }
        cJSON_AddItemToArray(c.wheels, w);
    }
    c.products = cJSON_CreateArray();
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "products")) {
        cJSON *p = cJSON_CreateObject();
        const char *id = kn_json_str(r, "id", "");
        cJSON_AddStringToObject(p, "_id", id);
        cJSON_AddStringToObject(p, "title", kn_json_str(r, "title", ""));
        cJSON_AddStringToObject(p, "category", kn_json_str(r, "section", ""));
        cJSON_AddBoolToObject(p, "active", 1);
        cJSON *prices = cJSON_AddArrayToObject(p, "price");
        cJSON *pr = cJSON_CreateObject();
        char pid[96];
        snprintf(pid, sizeof pid, "%s_p0", id);
        cJSON_AddStringToObject(pr, "_id", pid);
        cJSON_AddStringToObject(pr, "type", kn_json_str(r, "price_type", "virtual_currency"));
        if (*kn_json_str(r, "price_currency", "")) {
            cJSON_AddStringToObject(pr, "currency", kn_json_str(r, "price_currency", ""));
            cJSON_AddNumberToObject(pr, "amount", (double) kn_json_int(r, "price_amount", 0));
        }
        cJSON_AddItemToObject(pr, "conditions", cJSON_CreateArray());
        cJSON_AddBoolToObject(pr, "active", 1);
        cJSON_AddItemToArray(prices, pr);
        cJSON_AddItemToObject(p, "products", parse_or(kn_json_str(r, "rewards", ""), cJSON_CreateArray()));
        if (kn_json_int(r, "refresh_s", 0) > 0) cJSON_AddNumberToObject(p, "refreshTime", (double) kn_json_int(r, "refresh_s", 0) * 1000.0);
        if (kn_json_int(r, "max_purchases", 0) > 0) cJSON_AddNumberToObject(p, "maxPurchaseCount", (double) kn_json_int(r, "max_purchases", 0));
        cJSON_AddItemToArray(c.products, p);
    }
    c.missions = cJSON_CreateArray();
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "missions")) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "_id", kn_json_str(r, "id", ""));
        cJSON_AddBoolToObject(m, "isActive", 1);
        cJSON_AddStringToObject(m, "type", kn_json_str(r, "mission_type", "daily"));
        cJSON_AddStringToObject(m, "key", kn_json_str(r, "mkey", ""));
        cJSON_AddStringToObject(m, "textKey", kn_json_str(r, "text_key", ""));
        cJSON_AddNumberToObject(m, "requiredValue", (double) kn_json_int(r, "required", 1));
        cJSON_AddBoolToObject(m, "best", kn_json_int(r, "max_mode", 0) != 0);
        cJSON_AddNumberToObject(m, "weight", (double) kn_json_int(r, "weight", 1));
        cJSON_AddItemToObject(m, "rewards", parse_or(kn_json_str(r, "rewards", ""), cJSON_CreateArray()));
        cJSON_AddItemToArray(c.missions, m);
    }
    c.track = cJSON_CreateArray();
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "track")) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "level", (double) kn_json_int(r, "level", 0));
        cJSON_AddStringToObject(t, "slotId", kn_json_str(r, "slot_id", ""));
        cJSON_AddBoolToObject(t, "isPlayerChoice", kn_json_int(r, "player_choice", 0) != 0);
        cJSON_AddItemToObject(t, "items", parse_or(kn_json_str(r, "rewards", ""), cJSON_CreateArray()));
        cJSON_AddItemToArray(c.track, t);
    }
    c.config = cJSON_CreateObject();
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(v, "config")) {
        const char *val = kn_json_str(r, "value", "");
        cJSON *pv = cJSON_Parse(val);
        cJSON_AddItemToObject(c.config, kn_json_str(r, "ckey", ""), pv ? pv : cJSON_CreateString(val));
    }
    const int items = cJSON_GetArraySize(c.items), wheels = cJSON_GetArraySize(c.wheels);
    cJSON_Delete(v);
    if (items <= 0 || wheels <= 0) {
        catalog_free(&c);
        if (!s_loaded) KN_WARN(MOD, "Strike catalogue tables are empty");
        return;
    }
    const int first = !s_loaded;
    catalog_free(&s_cat);
    s_cat = c;
    s_loaded = 1;
    if (first) KN_INFO(MOD, "Strike catalogue: %d items, %d wheels, %d products, %d missions, %d track levels", items, wheels,
                       cJSON_GetArraySize(c.products), cJSON_GetArraySize(c.missions), cJSON_GetArraySize(c.track));
}

static void reload_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (s_brain && s_brain->db) kn_db_call(s_brain->db, &P_STRIKE_CATALOG, 0, NULL, catalog_done, NULL);
}

static void h_http_board(kn_http_req *req, void *ud);

int strike_econ_init(Brain *b) {
    s_brain = b;
    if (b->http) kn_http_route(b->http, "GET", "/api/strike/leaderboards/:type", h_http_board, b);
    if (!b->db) return 0;
    const int64_t ms = kn_config_int(b->cfg, "accounts.store_reload_ms", 60000);
    s_reload_timer = kn_loop_add_timer(b->loop, 0, (uint64_t) (ms > 1000 ? ms : 60000), reload_tick, NULL);
    return 0;
}

void strike_econ_shutdown(Brain *b) {
    if (s_reload_timer && b && b->loop) kn_loop_cancel_timer(b->loop, s_reload_timer);
    s_reload_timer = 0;
    catalog_free(&s_cat);
    s_loaded = 0;
}

/* ------------------------------------------------------------------ lookups */

static const cJSON *find_by(const cJSON *arr, const char *key, const char *id) {
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) if (id && !strcmp(kn_json_str(r, key, ""), id)) return r;
    return NULL;
}

static const cJSON *item_def(const char *id) { return find_by(s_cat.items, "_id", id); }
static const cJSON *wheel_def(const char *id) { return find_by(s_cat.wheels, "_id", id); }
static const cJSON *mission_def(const char *id) { return find_by(s_cat.missions, "_id", id); }
static const cJSON *track_slot(const char *id) { return find_by(s_cat.track, "slotId", id); }

/* the loadout slot of an item (strike_economy.h strike_item_slot) */
static int item_slot(const cJSON *def, char *out, size_t size) {
    const int type = (int) kn_json_int(def, "type", -1);
    if (type == STRIKE_ITEM_WEAPON && kn_json_int(def, "weapon", -1) >= 0) {
        snprintf(out, size, "w%d", (int) kn_json_int(def, "weapon", -1));
        return 1;
    }
    switch (type) {
    case STRIKE_ITEM_HAT: case STRIKE_ITEM_BACK: case STRIKE_ITEM_MELEE: case STRIKE_ITEM_SPRAY: case STRIKE_ITEM_DYE:
    case STRIKE_ITEM_WAIST: case STRIKE_ITEM_FACE: case STRIKE_ITEM_SHOE: case STRIKE_ITEM_PET: case STRIKE_ITEM_WRIST:
    case STRIKE_ITEM_CHARM:
        snprintf(out, size, "t%d", type);
        return 1;
    default:
        return 0;
    }
}

static double rand01(void) { return (double) (kn_random_u64() >> 11) * (1.0 / 9007199254740992.0); }

static int64_t now_ms(void) { return kn_wall_ms(); }

/* reward (FRVR's c1 union) for an item / class definition */
static cJSON *reward_of_item(const cJSON *def) {
    cJSON *r = cJSON_CreateObject();
    const int is_class = kn_json_int(def, "type", 0) == STRIKE_ITEM_CLASS;
    cJSON_AddStringToObject(r, "type", is_class ? "class" : "item");
    cJSON *item = cJSON_Duplicate(def, 1);
    cJSON_AddStringToObject(item, "itemId", kn_json_str(def, "_id", ""));
    cJSON_AddItemToObject(r, "item", item);
    return r;
}

/* a reward list of the catalogue (h0) with the item definitions resolved (c1) */
static cJSON *rewards_resolved(const cJSON *rewards) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rewards) {
        const char *type = kn_json_str(r, "type", "");
        const cJSON *def = item_def(kn_json_str(r, "item", ""));
        if ((!strcmp(type, "item") || !strcmp(type, "class")) && def) cJSON_AddItemToArray(out, reward_of_item(def));
        else cJSON_AddItemToArray(out, cJSON_Duplicate(r, 1));
    }
    return out;
}

/* ------------------------------------------------------------------ request plumbing */

typedef struct SCtx {
    int64_t rid;
    int count;
    int64_t a;
    char id[96];
    cJSON *data; /* owned by the callback (deleted there) */
} SCtx;

static SCtx *sctx(const GapiReq *r) {
    SCtx *c = calloc(1, sizeof *c);
    if (c) c->rid = r->rid;
    return c;
}

static int need_login(SocialSession *s, const GapiReq *r) {
    if (s->account_id > 0) return 1;
    gapi_error(s, r->rid, 401, "UNAUTHORIZED", "You must be signed in.");
    return 0;
}

static int need_catalog(SocialSession *s, const GapiReq *r) {
    if (s_loaded) return 1;
    gapi_error(s, r->rid, 503, "STRIKE_UNAVAILABLE", "The Strike store is unavailable.");
    return 0;
}

static const char *body_str(const GapiReq *r, const char *k) { return kn_json_str(r->body, k, NULL); }

static int64_t body_int(const GapiReq *r, const char *k, int64_t def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r->body, k);
    if (cJSON_IsNumber(v)) return (int64_t) v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return strtoll(v->valuestring, NULL, 10);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

static void fail_db(SocialSession *s, SCtx *c, kn_db_result *res, const char *code) {
    const char *msg = acct_db_error(res, MOD);
    const int user = !strcmp(kn_db_sqlstate(res), "P0001");
    gapi_error(s, c->rid, user ? 400 : 500, code, msg);
}

/* ------------------------------------------------------------------ item definitions */

static int r_items(SocialSession *s, const GapiReq *r) {
    if (!need_catalog(s, r)) return 1;
    if (gapi_is(r, "GET", "/strike/v1/item-definitions/all")) {
        gapi_reply(s, r->rid, cJSON_Duplicate(s_cat.items, 1));
    } else if (gapi_is(r, "GET", "/strike/v1/item-definitions/size")) {
        gapi_reply(s, r->rid, cJSON_CreateNumber(cJSON_GetArraySize(s_cat.items)));
    } else if (gapi_is(r, "GET", "/strike/v1/item-definitions/default/:type")) {
        const long type = strtol(r->seg[4], NULL, 10);
        const cJSON *it, *found = NULL;
        cJSON_ArrayForEach(it, s_cat.items) if (!found && kn_json_int(it, "type", -1) == type && cJSON_IsTrue(cJSON_GetObjectItem(it, "default"))) found = it;
        if (found) gapi_reply(s, r->rid, cJSON_Duplicate(found, 1));
        else gapi_error(s, r->rid, 404, "ITEM_NOT_FOUND", "Invalid Item Type/Item with that type does not exist/There is no default item for that type");
    } else if (gapi_is(r, "GET", "/strike/v1/item-definitions/name/:name")) {
        const cJSON *found = find_by(s_cat.items, "name", r->seg[4]);
        if (found) gapi_reply(s, r->rid, cJSON_Duplicate(found, 1));
        else gapi_error(s, r->rid, 404, "ITEM_NOT_FOUND", "Invalid Item Name/Item with that Name does not exist");
    } else if (gapi_is(r, "POST", "/strike/v1/item-definitions/itemList")) {
        cJSON *out = cJSON_CreateArray();
        const cJSON *id;
        cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(r->body, "itemIDs")) {
            const cJSON *def = cJSON_IsString(id) ? item_def(id->valuestring) : NULL;
            if (!def) {
                cJSON_Delete(out);
                gapi_error(s, r->rid, 404, "ITEM_NOT_FOUND", "Invalid Item Def IDs/One or more item defs with specified IDs do not exist");
                return 1;
            }
            cJSON_AddItemToArray(out, cJSON_Duplicate(def, 1));
        }
        gapi_reply(s, r->rid, out);
    } else if (gapi_is(r, "GET", "/strike/v1/item-definitions/:id")) {
        const cJSON *def = item_def(r->seg[3]);
        if (def) gapi_reply(s, r->rid, cJSON_Duplicate(def, 1));
        else gapi_error(s, r->rid, 404, "ITEM_NOT_FOUND", "Invalid Item Def Id/Item Def with that id does not exist");
    } else {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ wheels */

/* a wheel as listed (FRVR's list shape: no pools' items) + the account's lastSpinTime */
static cJSON *wheel_listed(const cJSON *w, const cJSON *spins, int with_items) {
    cJSON *o = cJSON_Duplicate(w, 1);
    const cJSON *row = spins ? find_by(spins, "wheel_id", kn_json_str(w, "_id", "")) : NULL;
    if (row && kn_json_int(row, "last_ms", 0) > 0) cJSON_AddNumberToObject(o, "lastSpinTime", (double) kn_json_int(row, "last_ms", 0));
    if (with_items) {
        cJSON *rw;
        cJSON_ArrayForEach(rw, cJSON_GetObjectItemCaseSensitive(o, "rewards")) {
            const cJSON *pool = cJSON_GetObjectItemCaseSensitive(s_cat.pools, kn_json_str(rw, "poolId", ""));
            cJSON *items = cJSON_AddArrayToObject(rw, "items");
            const cJSON *id;
            cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(pool, "items")) {
                cJSON *h = cJSON_CreateObject();
                cJSON_AddStringToObject(h, "type", "item");
                cJSON_AddStringToObject(h, "itemDefId", id->valuestring);
                cJSON_AddItemToArray(items, h);
            }
        }
    }
    return o;
}

static void wheels_done(SocialSession *s, kn_db_result *res, void *extra) {
    SCtx *c = extra;
    cJSON *spins = kn_db_ok(res) ? kn_db_result_to_json(res) : NULL;
    cJSON *out = cJSON_CreateArray();
    const cJSON *w;
    cJSON_ArrayForEach(w, s_cat.wheels) {
        if (c->data && cJSON_GetArraySize(c->data) > 0) {
            int want = 0;
            const cJSON *id;
            cJSON_ArrayForEach(id, c->data) want |= cJSON_IsString(id) && !strcmp(id->valuestring, kn_json_str(w, "_id", ""));
            if (!want) continue;
        }
        cJSON_AddItemToArray(out, wheel_listed(w, spins, c->count));
    }
    cJSON_Delete(spins);
    cJSON_Delete(c->data);
    if (c->a) { /* /v1/spin-wheels/:id: one wheel */
        cJSON *one = cJSON_DetachItemFromArray(out, 0);
        cJSON_Delete(out);
        if (one) gapi_reply(s, c->rid, one);
        else gapi_error(s, c->rid, 404, "WHEEL_NOT_FOUND", "Spin wheel not found");
        return;
    }
    gapi_reply(s, c->rid, out);
}

static void wheels_reply(SocialSession *s, const GapiReq *r, cJSON *ids, int with_items, int one) {
    SCtx *c = sctx(r);
    if (!c) {
        cJSON_Delete(ids);
        return;
    }
    c->data = ids;
    c->count = with_items;
    c->a = one;
    char id[24];
    social_query(s, SQL(.any = "SELECT wheel_id, spins AS spins__i, last_spin_ms AS last_ms__i FROM strike_wheel_spins WHERE account_id = $1"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id > 0 ? s->account_id : 0)), wheels_done, c);
}

/* the spin: rolls here, charge + grant in P_STRIKE_SPIN */
typedef struct SpinCtx {
    int64_t rid;
    char wheel[64];
    int count, heroic;
    int64_t at;
    cJSON *results; /* [{reward, poolId, poolWeight, rand}] */
} SpinCtx;

static void spin_done(SocialSession *s, kn_db_result *res, void *extra) {
    SpinCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        const int user = !strcmp(kn_db_sqlstate(res), "P0001");
        gapi_error(s, c->rid, user ? 400 : 500, "SPIN_FAILED", acct_db_error(res, MOD));
        cJSON_Delete(c->results);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rewards", c->results);
    cJSON_AddNumberToObject(o, "heroicSpinsTriggered", c->heroic);
    cJSON_AddNumberToObject(o, "goldenSpinCount", kn_json_num(v, "goldenSpinCount", 0));
    cJSON_AddNumberToObject(o, "goldenTicketsFromMeter", kn_json_num(v, "goldenFromMeter", 0));
    cJSON_AddItemToObject(o, "wallet", cJSON_DetachItemFromObject(v, "wallet"));
    cJSON_Delete(v);
    gapi_reply(s, c->rid, o);
}

static void r_spin(SocialSession *s, const GapiReq *r) {
    const cJSON *w = wheel_def(r->seg[3]);
    if (!w) {
        gapi_error(s, r->rid, 404, "WHEEL_NOT_FOUND", "Spin wheel not found");
        return;
    }
    const int count = (int) body_int(r, "count", body_int(r, "amount", 1));
    const int golden = body_int(r, "golden", 0) != 0;
    const cJSON *price = cJSON_GetObjectItemCaseSensitive(w, "price");
    const int64_t period_ms = (int64_t) kn_json_int(w, "timeBetweenSpins", 0) * 1000;
    if (count < 1 || count > 10 || (period_ms > 0 && count != 1)) {
        gapi_error(s, r->rid, 400, "BAD_REQUEST", "Invalid spin count");
        return;
    }
    const cJSON *slices = cJSON_GetObjectItemCaseSensitive(w, "rewards");
    const int n = cJSON_GetArraySize(slices);
    if (n <= 0 || n > 16) {
        gapi_error(s, r->rid, 500, "WHEEL_EMPTY", "This wheel has no rewards");
        return;
    }
    double weights[16];
    const cJSON *pools[16];
    for (int i = 0; i < n; i++) {
        const cJSON *sl = cJSON_GetArrayItem(slices, i);
        pools[i] = cJSON_GetObjectItemCaseSensitive(s_cat.pools, kn_json_str(sl, "poolId", ""));
        weights[i] = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(pools[i], "items")) > 0 ? kn_json_num(sl, "weight", 0) : 0.0;
    }
    int slice_of[10];
    double roll_of[10];
    const char *item_of[10];
    for (int k = 0; k < count; k++) {
        roll_of[k] = rand01();
        const int sl = strike_wheel_slice_for_roll(weights, n, roll_of[k]);
        if (sl < 0) {
            gapi_error(s, r->rid, 500, "WHEEL_EMPTY", "This wheel has no rewards");
            return;
        }
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(pools[sl], "items");
        const int pick = (int) (rand01() * cJSON_GetArraySize(items));
        slice_of[k] = sl;
        item_of[k] = cJSON_GetArrayItem(items, pick)->valuestring;
    }
    /* guaranteedIfSpinCountIs: a spin of that many holds one in the slice's pool or a rarer one */
    for (int g = 0; g < n; g++) {
        const cJSON *sl = cJSON_GetArrayItem(slices, g);
        if (kn_json_int(sl, "guaranteedIfSpinCountIs", 0) != count || weights[g] <= 0) continue;
        const int64_t want = kn_json_int(pools[g], "rarity", 0);
        int met = 0;
        for (int k = 0; k < count; k++) met |= kn_json_int(pools[slice_of[k]], "rarity", 0) >= want;
        if (!met) {
            const cJSON *items = cJSON_GetObjectItemCaseSensitive(pools[g], "items");
            slice_of[count - 1] = g;
            item_of[count - 1] = cJSON_GetArrayItem(items, (int) (rand01() * cJSON_GetArraySize(items)))->valuestring;
        }
    }
    SpinCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->rid = r->rid;
    c->count = count;
    snprintf(c->wheel, sizeof c->wheel, "%s", kn_json_str(w, "_id", ""));
    const double heroic_chance = kn_json_num(w, "heroicSpinTriggerChance", 0);
    char items_csv[10 * 72] = "";
    size_t len = 0;
    c->results = cJSON_CreateArray();
    for (int k = 0; k < count; k++) {
        if (heroic_chance > 0 && rand01() < heroic_chance) c->heroic++;
        const cJSON *def = item_def(item_of[k]);
        const cJSON *sl = cJSON_GetArrayItem(slices, slice_of[k]);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "reward", def ? reward_of_item(def) : cJSON_CreateNull());
        cJSON_AddStringToObject(res, "poolId", kn_json_str(sl, "poolId", ""));
        cJSON_AddNumberToObject(res, "poolWeight", kn_json_num(sl, "weight", 0));
        cJSON_AddNumberToObject(res, "rand", roll_of[k]);
        cJSON_AddItemToArray(c->results, res);
        len += (size_t) snprintf(items_csv + len, sizeof items_csv - len, "%s%s", k ? "," : "", item_of[k]);
    }
    /* the price: golden tickets pay a Lucky Draw's golden spin (FRVR: currency = SPIN_GOLDEN_TICKET) */
    const char *currency = price ? (golden ? "spin_golden_ticket" : kn_json_str(price, "currency", "kr")) : "";
    const int64_t cost = price ? kn_json_int(price, "amount", 0) * count : 0;
    char id[24], scost[24], snow[24], sper[24], sacc[24], sgold[24], sher[24], smeter[24];
    const int counts_golden = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "countsGolden")) && !golden;
    social_call(s, &P_STRIKE_SPIN,
                KN_DB_ARGS(acct_itoa(id, s->account_id), c->wheel, currency, acct_itoa(scost, cost), acct_itoa(snow, now_ms()),
                           acct_itoa(sper, period_ms), acct_itoa(sacc, kn_json_int(w, "maxAccumulatedSpins", 1)),
                           acct_itoa(sgold, counts_golden), acct_itoa(sher, c->heroic), items_csv,
                           acct_itoa(smeter, cfg_int("golden_spin_meter", strike_economy()->golden_spin_meter))),
                spin_done, c);
}

static int r_wheels(SocialSession *s, const GapiReq *r) {
    if (!need_catalog(s, r)) return 1;
    if (gapi_is(r, "GET", "/strike/v2/spin-wheels/list")) {
        wheels_reply(s, r, NULL, 0, 0);
    } else if (gapi_is(r, "POST", "/strike/v2/spin-wheels/details")) {
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(r->body, "ids");
        if (!ids && cJSON_IsArray(r->body)) ids = r->body;
        wheels_reply(s, r, ids ? cJSON_Duplicate(ids, 1) : NULL, 1, 0);
    } else if (gapi_is(r, "GET", "/strike/v2/spin-wheels/firstSpinDate")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNullToObject(o, "date");
        gapi_reply(s, r->rid, o);
    } else if (gapi_is(r, "GET", "/strike/v1/spin-wheels/:id")) {
        cJSON *ids = cJSON_CreateArray();
        cJSON_AddItemToArray(ids, cJSON_CreateString(r->seg[3]));
        wheels_reply(s, r, ids, 1, 1);
    } else if (gapi_is(r, "POST", "/strike/v2/spin-wheels/:id/spin")) {
        if (need_login(s, r)) r_spin(s, r);
    } else {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ the account */

static cJSON *level_json(int64_t xp) {
    const StrikeLevel l = strike_level_for_xp(xp);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "level", l.level);
    cJSON_AddNumberToObject(o, "xpInCurrentLevel", (double) l.xp_in_level);
    cJSON_AddNumberToObject(o, "xpToNextLevel", (double) l.xp_to_next);
    cJSON_AddNumberToObject(o, "totalXpNeeded", (double) l.total_needed);
    return o;
}

/* the classes an account plays: the starter classes + its class items */
static cJSON *classes_of(const cJSON *items) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *def;
    cJSON_ArrayForEach(def, s_cat.items) {
        if (kn_json_int(def, "type", -1) != STRIKE_ITEM_CLASS) continue;
        int owned = cJSON_IsTrue(cJSON_GetObjectItem(def, "default"));
        if (!owned) owned = find_by(items, "item_id", kn_json_str(def, "_id", "")) != NULL;
        if (!owned) continue;
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "classId", (double) kn_json_int(def, "classId", -1));
        cJSON_AddStringToObject(c, "itemId", kn_json_str(def, "_id", ""));
        cJSON_AddItemToArray(out, c);
    }
    return out;
}

enum { ME_PLAYER, ME_ITEMS, ME_CLASSES, ME_SHOP };

static cJSON *shop_json(const cJSON *purchases);

static void me_done(SocialSession *s, kn_db_result *res, void *extra) {
    SCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        fail_db(s, c, res, "PLAYER_FAILED");
        return;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(v, "items");
    if (c->a == ME_ITEMS) {
        cJSON *out = cJSON_CreateArray();
        const cJSON *it;
        cJSON_ArrayForEach(it, items) {
            const cJSON *def = item_def(kn_json_str(it, "item_id", ""));
            if (!def) continue; /* features ("feature:missions") are not items */
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "_id", kn_json_str(it, "item_id", ""));
            cJSON_AddStringToObject(o, "itemId", kn_json_str(it, "item_id", ""));
            cJSON_AddNumberToObject(o, "count", (double) kn_json_int(it, "count", 1));
            cJSON_AddNumberToObject(o, "createdAt", (double) kn_json_int(it, "first_ms", 0));
            cJSON_AddItemToObject(o, "item", cJSON_Duplicate(def, 1));
            cJSON_AddItemToArray(out, o);
        }
        cJSON_Delete(v);
        gapi_reply(s, c->rid, out);
        return;
    }
    if (c->a == ME_CLASSES) {
        cJSON *out = classes_of(items);
        cJSON_Delete(v);
        gapi_reply(s, c->rid, out);
        return;
    }
    if (c->a == ME_SHOP) {
        cJSON *out = shop_json(cJSON_GetObjectItemCaseSensitive(v, "purchases"));
        cJSON_Delete(v);
        gapi_reply(s, c->rid, out);
        return;
    }
    const cJSON *prog = cJSON_GetObjectItemCaseSensitive(v, "progress");
    const int64_t xp = kn_json_int(prog, "xp", 0);
    cJSON *o = cJSON_CreateObject();
    char pid[24];
    cJSON_AddStringToObject(o, "playerId", acct_itoa(pid, s->account_id));
    cJSON_AddStringToObject(o, "name", s->username);
    cJSON *wallet = cJSON_DetachItemFromObject(v, "wallet");
    if (!wallet) wallet = cJSON_CreateObject();
    cJSON_DeleteItemFromObject(wallet, "xp");
    cJSON_AddNumberToObject(wallet, "xp", (double) xp); /* the wallet's XP is the account's Strike XP */
    for (int cur = 0; cur < STRIKE_CUR_COUNT; cur++)
        if (!cJSON_GetObjectItemCaseSensitive(wallet, strike_currency_key((StrikeCurrency) cur)))
            cJSON_AddNumberToObject(wallet, strike_currency_key((StrikeCurrency) cur), 0);
    cJSON_AddItemToObject(o, "wallet", wallet);
    cJSON_AddNumberToObject(o, "xp", (double) xp);
    cJSON_AddItemToObject(o, "level", level_json(xp));
    cJSON_AddNumberToObject(o, "maxLevelCap", (double) cfg_int("max_level", strike_economy()->max_level));
    cJSON_AddNumberToObject(o, "goldenSpinCount", (double) kn_json_int(prog, "golden_spin_count", 0));
    cJSON_AddNumberToObject(o, "spinsToGetGoldenSpin", (double) cfg_int("golden_spin_meter", strike_economy()->golden_spin_meter));
    cJSON *stats = cJSON_AddObjectToObject(o, "stats");
    static const char *const keys[][2] = {{"kills", "kills"}, {"deaths", "deaths"}, {"wins", "wins"}, {"games", "games"},
                                          {"score", "score"}, {"timePlayed", "time_ms"}};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) cJSON_AddNumberToObject(stats, keys[i][0], (double) kn_json_int(prog, keys[i][1], 0));
    cJSON_AddItemToObject(o, "loadout", cJSON_DetachItemFromObject(v, "loadout"));
    cJSON_AddItemToObject(o, "progressionClaimedItems", cJSON_DetachItemFromObject(v, "claims"));
    cJSON_AddItemToObject(o, "classes", classes_of(items));
    const int unlock = (int) cfg_int("missions_unlock_level", strike_economy()->missions_unlock_level);
    cJSON_AddBoolToObject(o, "missionsUnlocked", strike_level_for_xp(xp).level >= unlock);
    cJSON_AddNumberToObject(o, "itemCount", cJSON_GetArraySize(items));
    cJSON_Delete(v);
    gapi_reply(s, c->rid, o);
}

static void me_call(SocialSession *s, const GapiReq *r, int what) {
    SCtx *c = sctx(r);
    if (!c) return;
    c->a = what;
    char id[24];
    social_call(s, &P_STRIKE_ME, KN_DB_ARGS(acct_itoa(id, s->account_id)), me_done, c);
}

static void json_done(SocialSession *s, kn_db_result *res, void *extra) {
    SCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        fail_db(s, c, res, c->id[0] ? c->id : "REQUEST_FAILED");
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(v, "rewards")) { /* granted rewards with their item definitions */
        cJSON *res_rw = rewards_resolved(cJSON_GetObjectItemCaseSensitive(v, "rewards"));
        cJSON_ReplaceItemInObject(v, "rewards", res_rw);
    }
    if (cJSON_GetObjectItemCaseSensitive(v, "claims")) cJSON_AddItemToObject(v, "progressionClaimedItems", cJSON_DetachItemFromObject(v, "claims"));
    gapi_reply(s, c->rid, v);
}

static void r_equip(SocialSession *s, const GapiReq *r) {
    const char *item = body_str(r, "itemId");
    const char *slot_in = body_str(r, "slot");
    char slot[16] = "";
    const cJSON *def = item ? item_def(item) : NULL;
    if (item && !def) {
        gapi_error(s, r->rid, 404, "ITEM_NOT_FOUND", "Invalid Item Id/Item with that id does not exist");
        return;
    }
    if (def && !item_slot(def, slot, sizeof slot)) {
        gapi_error(s, r->rid, 400, "NOT_EQUIPPABLE", "This item can't be equipped");
        return;
    }
    if (!def) {
        if (!slot_in || strlen(slot_in) >= sizeof slot || (slot_in[0] != 'w' && slot_in[0] != 't')) {
            gapi_error(s, r->rid, 400, "BAD_REQUEST", "Missing itemId / slot");
            return;
        }
        snprintf(slot, sizeof slot, "%s", slot_in);
    }
    SCtx *c = sctx(r);
    if (!c) return;
    snprintf(c->id, sizeof c->id, "EQUIP_FAILED");
    char id[24];
    /* a default item is the plain look: equipping it clears the slot */
    const char *equip = def && !cJSON_IsTrue(cJSON_GetObjectItem(def, "default")) ? item : "";
    social_call(s, &P_STRIKE_EQUIP, KN_DB_ARGS(acct_itoa(id, s->account_id), slot, equip), json_done, c);
}

/* the track slot of `slot_id`: the catalogue's, or "<L>_00" past the last level (1 spin ticket) */
static int track_reward(const char *slot_id, int *level, cJSON **rewards, int *choice_needed) {
    const cJSON *t = track_slot(slot_id);
    if (t) {
        *level = (int) kn_json_int(t, "level", 0);
        *rewards = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(t, "items"), 1);
        *choice_needed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "isPlayerChoice"));
        return 1;
    }
    int lvl = 0;
    char tail[8] = "";
    if (sscanf(slot_id, "%d_%7s", &lvl, tail) != 2 || strcmp(tail, "00")) return 0;
    const int max = (int) cfg_int("max_level", strike_economy()->max_level);
    if (lvl <= max || lvl > 100000) return 0;
    const cJSON *past = cfg("past_max_level_reward");
    *level = lvl;
    *rewards = cJSON_IsArray(past) ? cJSON_Duplicate(past, 1) : cJSON_CreateArray();
    *choice_needed = 0;
    return 1;
}

static void r_claim_progression(SocialSession *s, const GapiReq *r) {
    const char *slot = body_str(r, "slotId");
    int level = 0, choice_needed = 0;
    cJSON *rewards = NULL;
    if (!slot || !track_reward(slot, &level, &rewards, &choice_needed)) {
        gapi_error(s, r->rid, 404, "SLOT_NOT_FOUND", "Could not claim the reward");
        return;
    }
    int64_t choice = body_int(r, "choice", -1);
    if (choice_needed) {
        if (choice < 0 || choice >= cJSON_GetArraySize(rewards)) {
            cJSON_Delete(rewards);
            gapi_error(s, r->rid, 400, "CHOICE_REQUIRED", "Pick one");
            return;
        }
        cJSON *one = cJSON_CreateArray();
        cJSON_AddItemToArray(one, cJSON_DetachItemFromArray(rewards, (int) choice));
        cJSON_Delete(rewards);
        rewards = one;
    } else {
        choice = 0;
    }
    char *text = cJSON_PrintUnformatted(rewards);
    cJSON_Delete(rewards);
    SCtx *c = sctx(r);
    if (!c || !text) {
        free(c);
        free(text);
        return;
    }
    snprintf(c->id, sizeof c->id, "CLAIM_FAILED");
    char id[24], xp[24], ch[24];
    social_call(s, &P_STRIKE_TRACK_CLAIM,
                KN_DB_ARGS(acct_itoa(id, s->account_id), slot, acct_itoa(xp, strike_xp_for_level(level)), acct_itoa(ch, choice), text),
                json_done, c);
    free(text);
}

/* ------------------------------------------------------------------ the store */

/* the period a product's purchases count in: refresh windows since 1970, 0 = forever */
static int64_t product_period(const cJSON *p) {
    const int64_t refresh = (int64_t) kn_json_num(p, "refreshTime", 0);
    return refresh > 0 ? now_ms() / refresh : 0;
}

static int deal_candidate(const cJSON *def) {
    const int64_t rar = kn_json_int(def, "rarity", -1);
    return cJSON_IsTrue(cJSON_GetObjectItem(def, "spinnable")) && kn_json_int(def, "type", -1) == STRIKE_ITEM_WEAPON && rar >= 1 && rar <= 4;
}

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* today's deals (strike_daily_deals over the catalogue tables): products "deal_<day>_<item>" */
static cJSON *deals_today(void) {
    cJSON *out = cJSON_CreateArray();
    const int day = strike_day(now_ms());
    int candidates = 0;
    const cJSON *def;
    cJSON_ArrayForEach(def, s_cat.items) candidates += deal_candidate(def);
    int want = (int) cfg_int("deal_count", strike_economy()->deal_count);
    if (want > candidates) want = candidates;
    const cJSON *prices = cfg("deal_prices");
    uint64_t state = ((uint64_t) (uint32_t) day << 32) ^ 0x5354524B444CULL;
    const char *picked[32];
    int got = 0;
    for (int guard = 0; got < want && got < 32 && guard < 1000; guard++) {
        int k = (int) (splitmix(&state) % (uint64_t) candidates);
        const cJSON *hit = NULL;
        cJSON_ArrayForEach(def, s_cat.items) if (deal_candidate(def) && k-- == 0) {
            hit = def;
            break;
        }
        int dup = 0;
        for (int j = 0; j < got && hit; j++) dup |= !strcmp(picked[j], kn_json_str(hit, "_id", ""));
        if (!hit || dup) continue;
        picked[got++] = kn_json_str(hit, "_id", "");
        cJSON *p = cJSON_CreateObject();
        char pid[96];
        snprintf(pid, sizeof pid, "deal_%d_%s", day, kn_json_str(hit, "_id", ""));
        cJSON_AddStringToObject(p, "_id", pid);
        cJSON_AddStringToObject(p, "title", kn_json_str(hit, "name", ""));
        cJSON_AddStringToObject(p, "category", "deals");
        cJSON_AddBoolToObject(p, "active", 1);
        cJSON *pr = cJSON_CreateObject();
        char prid[100];
        snprintf(prid, sizeof prid, "%s_p0", pid);
        cJSON_AddStringToObject(pr, "_id", prid);
        cJSON_AddStringToObject(pr, "type", "virtual_currency");
        cJSON_AddStringToObject(pr, "currency", "kr");
        const cJSON *price = cJSON_GetArrayItem(prices, (int) kn_json_int(hit, "rarity", 0));
        cJSON_AddNumberToObject(pr, "amount", cJSON_IsNumber(price) ? price->valuedouble
                                                                    : strike_economy()->rarities[kn_json_int(hit, "rarity", 0)].deal_price);
        cJSON_AddItemToObject(pr, "conditions", cJSON_CreateArray());
        cJSON_AddBoolToObject(pr, "active", 1);
        cJSON_AddItemToArray(cJSON_AddArrayToObject(p, "price"), pr);
        cJSON *rw = cJSON_CreateObject();
        cJSON_AddStringToObject(rw, "type", "item");
        cJSON_AddStringToObject(rw, "item", kn_json_str(hit, "_id", ""));
        cJSON_AddItemToArray(cJSON_AddArrayToObject(p, "products"), rw);
        cJSON_AddNumberToObject(p, "refreshTime", 86400000.0);
        cJSON_AddNumberToObject(p, "maxPurchaseCount", 1);
        cJSON_AddItemToArray(out, p);
    }
    return out;
}

/* front.sections (FRVR's shop v2 front, kI.storeSpinWheelDetails): the SpinWheel section that mounts the
 * Lucky Draw's golden spin. Its spinWheels are the catalogue's golden_spin wheels (hideInShop: not in the
 * Spins list); its products are the ones that grant a golden spin ticket, with spinWheelType golden_spin
 * (FRVR's buildSpinButtonFromProduct). Built from the catalogue, so a wheel or product added there shows. */
static cJSON *front_json(void) {
    cJSON *front = cJSON_CreateObject();
    cJSON *sections = cJSON_AddArrayToObject(front, "sections");
    cJSON *sec = cJSON_CreateObject();
    cJSON_AddStringToObject(sec, "id", "lucky-draw");
    cJSON_AddStringToObject(sec, "titleKey", "store.section.spins");
    cJSON *content = cJSON_AddArrayToObject(sec, "content");
    cJSON *section = cJSON_CreateObject();
    cJSON_AddStringToObject(section, "type", "spin_wheel");
    cJSON *ws = cJSON_AddArrayToObject(section, "spinWheels");
    const cJSON *w;
    cJSON_ArrayForEach(w, s_cat.wheels) {
        if (strcmp(kn_json_str(w, "type", ""), "golden_spin")) continue;
        cJSON *wr = cJSON_CreateObject();
        cJSON_AddStringToObject(wr, "type", "spin_wheel_ref");
        cJSON_AddStringToObject(wr, "spinWheelId", kn_json_str(w, "_id", ""));
        cJSON_AddItemToArray(ws, wr);
    }
    cJSON *ps = cJSON_AddArrayToObject(section, "products");
    const cJSON *p;
    cJSON_ArrayForEach(p, s_cat.products) {
        const cJSON *rw;
        int golden = 0;
        cJSON_ArrayForEach(rw, cJSON_GetObjectItemCaseSensitive(p, "products"))
            if (!strcmp(kn_json_str(rw, "type", ""), "currency") && !strcmp(kn_json_str(rw, "currency", ""), "spin_golden_ticket")) golden = 1;
        if (!golden) continue;
        cJSON *ref = cJSON_CreateObject();
        cJSON *pr = cJSON_AddObjectToObject(ref, "productRef");
        cJSON_AddStringToObject(pr, "type", "product_ref");
        cJSON_AddStringToObject(pr, "productId", kn_json_str(p, "_id", ""));
        cJSON_AddStringToObject(ref, "spinWheelType", "golden_spin");
        cJSON_AddItemToArray(ps, ref);
    }
    cJSON_AddItemToArray(content, section);
    cJSON_AddItemToArray(sections, sec);
    return front;
}

/* products + sections; purchases = the account's strike_purchases rows (NULL for a guest) */
static cJSON *shop_json(const cJSON *purchases) {
    cJSON *o = cJSON_CreateObject();
    cJSON *products = cJSON_AddArrayToObject(o, "products");
    cJSON *deals = deals_today();
    const cJSON *lists[2] = {s_cat.products, deals};
    for (int l = 0; l < 2; l++) {
        const cJSON *p;
        cJSON_ArrayForEach(p, lists[l]) {
            cJSON *e = cJSON_Duplicate(p, 1);
            const int64_t period = product_period(p);
            int64_t bought = 0;
            const cJSON *row;
            cJSON_ArrayForEach(row, purchases)
                if (!strcmp(kn_json_str(row, "product_id", ""), kn_json_str(p, "_id", "")) && kn_json_int(row, "period", -1) == period)
                    bought = kn_json_int(row, "count", 0);
            cJSON_AddNumberToObject(e, "purchaseCount", (double) bought);
            if (kn_json_num(p, "refreshTime", 0) > 0)
                cJSON_AddNumberToObject(e, "timeSecondsToRefresh", (double) (((period + 1) * (int64_t) kn_json_num(p, "refreshTime", 0) - now_ms()) / 1000));
            cJSON_ReplaceItemInObject(e, "products", rewards_resolved(cJSON_GetObjectItemCaseSensitive(p, "products")));
            cJSON_AddItemToArray(products, e);
        }
    }
    cJSON_Delete(deals);
    /* sections (store.section.*): the wheels first, then products by category */
    static const char *const order[][2] = {{"spins", "store.section.spins"}, {"deals", "store.section.black.market.deals"},
                                           {"tickets", "store.section.tickets"}, {"bundles", "store.section.bundles"},
                                           {"free", "store.section.featured"}, {"currency", "store.section.currency"}};
    cJSON *sections = cJSON_AddArrayToObject(o, "sections");
    for (size_t i = 0; i < sizeof order / sizeof *order; i++) {
        cJSON *sec = cJSON_CreateObject();
        cJSON_AddStringToObject(sec, "id", order[i][0]);
        cJSON_AddStringToObject(sec, "titleKey", order[i][1]);
        cJSON *content = cJSON_AddArrayToObject(sec, "content");
        if (!strcmp(order[i][0], "spins")) {
            cJSON *ref = cJSON_CreateObject();
            cJSON_AddStringToObject(ref, "type", "spin_wheel");
            cJSON *ws = cJSON_AddArrayToObject(ref, "spinWheels");
            const cJSON *w;
            cJSON_ArrayForEach(w, s_cat.wheels) {
                if (cJSON_IsTrue(cJSON_GetObjectItem(w, "hideInShop"))) continue;
                cJSON *wr = cJSON_CreateObject();
                cJSON_AddStringToObject(wr, "type", "spin_wheel_ref");
                cJSON_AddStringToObject(wr, "spinWheelId", kn_json_str(w, "_id", ""));
                cJSON_AddItemToArray(ws, wr);
            }
            cJSON_AddItemToArray(content, ref);
        } else {
            const cJSON *p;
            cJSON_ArrayForEach(p, products) {
                if (strcmp(kn_json_str(p, "category", ""), order[i][0])) continue;
                cJSON *ref = cJSON_CreateObject();
                cJSON_AddStringToObject(ref, "type", "product_ref");
                cJSON_AddStringToObject(ref, "productId", kn_json_str(p, "_id", ""));
                cJSON_AddItemToArray(content, ref);
            }
        }
        if (cJSON_GetArraySize(content) > 0) cJSON_AddItemToArray(sections, sec);
        else cJSON_Delete(sec);
    }
    cJSON_AddItemToObject(o, "front", front_json());
    return o;
}

static void r_purchase(SocialSession *s, const GapiReq *r) {
    const char *pid = body_str(r, "productId");
    cJSON *deals = deals_today();
    const cJSON *p = pid ? find_by(s_cat.products, "_id", pid) : NULL;
    if (!p && pid) p = find_by(deals, "_id", pid);
    if (!p) {
        cJSON_Delete(deals);
        gapi_error(s, r->rid, 404, "PRODUCT_NOT_FOUND", "Product not found");
        return;
    }
    const cJSON *price = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "price"), 0);
    const char *ptype = kn_json_str(price, "type", "");
    if (strcmp(ptype, "virtual_currency") && strcmp(ptype, "free")) {
        cJSON_Delete(deals);
        gapi_error(s, r->rid, 400, "PAYMENT_UNAVAILABLE", "Real-money purchases are not available"); /* no real payments */
        return;
    }
    int64_t qty = body_int(r, "quantity", 1);
    if (qty < 1 || qty > 100) qty = 1;
    char *rewards = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(p, "products"));
    SCtx *c = sctx(r);
    if (!c || !rewards) {
        free(c);
        free(rewards);
        cJSON_Delete(deals);
        return;
    }
    snprintf(c->id, sizeof c->id, "PURCHASE_FAILED");
    char id[24], per[24], max[24], cost[24], q[24];
    social_call(s, &P_STRIKE_PURCHASE,
                KN_DB_ARGS(acct_itoa(id, s->account_id), kn_json_str(p, "_id", ""), acct_itoa(per, product_period(p)),
                           acct_itoa(max, kn_json_int(p, "maxPurchaseCount", 0)), kn_json_str(price, "currency", ""),
                           acct_itoa(cost, strcmp(ptype, "free") ? kn_json_int(price, "amount", 0) : 0), rewards, acct_itoa(q, qty)),
                json_done, c);
    free(rewards);
    cJSON_Delete(deals);
}

static int r_shop(SocialSession *s, const GapiReq *r) {
    if (!need_catalog(s, r)) return 1;
    if (gapi_is(r, "GET", "/strike/v1/shop") || gapi_is(r, "GET", "/strike/v2/shop")) {
        if (s->account_id > 0) me_call(s, r, ME_SHOP);
        else gapi_reply(s, r->rid, shop_json(NULL));
    } else if (gapi_is(r, "POST", "/strike/v1/shop/purchase")) {
        if (need_login(s, r)) r_purchase(s, r);
    } else if (gapi_is(r, "POST", "/strike/v1/shop/validate")) {
        gapi_error(s, r->rid, 400, "PAYMENT_UNAVAILABLE", "Real-money purchases are not available");
    } else {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ missions */

/* today's missions of an account: weighted, seeded by (day, account) as strike_daily_missions */
static int daily_missions(int day, int64_t account, const cJSON **out, int want) {
    const int total_n = cJSON_GetArraySize(s_cat.missions);
    if (want > total_n) want = total_n;
    if (want > 16) want = 16;
    uint64_t state = ((uint64_t) (uint32_t) day << 32) ^ (uint64_t) account ^ 0x5354524B4D49ULL;
    int taken[256] = {0}, got = 0;
    while (got < want) {
        int total = 0, i = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, s_cat.missions) {
            if (i < 256 && !taken[i] && kn_json_int(m, "weight", 1) > 0) total += (int) kn_json_int(m, "weight", 1);
            i++;
        }
        if (total <= 0) break;
        int rr = (int) (splitmix(&state) % (uint64_t) total);
        i = 0;
        cJSON_ArrayForEach(m, s_cat.missions) {
            const int w = (int) kn_json_int(m, "weight", 1);
            if (i < 256 && !taken[i] && w > 0) {
                if (rr < w) {
                    taken[i] = 1;
                    out[got++] = m;
                    break;
                }
                rr -= w;
            }
            i++;
        }
    }
    return got;
}

/* "id:key:required:max,..." of today's missions */
static void mission_spec(int day, int64_t account, char *out, size_t size) {
    const cJSON *ms[16];
    const int n = daily_missions(day, account, ms, (int) cfg_int("daily_mission_count", strike_economy()->daily_mission_count));
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len < size; i++)
        len += (size_t) snprintf(out + len, size - len, "%s%s:%s:%d:%d", i ? "," : "", kn_json_str(ms[i], "_id", ""), kn_json_str(ms[i], "key", ""),
                                 (int) kn_json_int(ms[i], "requiredValue", 1), cJSON_IsTrue(cJSON_GetObjectItem(ms[i], "best")));
}

static void missions_done(SocialSession *s, kn_db_result *res, void *extra) {
    SCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        fail_db(s, c, res, "MISSIONS_FAILED");
        return;
    }
    const int unlock = (int) cfg_int("missions_unlock_level", strike_economy()->missions_unlock_level);
    const int level = strike_level_for_xp(kn_json_int(v, "xp", 0)).level;
    cJSON *o = cJSON_CreateObject();
    cJSON *daily = cJSON_AddObjectToObject(o, "dailyMissions");
    cJSON *list = cJSON_AddArrayToObject(daily, "missions");
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(v, "missions")) {
        const cJSON *def = mission_def(kn_json_str(row, "id", ""));
        if (!def) continue;
        cJSON *m = cJSON_CreateObject();
        cJSON *details = cJSON_Duplicate(def, 1);
        cJSON_ReplaceItemInObject(details, "rewards", rewards_resolved(cJSON_GetObjectItemCaseSensitive(def, "rewards")));
        cJSON_AddItemToObject(m, "details", details);
        cJSON_AddNumberToObject(m, "progress", (double) kn_json_int(row, "progress", 0));
        cJSON_AddBoolToObject(m, "completed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "completed")) || kn_json_int(row, "completed", 0));
        cJSON_AddBoolToObject(m, "claimed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "claimed")) || kn_json_int(row, "claimed", 0));
        cJSON_AddItemToArray(list, m);
    }
    const int64_t now = now_ms();
    cJSON_AddNumberToObject(daily, "updatedAt", (double) (now - now % 86400000LL));
    cJSON_AddNumberToObject(daily, "timeSecondsToRefresh", (double) ((86400000LL - now % 86400000LL) / 1000));
    cJSON_AddItemToObject(o, "missions", cJSON_CreateArray());
    cJSON_AddBoolToObject(o, "locked", level < unlock);
    cJSON_AddNumberToObject(o, "unlockLevel", unlock);
    cJSON_Delete(v);
    gapi_reply(s, c->rid, o);
}

static void mission_claim_done(SocialSession *s, kn_db_result *res, void *extra) {
    SCtx *c = extra;
    if (!kn_db_ok(res)) {
        fail_db(s, c, res, "MISSION_CLAIM_FAILED");
        return;
    }
    cJSON *v = acct_db_json(res);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rewards", rewards_resolved(cJSON_GetObjectItemCaseSensitive(v, "rewards")));
    cJSON_AddItemToObject(o, "wallet", cJSON_DetachItemFromObject(v, "wallet"));
    cJSON_AddItemToObject(o, "playerMissions", cJSON_DetachItemFromObject(v, "missions"));
    cJSON_Delete(v);
    gapi_reply(s, c->rid, o);
}

static int r_missions(SocialSession *s, const GapiReq *r) {
    if (!need_catalog(s, r)) return 1;
    const int day = strike_day(now_ms());
    char id[24], sday[24];
    if (gapi_is(r, "GET", "/strike/v1/missions/me")) {
        if (!need_login(s, r)) return 1;
        char spec[1024];
        mission_spec(day, s->account_id, spec, sizeof spec);
        SCtx *c = sctx(r);
        if (c) social_call(s, &P_STRIKE_MISSIONS, KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(sday, day), spec), missions_done, c);
        return 1;
    }
    if (gapi_is(r, "POST", "/strike/v1/missions/claim/:id")) {
        if (!need_login(s, r)) return 1;
        const cJSON *def = mission_def(r->seg[4]);
        if (!def) {
            gapi_error(s, r->rid, 404, "MISSION_NOT_FOUND", "Mission not found");
            return 1;
        }
        char *rewards = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(def, "rewards"));
        SCtx *c = sctx(r);
        if (c && rewards) {
            char xp[24];
            const int unlock = (int) cfg_int("missions_unlock_level", strike_economy()->missions_unlock_level);
            social_call(s, &P_STRIKE_MISSION_CLAIM,
                        KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(sday, day), r->seg[4], rewards, acct_itoa(xp, strike_xp_for_level(unlock))),
                        mission_claim_done, c);
        } else {
            free(c);
        }
        free(rewards);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ game config, leaderboards */

static cJSON *game_config(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON *track = cJSON_AddObjectToObject(o, "accountProgression");
    const cJSON *t;
    cJSON_ArrayForEach(t, s_cat.track) {
        char lvl[16];
        snprintf(lvl, sizeof lvl, "%d", (int) kn_json_int(t, "level", 0));
        cJSON *e = cJSON_Duplicate(t, 1);
        cJSON_ReplaceItemInObject(e, "items", rewards_resolved(cJSON_GetObjectItemCaseSensitive(t, "items")));
        cJSON_AddItemToObject(track, lvl, e);
    }
    cJSON_AddItemToObject(o, "accountProgressionPremium", cJSON_CreateObject()); /* no premium track: no real money */
    cJSON *sw = cJSON_AddObjectToObject(o, "spinWheels");
    cJSON_AddNumberToObject(sw, "spinsToGetGoldenSpin", (double) cfg_int("golden_spin_meter", strike_economy()->golden_spin_meter));
    cJSON_AddStringToObject(sw, "luckyDraw", "lucky_draw");
    cJSON_AddStringToObject(sw, "goldenSpin", "golden_spin");
    cJSON *m = cJSON_AddObjectToObject(o, "missions");
    cJSON_AddNumberToObject(m, "unlockLevel", (double) cfg_int("missions_unlock_level", strike_economy()->missions_unlock_level));
    cJSON_AddNumberToObject(m, "dailyCount", (double) cfg_int("daily_mission_count", strike_economy()->daily_mission_count));
    cJSON *xp = cJSON_AddObjectToObject(o, "xpCurve");
    cJSON_AddNumberToObject(xp, "base", strike_economy()->xp_base);
    cJSON_AddNumberToObject(xp, "step", strike_economy()->xp_step);
    cJSON_AddNumberToObject(xp, "divisor", strike_economy()->xp_divisor);
    cJSON_AddNumberToObject(o, "maxLevelCap", (double) cfg_int("max_level", strike_economy()->max_level));
    const cJSON *past = cfg("past_max_level_reward");
    cJSON_AddItemToObject(o, "pastMaxLevelReward", past ? cJSON_Duplicate(past, 1) : cJSON_CreateArray());
    cJSON *cur = cJSON_AddArrayToObject(o, "currencies");
    for (int i = 0; i < STRIKE_CUR_COUNT; i++) cJSON_AddItemToArray(cur, cJSON_CreateString(strike_currency_key((StrikeCurrency) i)));
    cJSON *rar = cJSON_AddArrayToObject(o, "rarities");
    for (int i = 0; i < STRIKE_RARITY_COUNT; i++) {
        const StrikeRarity *rr = strike_rarity(i);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", rr->name);
        cJSON_AddStringToObject(e, "color", rr->color);
        cJSON_AddStringToObject(e, "language", rr->i18n);
        cJSON_AddItemToArray(rar, e);
    }
    return o;
}

static void board_done(SocialSession *s, kn_db_result *res, void *extra) {
    SCtx *c = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        fail_db(s, c, res, "LEADERBOARD_FAILED");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "board", kn_json_str(v, "board", ""));
    cJSON *players = cJSON_AddArrayToObject(o, "players");
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(v, "rows")) {
        cJSON *p = cJSON_CreateObject();
        char pid[24];
        cJSON_AddNumberToObject(p, "rank", (double) kn_json_int(row, "rank", 0));
        cJSON_AddStringToObject(p, "playerId", acct_itoa(pid, kn_json_int(row, "id", 0)));
        cJSON_AddStringToObject(p, "name", kn_json_str(row, "name", ""));
        cJSON_AddNumberToObject(p, "value", (double) kn_json_int(row, "value", 0));
        cJSON_AddNumberToObject(p, "level", strike_level_for_xp(kn_json_int(row, "xp", 0)).level);
        cJSON_AddItemToArray(players, p);
    }
    const cJSON *me = cJSON_GetObjectItemCaseSensitive(v, "me");
    cJSON_AddItemToObject(o, "me", cJSON_IsObject(me) ? cJSON_Duplicate(me, 1) : cJSON_CreateNull());
    cJSON_Delete(v);
    gapi_reply(s, c->rid, o);
}

static int r_misc(SocialSession *s, const GapiReq *r) {
    if (gapi_is(r, "GET", "/strike/v1/game-config")) {
        if (need_catalog(s, r)) gapi_reply(s, r->rid, game_config());
        return 1;
    }
    if (gapi_is(r, "GET", "/strike/v1/leaderboards/:board")) {
        char lim[16], off[16], id[24], slim[24], soff[24];
        int64_t limit = gapi_query(r, "limit", lim, sizeof lim) ? strtoll(lim, NULL, 10) : 50;
        int64_t offset = gapi_query(r, "offset", off, sizeof off) ? strtoll(off, NULL, 10) : 0;
        if (limit < 1 || limit > 100) limit = 50;
        if (offset < 0 || offset > 100000) offset = 0;
        SCtx *c = sctx(r);
        if (c) social_call(s, &P_STRIKE_LEADERBOARD, KN_DB_ARGS(r->seg[3], acct_itoa(slim, limit), acct_itoa(soff, offset),
                                                                 acct_itoa(id, s->account_id > 0 ? s->account_id : 0)), board_done, c);
        return 1;
    }
    if (!need_catalog(s, r)) return 1;
    if (gapi_is(r, "GET", "/strike/v1/players/me")) {
        if (need_login(s, r)) me_call(s, r, ME_PLAYER);
    } else if (gapi_is(r, "GET", "/strike/v1/players/me/items")) {
        if (need_login(s, r)) me_call(s, r, ME_ITEMS);
    } else if (gapi_is(r, "GET", "/strike/v1/players/me/classes")) {
        if (need_login(s, r)) me_call(s, r, ME_CLASSES);
    } else if (gapi_is(r, "POST", "/strike/v1/players/me/equip")) {
        if (need_login(s, r)) r_equip(s, r);
    } else if (gapi_is(r, "POST", "/strike/v1/players/me/claim/progression")) {
        if (need_login(s, r)) r_claim_progression(s, r);
    } else {
        return 0;
    }
    return 1;
}

int strike_econ_gapi(SocialSession *s, const GapiReq *r) {
    if (r->nseg < 2 || strcmp(r->seg[0], "strike")) return 0;
    if (r->nseg >= 3 && !strcmp(r->seg[2], "item-definitions")) return r_items(s, r);
    if (r->nseg >= 3 && !strcmp(r->seg[2], "spin-wheels")) return r_wheels(s, r);
    if (r->nseg >= 3 && !strcmp(r->seg[2], "shop")) return r_shop(s, r);
    if (r->nseg >= 3 && !strcmp(r->seg[2], "missions")) return r_missions(s, r);
    return r_misc(s, r);
}

/* ------------------------------------------------------------------ HTTP leaderboards */

static void http_board_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    cJSON *o = cJSON_CreateObject(), *data = cJSON_AddArrayToObject(o, "data");
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(v, "rows")) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "position", (double) kn_json_int(row, "rank", 0));
        cJSON_AddStringToObject(e, "player_name", kn_json_str(row, "name", ""));
        cJSON_AddStringToObject(e, "player_clan", "");
        cJSON_AddNumberToObject(e, "value", (double) kn_json_int(row, "value", 0));
        cJSON_AddNumberToObject(e, "level", strike_level_for_xp(kn_json_int(row, "xp", 0)).level);
        cJSON_AddItemToArray(data, e);
    }
    cJSON_Delete(v);
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

void strike_econ_http_board(Brain *b, kn_http_req *req, const char *type) {
    static const char *const map[][2] = {{"xp", "xp"}, {"level", "xp"}, {"score", "score"}, {"player_score", "score"},
                                         {"kills", "kills"}, {"player_kills", "kills"}, {"wins", "wins"}, {"player_wins", "wins"},
                                         {"games", "games"}, {"player_games_played", "games"}};
    const char *board = NULL;
    for (size_t i = 0; type && i < sizeof map / sizeof *map; i++)
        if (!strcmp(type, map[i][0])) board = map[i][1];
    if (!board || !b || !b->db || kn_db_call(b->db, &P_STRIKE_LEADERBOARD, KN_DB_ARGS(board, "100", "0", "0"), http_board_done, req) != 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddItemToObject(o, "data", cJSON_CreateArray());
        brain_reply_json(req, 200, o);
        cJSON_Delete(o);
    }
}

static void h_http_board(kn_http_req *req, void *ud) { strike_econ_http_board(ud, req, kn_http_param(req, "type")); }

/* ------------------------------------------------------------------ Strike game servers */

typedef struct OverlayCtx {
    cJSON *account;
    AcctReplyFn cb;
    void *ud;
} OverlayCtx;

static void overlay_done(kn_db_result *res, void *ud) {
    OverlayCtx *o = ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        /* no Strike data: the account joins owning nothing of Strike's (never Box Office's items) */
        KN_WARN(MOD, "Strike overlay: %s", kn_db_ok(res) ? "no data" : kn_db_error(res));
        v = cJSON_CreateObject();
    }
    cJSON *skins = cJSON_CreateArray();
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(v, "items")) {
        if (kn_json_int(it, "skin", -1) < 0) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ind", (double) kn_json_int(it, "skin", -1));
        cJSON_AddNumberToObject(e, "cnt", (double) kn_json_int(it, "count", 1));
        cJSON_AddItemToArray(skins, e);
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(o->account, "data");
    if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 10) cJSON_ReplaceItemInArray(data, 10, cJSON_Duplicate(skins, 1));
    cJSON_DeleteItemFromObject(o->account, "skins");
    cJSON_AddItemToObject(o->account, "skins", skins);
    const int64_t xp = kn_json_int(v, "xp", 0);
    cJSON_ReplaceItemInObject(o->account, "funds", cJSON_CreateNumber((double) kn_json_int(v, "kr", 0)));
    cJSON_DeleteItemFromObject(o->account, "level");
    cJSON_AddNumberToObject(o->account, "level", strike_level_for_xp(xp).level);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "xp", (double) xp);
    cJSON_AddNumberToObject(st, "level", strike_level_for_xp(xp).level);
    cJSON_AddItemToObject(st, "loadout", cJSON_DetachItemFromObject(v, "loadout"));
    cJSON_DeleteItemFromObject(o->account, "strike");
    cJSON_AddItemToObject(o->account, "strike", st);
    cJSON_Delete(v);
    o->cb(o->ud, NULL, o->account);
    cJSON_Delete(o->account);
    free(o);
}

void strike_econ_overlay(Brain *b, const cJSON *account, AcctReplyFn cb, void *ud) {
    const int64_t id = kn_json_int(account, "id", 0);
    OverlayCtx *o = calloc(1, sizeof *o);
    if (!o || !b || !b->db || id <= 0) {
        free(o);
        cb(ud, NULL, account);
        return;
    }
    o->account = cJSON_Duplicate(account, 1);
    o->cb = cb;
    o->ud = ud;
    char sid[24];
    if (kn_db_call(b->db, &P_STRIKE_OVERLAY, KN_DB_ARGS(acct_itoa(sid, id)), overlay_done, o) != 0) {
        cb(ud, NULL, account);
        cJSON_Delete(o->account);
        free(o);
    }
}

typedef struct CommitCtx {
    int64_t account;
    AcctReplyFn cb;
    void *ud;
} StrikeCommitCtx;

static void commit_done(kn_db_result *res, void *ud) {
    StrikeCommitCtx *c = ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        c->cb(c->ud, acct_db_error(res, MOD), NULL);
        free(c);
        return;
    }
    const int before = strike_level_for_xp(kn_json_int(v, "xp_before", 0)).level, after = strike_level_for_xp(kn_json_int(v, "xp", 0)).level;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "accountId", (double) c->account);
    cJSON_AddNumberToObject(o, "xp", kn_json_num(v, "xp", 0));
    cJSON_AddNumberToObject(o, "level", after);
    cJSON_AddBoolToObject(o, "levelUp", after > before);
    cJSON_AddNumberToObject(o, "kr", kn_json_num(v, "kr", 0));
    cJSON_AddBoolToObject(o, "duplicate", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "duplicate")));
    cJSON_AddItemToObject(o, "missions", cJSON_DetachItemFromObject(v, "missions"));
    cJSON_Delete(v);
    c->cb(c->ud, NULL, o);
    cJSON_Delete(o);
    free(c);
}

void strike_econ_commit(Brain *b, int64_t account_id, const cJSON *player, const char *match_key, AcctReplyFn cb, void *ud) {
    if (!b || !b->db || account_id <= 0) {
        cb(ud, "no database", NULL);
        return;
    }
    StrikeCommitCtx *c = calloc(1, sizeof *c);
    char *payload = cJSON_PrintUnformatted(player);
    if (!c || !payload) {
        free(c);
        free(payload);
        cb(ud, "Server error.", NULL);
        return;
    }
    c->account = account_id;
    c->cb = cb;
    c->ud = ud;
    const int day = strike_day(now_ms());
    char spec[1024] = "";
    if (s_loaded) mission_spec(day, account_id, spec, sizeof spec);
    char id[24], sday[24], maxkr[24];
    if (kn_db_call(b->db, &P_STRIKE_COMMIT,
                   KN_DB_ARGS(acct_itoa(id, account_id), match_key, payload, acct_itoa(sday, day), spec, acct_itoa(maxkr, g_acct.max_match_kr)),
                   commit_done, c) != 0) {
        free(c);
        cb(ud, "Server error.", NULL);
    }
    free(payload);
}
