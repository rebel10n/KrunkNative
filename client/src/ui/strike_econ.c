// Krunker Strike's economy on the client (client/include/strike_econ.h): the account's Strike state
// from kn_brain's /strike gateway (services/brain/strike_econ.c, over v10_gapi), the requests of the
// Strike screens and the loadout the game wears. Inert unless flavor_get() == FLAVOR_STRIKE.
#include <strike_econ.h>

#include <backend_ui.h>
#include <flavor.h>
#include <v10_social.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQ_MAX 32
#define LISTEN_MAX 8

static StrikeAccount g_acc;
static cJSON *g_items;    // /players/me/items
static cJSON *g_loadout;  // {slot: itemId}
static cJSON *g_claims;   // [slotId]
static cJSON *g_classes;  // [{classId, itemId}]
static int g_seen_account = -1; // account_get()'s id at the last tick (-1: never looked)
static int g_refreshing;

static struct {
    StrikeEconListener fn;
    void *user;
} g_listeners[LISTEN_MAX];

typedef enum { K_PASS, K_ME, K_ITEMS, K_STATE } ReqKind; // K_STATE: refresh players/me + items after success

typedef struct {
    int used;
    ReqKind kind;
    StrikeEconFn fn;
    void *user;
    int account_id; // the answer belongs to this account (a sign-out in between drops it)
} Req;
static Req g_reqs[REQ_MAX];

static int strike_on(void) { return flavor_get() == FLAVOR_STRIKE; }

static int signed_in(void) {
    const AccountInfo *a = account_get();
    return a && a->id > 0;
}

static void notify(StrikeEconEvent ev) {
    for (int i = 0; i < LISTEN_MAX; i++)
        if (g_listeners[i].fn) g_listeners[i].fn(g_listeners[i].user, ev);
}

int strike_econ_listen(StrikeEconListener fn, void *user) {
    for (int i = 0; i < LISTEN_MAX && fn; i++) {
        if (g_listeners[i].fn) continue;
        g_listeners[i].fn = fn;
        g_listeners[i].user = user;
        return 1;
    }
    return 0;
}

void strike_econ_unlisten(StrikeEconListener fn, void *user) {
    for (int i = 0; i < LISTEN_MAX; i++)
        if (g_listeners[i].fn == fn && g_listeners[i].user == user) g_listeners[i].fn = NULL;
}

static void on_answer(void *user, int status, const cJSON *data, const cJSON *error);

static void clear_state(void) {
    memset(&g_acc, 0, sizeof g_acc);
    cJSON_Delete(g_items);
    cJSON_Delete(g_loadout);
    cJSON_Delete(g_claims);
    cJSON_Delete(g_classes);
    g_items = g_loadout = g_claims = g_classes = NULL;
    for (int i = 0; i < REQ_MAX; i++) { // answers for the old account are dropped
        if (g_reqs[i].used) v10_gapi_cancel(on_answer, &g_reqs[i]);
        g_reqs[i].used = 0;
    }
    g_refreshing = 0;
}

// ---------------------------------------------------------------- state from the answers

static int64_t num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

static void set_wallet(const cJSON *wallet) {
    if (!cJSON_IsObject(wallet)) return;
    for (int c = 0; c < STRIKE_CUR_COUNT; c++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(wallet, strike_currency_key((StrikeCurrency) c));
        if (cJSON_IsNumber(v)) g_acc.wallet[c] = (int64_t) v->valuedouble;
    }
}

static void replace(cJSON **slot, const cJSON *v) {
    if (!v) return;
    cJSON_Delete(*slot);
    *slot = cJSON_Duplicate(v, 1);
}

static void apply_me(const cJSON *d) {
    set_wallet(cJSON_GetObjectItemCaseSensitive(d, "wallet"));
    g_acc.xp = num(d, "xp");
    g_acc.wallet[STRIKE_CUR_XP] = g_acc.xp;
    g_acc.level = strike_level_for_xp(g_acc.xp);
    g_acc.max_level = (int) num(d, "maxLevelCap");
    g_acc.golden_spin_count = (int) num(d, "goldenSpinCount");
    g_acc.spins_to_golden = (int) num(d, "spinsToGetGoldenSpin");
    g_acc.missions_unlocked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "missionsUnlocked"));
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(d, "stats");
    g_acc.kills = num(st, "kills");
    g_acc.deaths = num(st, "deaths");
    g_acc.wins = num(st, "wins");
    g_acc.games = num(st, "games");
    g_acc.score = num(st, "score");
    g_acc.time_ms = num(st, "timePlayed");
    replace(&g_loadout, cJSON_GetObjectItemCaseSensitive(d, "loadout"));
    replace(&g_claims, cJSON_GetObjectItemCaseSensitive(d, "progressionClaimedItems"));
    replace(&g_classes, cJSON_GetObjectItemCaseSensitive(d, "classes"));
    g_acc.loaded = 1;
}

// what a successful write answered: wallet / loadout / claims, and the granted rewards
static void apply_write(const cJSON *d) {
    set_wallet(cJSON_GetObjectItemCaseSensitive(d, "wallet"));
    replace(&g_loadout, cJSON_GetObjectItemCaseSensitive(d, "loadout"));
    replace(&g_claims, cJSON_GetObjectItemCaseSensitive(d, "progressionClaimedItems"));
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(d, "goldenSpinCount"))) g_acc.golden_spin_count = (int) num(d, "goldenSpinCount");
}

// ---------------------------------------------------------------- requests

static int send_req(const char *method, const char *path, const cJSON *body, ReqKind kind, StrikeEconFn fn, void *user) {
    if (!strike_on() || !path) return 0;
    int slot = -1;
    for (int i = 0; i < REQ_MAX && slot < 0; i++)
        if (!g_reqs[i].used) slot = i;
    if (slot < 0) return 0;
    char full[512];
    snprintf(full, sizeof full, "/strike%s", path);
    Req *r = &g_reqs[slot];
    *r = (Req) {.used = 1, .kind = kind, .fn = fn, .user = user, .account_id = account_get() ? account_get()->id : 0};
    if (!v10_gapi(method, full, body, on_answer, r)) {
        r->used = 0;
        return 0;
    }
    return 1;
}

static void refresh_internal(void) {
    if (g_refreshing || !signed_in()) return;
    g_refreshing = send_req("GET", "/v1/players/me", NULL, K_ME, NULL, NULL);
    send_req("GET", "/v1/players/me/items", NULL, K_ITEMS, NULL, NULL);
}

static void on_answer(void *user, int status, const cJSON *data, const cJSON *error) {
    Req *r = user;
    if (!r->used) return; // dropped (sign-out / cancel)
    const Req q = *r;
    r->used = 0;
    const int same = account_get() && account_get()->id == q.account_id;
    if (same && status == 200) {
        switch (q.kind) {
        case K_ME:
            g_refreshing = 0;
            g_acc.account_id = q.account_id;
            apply_me(data);
            printf("[strike] account %d: level %d, %lld KR, %lld spin tickets, %lld golden\n", q.account_id, g_acc.level.level,
                   (long long) g_acc.wallet[STRIKE_CUR_KR], (long long) g_acc.wallet[STRIKE_CUR_SPIN_TICKET],
                   (long long) g_acc.wallet[STRIKE_CUR_GOLDEN_TICKET]);
            break;
        case K_ITEMS:
            replace(&g_items, data);
            break;
        case K_STATE:
            apply_write(data);
            refresh_internal(); // XP / items / meter from the brain
            break;
        default:
            break;
        }
        if (q.kind != K_PASS) notify(STRIKE_ECON_CHANGED);
    } else if (q.kind == K_ME) {
        g_refreshing = 0;
    }
    if (status != 200 && q.kind != K_ME && q.kind != K_ITEMS) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
        printf("[strike] request refused (%d): %s\n", status, msg ? msg : "no answer");
    }
    if (q.fn) q.fn(q.user, status, data, error);
}

void strike_econ_cancel(StrikeEconFn fn, void *user) {
    for (int i = 0; i < REQ_MAX; i++)
        if (g_reqs[i].used && g_reqs[i].fn == fn && g_reqs[i].user == user) g_reqs[i].fn = NULL;
}

int strike_econ_request(const char *method, const char *path, const cJSON *body, StrikeEconFn fn, void *user) {
    return send_req(method, path, body, K_PASS, fn, user);
}

int strike_econ_refresh(StrikeEconFn fn, void *user) {
    if (!signed_in()) return 0;
    send_req("GET", "/v1/players/me/items", NULL, K_ITEMS, NULL, NULL);
    return send_req("GET", "/v1/players/me", NULL, K_ME, fn, user);
}

int strike_econ_spin(const char *wheel_id, int count, int golden, StrikeEconFn fn, void *user) {
    if (!wheel_id || !signed_in()) return 0;
    char path[160];
    snprintf(path, sizeof path, "/v2/spin-wheels/%s/spin", wheel_id);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "count", count > 0 ? count : 1);
    cJSON_AddBoolToObject(b, "golden", golden != 0);
    const int sent = send_req("POST", path, b, K_STATE, fn, user);
    cJSON_Delete(b);
    return sent;
}

int strike_econ_buy(const char *product_id, int quantity, StrikeEconFn fn, void *user) {
    if (!product_id || !signed_in()) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "productId", product_id);
    char price[160];
    snprintf(price, sizeof price, "%s_p0", product_id);
    cJSON_AddStringToObject(b, "priceId", price);
    cJSON_AddNumberToObject(b, "quantity", quantity > 0 ? quantity : 1);
    const int sent = send_req("POST", "/v1/shop/purchase", b, K_STATE, fn, user);
    cJSON_Delete(b);
    return sent;
}

int strike_econ_equip(const char *item_id, StrikeEconFn fn, void *user) {
    if (!item_id || !signed_in()) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "itemId", item_id);
    const int sent = send_req("POST", "/v1/players/me/equip", b, K_STATE, fn, user);
    cJSON_Delete(b);
    return sent;
}

int strike_econ_unequip(const char *slot, StrikeEconFn fn, void *user) {
    if (!slot || !signed_in()) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "slot", slot);
    cJSON_AddNullToObject(b, "itemId");
    const int sent = send_req("POST", "/v1/players/me/equip", b, K_STATE, fn, user);
    cJSON_Delete(b);
    return sent;
}

int strike_econ_claim_level(const char *slot_id, int choice, StrikeEconFn fn, void *user) {
    if (!slot_id || !signed_in()) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "slotId", slot_id);
    if (choice >= 0) cJSON_AddNumberToObject(b, "choice", choice);
    const int sent = send_req("POST", "/v1/players/me/claim/progression", b, K_STATE, fn, user);
    cJSON_Delete(b);
    return sent;
}

int strike_econ_claim_mission(const char *mission_id, StrikeEconFn fn, void *user) {
    if (!mission_id || !signed_in()) return 0;
    char path[160];
    snprintf(path, sizeof path, "/v1/missions/claim/%s", mission_id);
    return send_req("POST", path, NULL, K_STATE, fn, user);
}

int strike_econ_get_shop(StrikeEconFn fn, void *user) { return send_req("GET", "/v1/shop", NULL, K_PASS, fn, user); }
int strike_econ_get_missions(StrikeEconFn fn, void *user) { return send_req("GET", "/v1/missions/me", NULL, K_PASS, fn, user); }
int strike_econ_get_game_config(StrikeEconFn fn, void *user) { return send_req("GET", "/v1/game-config", NULL, K_PASS, fn, user); }

int strike_econ_get_wheels(int with_items, StrikeEconFn fn, void *user) {
    if (!with_items) return send_req("GET", "/v2/spin-wheels/list", NULL, K_PASS, fn, user);
    cJSON *b = cJSON_CreateObject();
    const int sent = send_req("POST", "/v2/spin-wheels/details", b, K_PASS, fn, user);
    cJSON_Delete(b);
    return sent;
}

int strike_econ_get_leaderboard(const char *board, int limit, int offset, StrikeEconFn fn, void *user) {
    char path[160];
    snprintf(path, sizeof path, "/v1/leaderboards/%s?limit=%d&offset=%d", board ? board : "xp", limit > 0 ? limit : 50, offset > 0 ? offset : 0);
    return send_req("GET", path, NULL, K_PASS, fn, user);
}

// ---------------------------------------------------------------- state readers

const StrikeAccount *strike_econ_account(void) { return &g_acc; }
const cJSON *strike_econ_items(void) { return g_items; }

int strike_econ_item_count(const char *item_id) {
    if (!item_id) return 0;
    const StrikeItem *def = strike_item(item_id);
    if (def && def->is_default) return 1;
    const cJSON *it;
    cJSON_ArrayForEach(it, g_items) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "itemId"));
        if (id && !strcmp(id, item_id)) return (int) num(it, "count");
    }
    return 0;
}

int strike_econ_owns(const char *item_id) { return strike_econ_item_count(item_id) > 0; }

const char *strike_econ_equipped(const char *slot) {
    const char *id = slot ? cJSON_GetStringValue(cJSON_GetObjectItem(g_loadout, slot)) : NULL;
    return id && *id ? id : NULL;
}

// the store index an item wears: its definition from the brain (items answer), else the catalogue's
static int item_skin(const char *item_id) {
    const cJSON *it;
    cJSON_ArrayForEach(it, g_items) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "itemId"));
        if (!id || strcmp(id, item_id)) continue;
        const cJSON *skin = cJSON_GetObjectItem(cJSON_GetObjectItem(it, "item"), "id");
        return cJSON_IsNumber(skin) && (int) num(it, "count") > 0 ? skin->valueint : -1;
    }
    return -1; // not owned (or the items list not here yet): the plain look
}

int strike_econ_slot_skin(const char *slot) {
    const char *id = strike_econ_equipped(slot);
    return id ? item_skin(id) : -1;
}

int strike_econ_class_owned(int class_id) {
    const StrikeEconomy *e = strike_economy();
    for (int i = 0; i < e->starter_class_count; i++)
        if (e->starter_classes[i] == class_id) return 1;
    const cJSON *c;
    cJSON_ArrayForEach(c, g_classes) if (num(c, "classId") == class_id) return 1;
    return 0;
}

int strike_econ_track_claimed(const char *slot_id) {
    const cJSON *c;
    cJSON_ArrayForEach(c, g_claims) if (cJSON_IsString(c) && slot_id && !strcmp(c->valuestring, slot_id)) return 1;
    return 0;
}

int strike_econ_track_claimable(const char **out, int max) {
    const StrikeEconomy *e = strike_economy();
    int n = 0;
    if (!g_acc.loaded) return 0;
    for (int i = 0; i < e->track_count && n < max; i++)
        if (e->track[i].level <= g_acc.level.level && !strike_econ_track_claimed(e->track[i].slot_id)) out[n++] = e->track[i].slot_id;
    return n;
}

// ---------------------------------------------------------------- engine hooks

void strike_econ_tick(void) {
    if (!strike_on()) return;
    const AccountInfo *a = account_get();
    const int id = a ? a->id : 0;
    if (id == g_seen_account) return;
    const int had = g_seen_account > 0;
    g_seen_account = id;
    clear_state();
    if (had) notify(STRIKE_ECON_SIGNED_OUT);
    if (id > 0) refresh_internal();
}

int strike_econ_apply_look(int primary_weapon, int secondary_weapon, PlayerSkins *skins, PlayerCosmetics *cosmetics,
                           PlayerCosmeticsV7 *v7) {
    if (!strike_on()) return 0;
    char slot[16];
    if (skins) {
        snprintf(slot, sizeof slot, "w%d", primary_weapon);
        skins->primary = primary_weapon >= 0 ? strike_econ_slot_skin(slot) : -1;
        snprintf(slot, sizeof slot, "w%d", secondary_weapon);
        skins->secondary = secondary_weapon >= 0 ? strike_econ_slot_skin(slot) : -1;
        skins->melee = strike_econ_slot_skin("t3");
    }
    if (cosmetics) {
        cosmetics->hat = strike_econ_slot_skin("t1");
        cosmetics->body = strike_econ_slot_skin("t2");
        cosmetics->dye = strike_econ_slot_skin("t5");
        cosmetics->waist = strike_econ_slot_skin("t6");
        cosmetics->face = strike_econ_slot_skin("t7");
    }
    if (v7) {
        v7->head = v7->back = v7->player_card = -1;
        v7->shoe = strike_econ_slot_skin("t8");
        v7->pet = strike_econ_slot_skin("t9");
        v7->wrist = strike_econ_slot_skin("t11");
        v7->charms[0] = strike_econ_slot_skin("t12");
        v7->charms[1] = -1;
    }
    return 1;
}

int strike_econ_spray(void) { return strike_on() ? strike_econ_slot_skin("t4") : -1; }

// ---------------------------------------------------------------- page handlers

static const char *arg_str(const HtmlArg *args, int argc, int i, char *buf, size_t n) {
    if (i >= argc) return NULL;
    if (args[i].kind == HTML_ARG_STR) return args[i].str;
    if (args[i].kind == HTML_ARG_NUM) {
        snprintf(buf, n, "%.0f", args[i].num);
        return buf;
    }
    return NULL;
}

static int arg_int(const HtmlArg *args, int argc, int i, int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_BOOL) return args[i].num != 0;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

static void logged(void *user, int status, const cJSON *data, const cJSON *error) {
    const char *what = user;
    if (status != 200) return; // on_answer logged the refusal
    if (!strcmp(what, "spin")) {
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(data, "rewards")) {
            const cJSON *item = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "reward"), "item");
            printf("[strike] spin won %s (%s) from %s\n", cJSON_GetStringValue(cJSON_GetObjectItem(item, "itemId")),
                   cJSON_GetStringValue(cJSON_GetObjectItem(item, "name")), cJSON_GetStringValue(cJSON_GetObjectItem(r, "poolId")));
        }
    } else if (!strcmp(what, "equip")) {
        char *text = cJSON_PrintUnformatted(cJSON_GetObjectItem(data, "loadout"));
        printf("[strike] loadout %s\n", text ? text : "{}");
        free(text);
    } else {
        printf("[strike] %s ok\n", what);
    }
    (void) error;
}

int strike_econ_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc) {
    (void) ev;
    if (!name || strncmp(name, "knStrike", 8) != 0) return -1;
    char b0[32];
    const char *a0 = arg_str(args, argc, 0, b0, sizeof b0);
    if (!strcmp(name, "knStrikeRefresh")) strike_econ_refresh(logged, "refresh");
    else if (!strcmp(name, "knStrikeEquip")) strike_econ_equip(a0, logged, "equip");
    else if (!strcmp(name, "knStrikeUnequip")) strike_econ_unequip(a0, logged, "equip");
    else if (!strcmp(name, "knStrikeSpin")) strike_econ_spin(a0, arg_int(args, argc, 1, 1), arg_int(args, argc, 2, 0), logged, "spin");
    else if (!strcmp(name, "knStrikeBuy")) strike_econ_buy(a0, arg_int(args, argc, 1, 1), logged, "buy");
    else if (!strcmp(name, "knStrikeClaimLevel")) strike_econ_claim_level(a0, arg_int(args, argc, 1, -1), logged, "claim");
    else if (!strcmp(name, "knStrikeClaimMission")) strike_econ_claim_mission(a0, logged, "mission");
    else return -1;
    return 0;
}
