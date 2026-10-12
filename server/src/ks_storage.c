// Account persistence and services of the game server: GAME.STORAGE, Save / Load Data, KR
// rewards (server/include/ks_storage.h, doc/v7/krunkscript.md 2.6 and WP6).
#include <ks_storage.h>
#include <server_accounts.h>
#include <server_sandbox.h>
#include <server_log.h>
#include <brain.h>

#include <triggers_v7.h> // trigger_load_data_apply
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define KS_STORAGE_TIMEOUT 15.0f        // s: a brain request without its reply fails ("Error")
#define KS_STORAGE_FLUSH_INTERVAL 1.0f  // s: offline documents are written at most this often
#define KS_STORAGE_DOCS 4               // cached offline documents (this map + global loads)

enum { BACKEND_NONE, BACKEND_OFFLINE, BACKEND_BRAIN };
enum { REQ_STORAGE, REQ_LOAD, REQ_REWARD };

typedef struct KsStorageReq {
    struct KsStorageReq *next;
    int kind;
    uint32_t rid;           // brain request id, 0 = not waiting for the brain
    int ready;              // completed: delivered by the next ks_storage_poll
    float deadline;         // g_server->now
    KsStorageDone done;
    void *user;
    KsStoragePair *rows;
    int count;
    char error[128];
    int has_error;
    // REQ_REWARD
    int reward_conn;        // ClientConnection.id
    int reward_amount;
    int reward_funds;       // the account's KR after the reward, -1 unknown
} KsStorageReq;

typedef struct {
    char key[40];           // offline store key (ks_storage_offline_key)
    char map[128];
    cJSON *doc;             // {"map": name, "accounts": {name: {key: {"v": value, "g": 0|1}}}}
    int dirty;
    unsigned age;
} KsStorageDoc;

static struct {
    KsStorageReq *reqs;
    KsStorageDoc docs[KS_STORAGE_DOCS];
    unsigned doc_clock;
    float next_flush;
    KsStorageRead read;
    KsStorageWrite write;
    void (*sink)(cJSON *frame);
    int waiting_rewards[64]; // connection ids with a reward in flight (player.waitRew)
    int waiting_count;
} g_st;

// ------------------------------------------------------------------ helpers

static int backend(void) {
    if (g_st.sink) return BACKEND_BRAIN;
    if (sandbox_running()) return BACKEND_OFFLINE;
    if (brain_enabled()) return BACKEND_BRAIN;
    return BACKEND_NONE;
}

static const char *current_map_name(void) {
    const Game *game = g_server ? &g_server->game : NULL;
    if (!game || !game->maps || game->current_map_index < 0 || (size_t) game->current_map_index >= game->map_count) return NULL;
    const cJSON *name = cJSON_GetObjectItem(game->maps[game->current_map_index], "name");
    return cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : NULL;
}

// The map storage is keyed by, or NULL with *err: v7 checks !dbClient, then defaultSetts in
// production, then customMapData.n. The sandbox is v7's non-production server: any named map.
// Online the lobby must host a published map (kn_brain checks the name against its catalogue).
static const char *storage_map(const int be, const char **err) {
    *err = NULL;
    if (be == BACKEND_NONE) {
        *err = "Error";
        return NULL;
    }
    if (be == BACKEND_BRAIN && !g_st.sink) {
        const char *published = accounts_published_map();
        if (!published) *err = "Storage only supported on published maps";
        return published;
    }
    const char *name = current_map_name();
    if (!name) *err = "Missing map data";
    return name;
}

int ks_storage_account_of(const ClientConnection *client, char *name, const size_t n) {
    if (!client) return 0;
    if (sandbox_running()) {
        snprintf(name, n, "%s", KS_STORAGE_OFFLINE_ACCOUNT);
        return 1;
    }
    if (client->acct.account_id <= 0 || !client->acct.username[0]) return 0;
    snprintf(name, n, "%s", client->acct.username);
    return 1;
}

// players.findByName over the account names of the connections
static ClientConnection *client_by_account(const char *name) {
    for (size_t i = 0; name && name[0] && g_server && i < g_server->client_count; i++) {
        char account[64];
        ClientConnection *client = &g_server->clients[i];
        if (ks_storage_account_of(client, account, sizeof(account)) && !strcasecmp(account, name)) return client;
    }
    return NULL;
}

static ClientConnection *client_by_id(const int id) {
    for (size_t i = 0; g_server && i < g_server->client_count; i++)
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    return NULL;
}

static ClientConnection *client_of_player(const Player *player) {
    for (size_t i = 0; player && g_server && i < g_server->client_count; i++)
        if (g_server->clients[i].player == player) return &g_server->clients[i];
    return NULL;
}

static KsStorageReq *req_new(const int kind, KsStorageDone done, void *user) {
    KsStorageReq *req = calloc(1, sizeof(*req));
    if (!req) return NULL;
    req->kind = kind;
    req->done = done;
    req->user = user;
    KsStorageReq **tail = &g_st.reqs;
    while (*tail) tail = &(*tail)->next;
    *tail = req;
    return req;
}

static void req_fail(KsStorageReq *req, const char *error) {
    req->rid = 0;
    req->ready = 1;
    req->has_error = 1;
    // getStoragePlayer: any database failure is "No data found" (game.js:1233-1238)
    snprintf(req->error, sizeof(req->error), "%s", req->kind == REQ_LOAD ? "No data found" : error && error[0] ? error : "Error");
}

static void req_add_row(KsStorageReq *req, const char *key, const char *value) {
    KsStoragePair *rows = realloc(req->rows, (size_t) (req->count + 1) * sizeof(*rows));
    if (!rows) return;
    req->rows = rows;
    snprintf(rows[req->count].key, sizeof(rows[0].key), "%s", key ? key : "");
    snprintf(rows[req->count].value, sizeof(rows[0].value), "%s", value ? value : "");
    req->count++;
}

static int row_cmp(const void *a, const void *b) {
    return strcmp(((const KsStoragePair *) a)->key, ((const KsStoragePair *) b)->key);
}

// [KN] load results come in key order (bytewise) from both stores: v7's row order was the database's
static void req_sort_rows(KsStorageReq *req) {
    if (req->count > 1) qsort(req->rows, (size_t) req->count, sizeof(*req->rows), row_cmp);
}

static void req_free(KsStorageReq *req) {
    free(req->rows);
    free(req);
}

static void reward_unwait(const int conn) {
    for (int i = 0; i < g_st.waiting_count; i++) {
        if (g_st.waiting_rewards[i] != conn) continue;
        g_st.waiting_rewards[i] = g_st.waiting_rewards[--g_st.waiting_count];
        return;
    }
}

static int reward_waiting(const int conn) {
    for (int i = 0; i < g_st.waiting_count; i++)
        if (g_st.waiting_rewards[i] == conn) return 1;
    return 0;
}

// ------------------------------------------------------------------ offline store

void ks_storage_offline_key(const char *map_name, char *out, const size_t n) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *) (map_name ? map_name : ""); *p; p++) h = (h ^ *p) * 16777619u;
    snprintf(out, n, "ks-storage-%08x", h);
}

void ks_storage_set_offline_io(KsStorageRead read, KsStorageWrite write) {
    g_st.read = read;
    g_st.write = write;
}

// Built-in I/O: html_dom_core.c's storage layout ($XDG_CONFIG_HOME/krunknative/<key>.json).
static int file_dir(char *out, const size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int written;
    if (xdg && *xdg) {
        written = snprintf(out, n, "%s/krunknative", xdg);
    } else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home) home = ".";
        written = snprintf(out, n, "%s/.config/krunknative", home);
    }
    return written > 0 && (size_t) written < n;
}

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0700);
#endif
}

static int file_read(const char *key, char *buf, const size_t n) {
    char dir[512], path[600];
    if (!file_dir(dir, sizeof(dir))) return -1;
    snprintf(path, sizeof(path), "%s/%s.json", dir, key);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    if (buf && n) {
        const size_t want = (size_t) size < n - 1 ? (size_t) size : n - 1;
        buf[fread(buf, 1, want, f)] = 0;
    }
    fclose(f);
    return (int) size;
}

static void file_write(const char *key, const char *text) {
    char dir[512], path[600];
    if (!file_dir(dir, sizeof(dir))) return;
    snprintf(path, sizeof(path), "%s/%s.json", dir, key);
    if (!text) {
        remove(path);
        return;
    }
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        make_dir(parent); // ~/.config
    }
    make_dir(dir);
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        server_log_error("[storage] cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    const size_t len = strlen(text);
    const int ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0 || !ok) {
        remove(tmp);
        return;
    }
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0) remove(tmp);
}

static int io_read(const char *key, char *buf, const size_t n) {
    return g_st.read ? g_st.read(key, buf, n) : file_read(key, buf, n);
}

static void io_write(const char *key, const char *text) {
    if (g_st.write) g_st.write(key, text);
    else file_write(key, text);
}

static void doc_flush(KsStorageDoc *d) {
    if (!d->doc || !d->dirty) return;
    char *text = cJSON_PrintUnformatted(d->doc);
    if (text) io_write(d->key, text);
    free(text);
    d->dirty = 0;
}

static void docs_flush(void) {
    for (int i = 0; i < KS_STORAGE_DOCS; i++) doc_flush(&g_st.docs[i]);
}

// The document of a map (read from the store on first use; a new one when absent or unreadable).
static KsStorageDoc *doc_get(const char *map) {
    char key[40];
    ks_storage_offline_key(map, key, sizeof(key));
    KsStorageDoc *slot = NULL;
    for (int i = 0; i < KS_STORAGE_DOCS; i++) {
        KsStorageDoc *d = &g_st.docs[i];
        if (d->doc && !strcmp(d->key, key) && !strcmp(d->map, map)) {
            d->age = ++g_st.doc_clock;
            return d;
        }
        if (!slot || !d->doc || (slot->doc && d->age < slot->age)) slot = d;
    }
    doc_flush(slot);
    cJSON_Delete(slot->doc);
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->key, sizeof(slot->key), "%s", key);
    snprintf(slot->map, sizeof(slot->map), "%s", map);
    slot->age = ++g_st.doc_clock;
    const int size = io_read(key, NULL, 0);
    if (size > 0) {
        char *text = malloc((size_t) size + 1);
        if (text && io_read(key, text, (size_t) size + 1) >= 0) slot->doc = cJSON_Parse(text);
        free(text);
    }
    // a hash collision with another map's document starts over (it keeps its own file)
    const cJSON *doc_map = slot->doc ? cJSON_GetObjectItem(slot->doc, "map") : NULL;
    if (!cJSON_IsObject(slot->doc) || !cJSON_IsString(doc_map) || strcmp(doc_map->valuestring, map) != 0 ||
        !cJSON_IsObject(cJSON_GetObjectItem(slot->doc, "accounts"))) {
        if (slot->doc) server_log_error("[storage] %s: unreadable offline storage, starting empty", key);
        cJSON_Delete(slot->doc);
        slot->doc = cJSON_CreateObject();
        cJSON_AddStringToObject(slot->doc, "map", map);
        cJSON_AddObjectToObject(slot->doc, "accounts");
    }
    return slot;
}

// The rows of an account in a document (account names are case-insensitive, as findByName)
static cJSON *doc_account(KsStorageDoc *d, const char *account, const int create) {
    cJSON *accounts = cJSON_GetObjectItem(d->doc, "accounts");
    char lower[64];
    size_t i = 0;
    for (; account[i] && i + 1 < sizeof(lower); i++) lower[i] = (char) (account[i] >= 'A' && account[i] <= 'Z' ? account[i] + 32 : account[i]);
    lower[i] = 0;
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(accounts, lower);
    if (!rows && create) rows = cJSON_AddObjectToObject(accounts, lower);
    return cJSON_IsObject(rows) ? rows : NULL;
}

static void offline_update(KsStorageReq *req, const char *map, const char *account, const KsStorageData *data, const int type, const int scope) {
    KsStorageDoc *d = doc_get(map);
    cJSON *rows = doc_account(d, account, 1);
    if (!rows) {
        req_fail(req, "Error");
        return;
    }
    for (int i = 0; i < data->count; i++) {
        const KsStoragePair *pair = &data->pairs[i];
        cJSON *row = cJSON_GetObjectItemCaseSensitive(rows, pair->key);
        const cJSON *old = row ? cJSON_GetObjectItem(row, "v") : NULL;
        char value[KS_STORAGE_VALUE_BYTES + 32];
        const int verdict = ks_storage_apply(type, cJSON_IsString(old) ? old->valuestring : NULL, pair->value, value, sizeof(value));
        if (verdict < 0) {
            cJSON_DeleteItemFromObjectCaseSensitive(rows, pair->key);
            d->dirty = 1;
            continue;
        }
        if (verdict > 0) {
            cJSON *fresh = cJSON_CreateObject();
            cJSON_AddStringToObject(fresh, "v", value);
            cJSON_AddNumberToObject(fresh, "g", scope == KS_STORAGE_GLOBAL);
            if (row) cJSON_ReplaceItemInObjectCaseSensitive(rows, pair->key, fresh);
            else cJSON_AddItemToObject(rows, pair->key, fresh);
            row = fresh;
            d->dirty = 1;
        }
        const cJSON *now = row ? cJSON_GetObjectItem(row, "v") : NULL;
        if (cJSON_IsString(now)) req_add_row(req, pair->key, now->valuestring);
    }
    req->ready = 1;
}

static void offline_load(KsStorageReq *req, const char *this_map, const char *account, const char *from_map) {
    const int other = from_map && from_map[0] && strcmp(from_map, this_map) != 0;
    KsStorageDoc *d = doc_get(other ? from_map : this_map);
    const cJSON *rows = doc_account(d, account, 0);
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const cJSON *v = cJSON_GetObjectItem(row, "v"), *g = cJSON_GetObjectItem(row, "g");
        // another map: its global rows only (offline every map has the same creator, the player)
        if (!cJSON_IsString(v) || (other && !(cJSON_IsNumber(g) && g->valuedouble == 1))) continue;
        req_add_row(req, row->string, v->valuestring);
    }
    req_sort_rows(req);
    req->ready = 1;
}

// ------------------------------------------------------------------ brain backend

void ks_storage_set_brain_sink(void (*sink)(cJSON *frame)) {
    g_st.sink = sink;
}

static void brain_request(KsStorageReq *req, const char *op, cJSON *args) {
    cJSON *frame = cJSON_CreateObject();
    static uint32_t sink_rid = 1000000;
    req->rid = g_st.sink ? ++sink_rid : brain_next_rid();
    req->deadline = (g_server ? g_server->now : 0.0f) + KS_STORAGE_TIMEOUT;
    cJSON_AddStringToObject(frame, "type", "acct");
    cJSON_AddNumberToObject(frame, "rid", req->rid);
    cJSON_AddStringToObject(frame, "op", op);
    cJSON_AddItemToObject(frame, "args", args);
    if (g_st.sink) {
        g_st.sink(frame);
        return;
    }
    if (!brain_linked()) {
        cJSON_Delete(frame);
        req_fail(req, "Error");
        return;
    }
    if (brain_post(frame, 0) != 0) req_fail(req, "Error"); // brain_post consumed the frame
}

int ks_storage_on_brain_frame(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    if (!cJSON_IsNumber(rid) || rid->valuedouble <= 0) return 0;
    KsStorageReq *req = g_st.reqs;
    while (req && !(req->rid && req->rid == (uint32_t) rid->valuedouble)) req = req->next;
    if (!req) return 0;
    req->rid = 0;
    req->ready = 1;
    const cJSON *err = cJSON_GetObjectItem(frame, "err");
    const cJSON *data = cJSON_GetObjectItem(frame, "data");
    if (cJSON_IsString(err) && err->valuestring[0]) {
        req_fail(req, err->valuestring);
        return 1;
    }
    if (req->kind == REQ_REWARD) {
        const cJSON *rsp = cJSON_GetObjectItem(data, "rsp");
        const cJSON *funds = cJSON_GetObjectItem(data, "funds");
        if (!cJSON_IsString(rsp) || strcmp(rsp->valuestring, "Success") != 0) req_fail(req, cJSON_IsString(rsp) ? rsp->valuestring : "Error occured.");
        else req->reward_funds = cJSON_IsNumber(funds) ? (int) funds->valuedouble : -1;
        return 1;
    }
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(data, "rows")) {
        const cJSON *k = cJSON_GetArrayItem(row, 0), *v = cJSON_GetArrayItem(row, 1);
        if (cJSON_IsString(k) && cJSON_IsString(v)) req_add_row(req, k->valuestring, v->valuestring);
    }
    if (req->kind == REQ_LOAD) req_sort_rows(req);
    return 1;
}

void ks_storage_link_down(void) {
    for (KsStorageReq *req = g_st.reqs; req; req = req->next)
        if (req->rid) req_fail(req, "Error");
}

static cJSON *pairs_json(const KsStorageData *data) {
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < data->count; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(data->pairs[i].key));
        cJSON_AddItemToArray(pair, cJSON_CreateString(data->pairs[i].value));
        cJSON_AddItemToArray(list, pair);
    }
    return list;
}

// ------------------------------------------------------------------ requests

const char *ks_storage_update(const char *account_name, const KsStorageData *data, const int type, const int scope, KsStorageDone done, void *user) {
    const int be = backend();
    const char *err = NULL;
    const char *map = storage_map(be, &err);
    if (err) return err;
    if (!data) return "Missing storage data";
    const char *data_err = ks_storage_data_error(data);
    if (data_err) return data_err;
    if (type < 0 || type >= KS_STORAGE_TYPE_COUNT) return "Invalid storage type";
    const ClientConnection *client = client_by_account(account_name);
    if (!client) return "Player not connected";
    KsStorageReq *req = req_new(REQ_STORAGE, done, user);
    if (!req) return "Error";
    if (be == BACKEND_OFFLINE) {
        offline_update(req, map, KS_STORAGE_OFFLINE_ACCOUNT, data, type, scope);
        return NULL;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "id", client->acct.account_id);
    cJSON_AddStringToObject(args, "name", account_name);
    cJSON_AddStringToObject(args, "map", map);
    cJSON_AddNumberToObject(args, "type", type);
    cJSON_AddNumberToObject(args, "scope", scope == KS_STORAGE_GLOBAL);
    cJSON_AddItemToObject(args, "data", pairs_json(data));
    brain_request(req, "storage", args);
    return NULL;
}

const char *ks_storage_load(const char *account_name, const char *map_name, KsStorageDone done, void *user) {
    const int be = backend();
    const char *err = NULL;
    const char *map = storage_map(be, &err);
    if (err) return err;
    KsStorageReq *req = req_new(REQ_LOAD, done, user);
    if (!req) return "Error";
    if (be == BACKEND_OFFLINE) {
        offline_load(req, map, account_name ? account_name : "", map_name);
        return NULL;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", account_name ? account_name : "");
    cJSON_AddStringToObject(args, "map", map);
    if (map_name && map_name[0]) cJSON_AddStringToObject(args, "from", map_name);
    brain_request(req, "storage_load", args);
    return NULL;
}

void ks_storage_reward_kr(const Player *player, const double amount) {
    // v7: UTILS.isNumber(amount), Math.abs, > 0. KR are whole: the amount is floored [KN].
    if (!isfinite(amount) || backend() != BACKEND_BRAIN) return;
    if (!g_st.sink && !accounts_kr_rewards()) return;
    const int kr = (int) floor(fabs(amount) < 1e9 ? fabs(amount) : 1e9);
    ClientConnection *client = client_of_player(player);
    const char *map = g_st.sink ? current_map_name() : accounts_published_map();
    if (kr <= 0 || !client || !map || client->acct.account_id <= 0 || client->acct.hack || reward_waiting(client->id)) return;
    if (g_st.waiting_count >= (int) (sizeof(g_st.waiting_rewards) / sizeof(g_st.waiting_rewards[0]))) return;
    KsStorageReq *req = req_new(REQ_REWARD, NULL, NULL);
    if (!req) return;
    req->reward_conn = client->id;
    req->reward_amount = kr;
    g_st.waiting_rewards[g_st.waiting_count++] = client->id; // player.waitRew
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "id", client->acct.account_id);
    cJSON_AddNumberToObject(args, "amount", kr);
    cJSON_AddStringToObject(args, "map", map);
    brain_request(req, "map_reward", args);
}

static void reward_done(const KsStorageReq *req) {
    reward_unwait(req->reward_conn);
    ClientConnection *client = client_by_id(req->reward_conn);
    if (req->has_error) {
        server_log("[storage] KR reward of %d for connection %d refused: %s", req->reward_amount, req->reward_conn, req->error);
        return;
    }
    if (!client) return;
    if (req->reward_funds >= 0) client->acct.funds = req->reward_funds;
    char text[64];
    snprintf(text, sizeof(text), "Rewarded %d KR", req->reward_amount); // "ch" -1, null, text, 2
    NetChatPacket chat;
    net_packet_chat(&chat, -1, NET_CHAT_SERVER, NULL, text);
    game_send(&g_server->game, client->id, NET_PACKET_CHAT, &chat, sizeof(chat)); // peer gates of net_game_send
    NetPopupPacket popup; // "am" ["Reward", "N KR", 0, 1]
    memset(&popup, 0, sizeof(popup));
    snprintf(popup.text, sizeof(popup.text), "Reward %d KR", req->reward_amount);
    game_send(&g_server->game, client->id, NET_PACKET_POPUP, &popup, sizeof(popup));
}

void ks_storage_poll(void) {
    const float now = g_server ? g_server->now : 0.0f;
    for (KsStorageReq *req = g_st.reqs; req; req = req->next)
        if (req->rid && now >= req->deadline) req_fail(req, "Error");
    // deliver in order; a callback may queue new requests (appended, delivered next time)
    KsStorageReq **link = &g_st.reqs;
    while (*link) {
        KsStorageReq *req = *link;
        if (!req->ready) {
            link = &req->next;
            continue;
        }
        *link = req->next;
        if (req->kind == REQ_REWARD) reward_done(req);
        else if (req->done) req->done(req->user, req->rows, req->count, req->has_error ? req->error : NULL);
        req_free(req);
    }
    if (now >= g_st.next_flush) {
        g_st.next_flush = now + KS_STORAGE_FLUSH_INTERVAL;
        docs_flush();
    }
}

void ks_storage_shutdown(void) {
    docs_flush();
    while (g_st.reqs) {
        KsStorageReq *req = g_st.reqs;
        g_st.reqs = req->next;
        if (req->kind != REQ_REWARD && req->done) req->done(req->user, NULL, 0, req->kind == REQ_LOAD ? "No data found" : "Error");
        req_free(req);
    }
    for (int i = 0; i < KS_STORAGE_DOCS; i++) cJSON_Delete(g_st.docs[i].doc);
    memset(g_st.docs, 0, sizeof(g_st.docs));
    g_st.waiting_count = 0;
    g_st.next_flush = 0.0f;
}

// ------------------------------------------------------------------ map triggers (GameHooks)

// Save Data (49): storageUpdt(player.account.name, null, type, "private", function () {}, raw)
static void trigger_save(Game *game, const Player *player, const char *key, const int type, const char *value) {
    (void) game;
    char account[64];
    if (!ks_storage_account_of(client_of_player(player), account, sizeof(account))) return;
    KsStorageData data;
    ks_storage_data_init(&data);
    ks_storage_data_add(&data, key, value);
    ks_storage_update(account, &data, type, KS_STORAGE_PRIVATE, NULL, NULL); // errors are ignored, as v7's empty callback
}

// Load Data (57): the rows go into the custVals of the connection's player when they arrive
static void trigger_load_done(void *user, const KsStoragePair *rows, const int count, const char *error) {
    ClientConnection *client = client_by_id((int) (intptr_t) user);
    if (error || !count || !client || !client->player) return;
    const char **keys = malloc((size_t) count * sizeof(*keys));
    const char **values = malloc((size_t) count * sizeof(*values));
    if (keys && values) {
        for (int i = 0; i < count; i++) {
            keys[i] = rows[i].key;
            values[i] = rows[i].value;
        }
        trigger_load_data_apply(&g_server->game, client->player, keys, values, count);
    }
    free(keys);
    free(values);
}

static void trigger_load(Game *game, const Player *player) {
    (void) game;
    char account[64];
    const ClientConnection *client = client_of_player(player);
    if (!ks_storage_account_of(client, account, sizeof(account))) return;
    ks_storage_load(account, NULL, trigger_load_done, (void *) (intptr_t) client->id);
}

static void trigger_reward(Game *game, const Player *player, const int amount) {
    (void) game;
    ks_storage_reward_kr(player, amount);
}

void ks_storage_install_game_hooks(Game *game) {
    game->hooks.storage_save = trigger_save;
    game->hooks.storage_load = trigger_load;
    game->hooks.reward_kr = trigger_reward;
}
