/*
 * services/brain/db_storage.c - KrunkScript persistence for game servers
 * (doc/v7/krunkscript.md 2.6 / WP6, doc/backend.md 7.6 ops "storage",
 * "storage_load", "map_reward"): the v7.1.4 database procs 154 (storageUpdt),
 * 163 (getStoragePlayer) and 137 (rewardKR) on table ks_storage
 * (migrations 0300_ks_storage.sql). C on every driver (no .pg text): the value
 * rules are shared with the game server's offline store
 * (shared/src/ks_storage_data.c), so both stores behave the same.
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "knsvc/json.h"
#include "ks_storage_data.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* the published map of a name: {map_id, creator_id} or NULL */
static cJSON *published_map(Dbx *x, const char *name) {
    if (!name || !*name) return NULL;
    return dbx_row(x, SQL(.any = "SELECT map_id, creator_id FROM published_maps"
                                 " WHERE lower(map_name) = lower($1) AND deleted_at IS NULL ORDER BY map_id LIMIT 1"),
                   KN_DB_ARGS(name));
}

static void add_row(cJSON *rows, const char *key, const char *value) {
    cJSON *row = cJSON_CreateArray();
    cJSON_AddItemToArray(row, cJSON_CreateString(key));
    cJSON_AddItemToArray(row, cJSON_CreateString(value));
    cJSON_AddItemToArray(rows, row);
}

static kn_db_result *rows_result(Dbx *x, cJSON *rows) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rows", rows);
    return dbx_json(x, "jsonb_build_object", o);
}

/* (account id, account name, map name, type, scope, data [[key, value]...]) -> {rows: [[key, value]...]}:
 * the touched keys as stored after the operation (deleted ones absent). */
static kn_db_result *run_storage_update(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t id = PI(0);
    if (id <= 0) id = db_account_id(&x, P(1));
    if (id <= 0) {
        dbx_refuse(&x, "Player not connected");
        return dbx_finish(&x, NULL);
    }
    cJSON *map = published_map(&x, P(2));
    if (!map) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Storage only supported on published maps");
        return dbx_finish(&x, NULL);
    }
    const int64_t map_id = kn_json_int(map, "map_id", 0);
    cJSON_Delete(map);
    const int64_t type = PI(3), scope = PI(4) == 1 ? 1 : 0;
    cJSON *data = P(5) ? cJSON_Parse(P(5)) : NULL;
    /* the game server validated it already; the database keeps the same rules */
    KsStorageData pairs;
    ks_storage_data_init(&pairs);
    const cJSON *pair;
    cJSON_ArrayForEach(pair, data) {
        const char *k = cJSON_GetStringValue(cJSON_GetArrayItem(pair, 0)), *v = cJSON_GetStringValue(cJSON_GetArrayItem(pair, 1));
        ks_storage_data_add(&pairs, k ? k : "", v ? v : "");
    }
    const char *err = cJSON_IsArray(data) ? ks_storage_data_error(&pairs) : "Missing storage data";
    cJSON_Delete(data);
    if (!err && (type < 0 || type >= KS_STORAGE_TYPE_COUNT)) err = "Invalid storage type";
    if (err) {
        dbx_refuse(&x, err);
        return dbx_finish(&x, NULL);
    }
    char ids[24], maps[24], scopes[24];
    snprintf(ids, sizeof ids, "%lld", (long long) id);
    snprintf(maps, sizeof maps, "%lld", (long long) map_id);
    snprintf(scopes, sizeof scopes, "%lld", (long long) scope);
    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < pairs.count && !dbx_failed(&x); i++) {
        const KsStoragePair *kv = &pairs.pairs[i];
        char *old = dbx_text(&x, SQL(.any = "SELECT svalue FROM ks_storage WHERE account_id = $1 AND map_id = $2 AND skey = $3 {for_update}"),
                             KN_DB_ARGS(ids, maps, kv->key));
        char value[KS_STORAGE_VALUE_BYTES + 32];
        const int verdict = ks_storage_apply((int) type, old, kv->value, value, sizeof value);
        if (verdict < 0) {
            dbx_exec(&x, SQL(.any = "DELETE FROM ks_storage WHERE account_id = $1 AND map_id = $2 AND skey = $3"), KN_DB_ARGS(ids, maps, kv->key));
        } else if (verdict > 0 && old) {
            dbx_exec(&x, SQL(.any = "UPDATE ks_storage SET svalue = $4, scope = {int:$5}, updated_at = {now}"
                                    " WHERE account_id = $1 AND map_id = $2 AND skey = $3"),
                     KN_DB_ARGS(ids, maps, kv->key, value, scopes));
        } else if (verdict > 0) {
            dbx_exec(&x, SQL(.any = "INSERT INTO ks_storage (account_id, map_id, skey, svalue, scope) VALUES ($1, $2, $3, $4, {int:$5})"),
                     KN_DB_ARGS(ids, maps, kv->key, value, scopes));
        }
        const char *now = verdict > 0 ? value : verdict == 0 ? old : NULL;
        if (now) add_row(rows, kv->key, now);
        free(old);
    }
    return rows_result(&x, rows);
}
const kn_proc P_STORAGE_UPDATE = {"storage", NULL, run_storage_update};

/* (account name, this map, other map or NULL) -> {rows: [[key, value]...]} by key: every row of
 * this map, or the global rows of another map with the same creator (v7 passes
 * customMapData.creator to proc 163). An unknown account / map has no rows. */
static kn_db_result *run_storage_load(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = cJSON_CreateArray();
    cJSON *self = published_map(&x, P(1));
    if (!self) {
        cJSON_Delete(rows);
        if (!dbx_failed(&x)) dbx_refuse(&x, "Storage only supported on published maps");
        return dbx_finish(&x, NULL);
    }
    const int64_t id = db_account_id(&x, P(0));
    const char *this_map = P(1) ? P(1) : "", *from = P(2);
    if (from && (!*from || !strcasecmp(from, this_map))) from = NULL;
    cJSON *other = from ? published_map(&x, from) : NULL;
    const int64_t creator = kn_json_int(self, "creator_id", 0);
    int64_t map_id = kn_json_int(self, "map_id", 0);
    int global_only = 0;
    if (from) {
        map_id = other && creator > 0 && kn_json_int(other, "creator_id", 0) == creator ? kn_json_int(other, "map_id", 0) : 0;
        global_only = 1;
    }
    cJSON_Delete(self);
    cJSON_Delete(other);
    if (id > 0 && map_id > 0) {
        char ids[24], maps[24];
        snprintf(ids, sizeof ids, "%lld", (long long) id);
        snprintf(maps, sizeof maps, "%lld", (long long) map_id);
        cJSON *found = global_only
            ? dbx_rows(&x, SQL(.any = "SELECT skey, svalue FROM ks_storage WHERE account_id = $1 AND map_id = $2 AND scope = 1 ORDER BY skey"),
                       KN_DB_ARGS(ids, maps))
            : dbx_rows(&x, SQL(.any = "SELECT skey, svalue FROM ks_storage WHERE account_id = $1 AND map_id = $2 ORDER BY skey"),
                       KN_DB_ARGS(ids, maps));
        const cJSON *r;
        cJSON_ArrayForEach(r, found) {
            const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(r, "skey"));
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(r, "svalue"));
            if (k && v) add_row(rows, k, v);
        }
        cJSON_Delete(found);
    }
    return rows_result(&x, rows);
}
const kn_proc P_STORAGE_LOAD = {"storage_load", NULL, run_storage_load};

/* (account id, amount, map name) -> {rsp, funds?}: rewardKR (proc 137) pays the KR out of the
 * published map's funds (published_maps.map_funds); rsp "Success" with the account's funds. */
static kn_db_result *run_map_reward(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), amount = PI(1);
    cJSON *o = cJSON_CreateObject();
    cJSON *map = published_map(&x, P(2));
    if (amount <= 0 || !map) {
        cJSON_AddStringToObject(o, "rsp", map ? "Invalid amount" : "Rewards only on published maps");
        cJSON_Delete(map);
        return dbx_json(&x, "jsonb_build_object", o);
    }
    char maps[24], amounts[24];
    snprintf(maps, sizeof maps, "%lld", (long long) kn_json_int(map, "map_id", 0));
    snprintf(amounts, sizeof amounts, "%lld", (long long) amount);
    cJSON_Delete(map);
    if (dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_funds = map_funds - {int:$2} WHERE map_id = $1 AND map_funds >= {int:$2}"),
                 KN_DB_ARGS(maps, amounts)) == 0) {
        cJSON_AddStringToObject(o, "rsp", "Not enough map funds");
        return dbx_json(&x, "jsonb_build_object", o);
    }
    int64_t funds = 0;
    db_add_funds(&x, id, amount, "map_reward", P(2), &funds); /* a refusal rolls the map funds back */
    cJSON_AddStringToObject(o, "rsp", "Success");
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_MAP_REWARD = {"map_reward", NULL, run_map_reward};
