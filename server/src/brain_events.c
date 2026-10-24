// Krunker v10.0.3 game-server events that need kn_brain (server/include/server_brain_events.h). Line numbers are
// deob-10.0.3.js (/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js). The client side of every packet here is
// client/src/net/s2c_events.c; the brain's is services/brain/node_events.c.
#include <server_brain_events.h>
#include <server_accounts.h>
#include <server_log.h>
#include <brain.h>
#include <net_ext.h>
#include <store_spins.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BE_SPINNERS_MAX 64     // map_validate's MAP_V7_WHEEL_LIMIT (10) is far below
#define BE_REQS_MAX 64
#define BE_SPIN_EASE 0.05f     // updateSpinners: spinRotation += (spinVal - spinRotation) * 0.05 per server update (:219470)
#define BE_SPIN_STOP 0.002f    // ... until spinVal - spinRotation <= 0.002
#define BE_UNBS_BROADCAST 2    // the stop's unbS: everyone for rarity >= 2, else the spinner (:219472-219476)
#define BE_SPINNER_TRADIUS 7.0f // addSpinner's tRadius: 7 (:189062)

enum { BE_REQ_NONE, BE_REQ_TURF, BE_REQ_SPIN, BE_REQ_GAME, BE_REQ_CHARGE };

typedef struct {
    uint32_t rid;              // 0 = free
    int kind;                  // BE_REQ_*
    float deadline;            // g_server->now
    unsigned round;            // g_be.round when it was asked
    int conn;                  // ClientConnection.id (spin, charge)
    int account;
    int spinner;               // BE_REQ_SPIN: the spinner's uid
    int owner;                 // BE_REQ_SPIN: the player's uid (spinOwner)
    BrainChargeDone done;      // BE_REQ_CHARGE
    void *user;
} BeReq;

// one of MapManager.spinners on the server: its spinInfo as the server's updateSpinners uses it (:219439-219479)
typedef struct {
    int object;                // map object index
    int wheel;                 // store.wheels index (0..2)
    float spin_val, spin_rotation;
    int item, rarity, owner;   // itemID, store.skins[itemID].rarity, spinOwner (the player's uid)
    unsigned char running;     // the pointer turns (unbS when it stops)
    unsigned char busy;        // a "map_spin" is in flight (KrunkNative: one spin per wheel at a time)
} BeSpinner;

static struct {
    unsigned round;
    const Map *map;            // the map the spinners were built for
    int spinner_count;
    BeSpinner spinners[BE_SPINNERS_MAX];

    int turf_map;              // this round's eligible Turf Wars map, -1 none
    int turf_asked;            // "turf_clan" sent for this round
    int clan_has;              // territorialWars.clanInfo
    int clan_id;
    char clan_name[32], clan_logo[64], clan_color[24];

    char game_map[128];        // the hosted published map "studio_game" was asked for ("" none)
    int game_asked;
    int monetized;             // its answer (customMapData.monetization)

    BeReq reqs[BE_REQS_MAX];
} g_be = {.turf_map = -1};

// ------------------------------------------------------------------ helpers

static ClientConnection *client_by_id(const int id) {
    for (size_t i = 0; i < g_server->client_count; i++)
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    return NULL;
}

// the settled connection of an account (one per account: acc_kick_duplicates)
static ClientConnection *client_by_account(const int64_t account) {
    for (size_t i = 0; account > 0 && i < g_server->client_count; i++) {
        ClientConnection *c = &g_server->clients[i];
        if (c->acct.account_id == account && c->acct.join_state == SERVER_JOIN_READY) return c;
    }
    return NULL;
}

static void send_ext(const ClientConnection *client, const uint16_t subtype, const void *body, const uint16_t length) {
    if (!client || client->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    uint8_t buf[NET_EXT_HEADER_SIZE + 256];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
}

static void broadcast_ext(const uint16_t subtype, const void *body, const uint16_t length) {
    for (size_t i = 0; i < g_server->client_count; i++) send_ext(&g_server->clients[i], subtype, body, length);
}

static BeReq *req_new(const int kind) {
    for (int i = 0; i < BE_REQS_MAX; i++) {
        if (g_be.reqs[i].rid) continue;
        memset(&g_be.reqs[i], 0, sizeof(g_be.reqs[i]));
        g_be.reqs[i].kind = kind;
        g_be.reqs[i].round = g_be.round;
        g_be.reqs[i].deadline = g_server->now + BRAIN_EVENTS_REPLY_S;
        return &g_be.reqs[i];
    }
    return NULL;
}

// {"type":"acct","rid","op","args"} (args consumed); 0 sent with r->rid set, -1 not (r freed)
static int req_post(BeReq *r, const char *op, cJSON *args) {
    const uint32_t rid = brain_linked() ? brain_next_rid() : 0;
    if (!rid) {
        cJSON_Delete(args);
        r->rid = 0;
        return -1;
    }
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "acct");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddStringToObject(frame, "op", op);
    cJSON_AddItemToObject(frame, "args", args);
    // not reliable: a spin or a charge that was sent once is never sent again (a reconnect after a lost answer must
    // not take the KR twice; the charge's key guards the brain side too)
    if (brain_post(frame, 0) != 0) {
        r->rid = 0;
        return -1;
    }
    r->rid = rid;
    return 0;
}

static BeReq *req_of(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    if (!cJSON_IsNumber(rid) || rid->valuedouble <= 0) return NULL;
    for (int i = 0; i < BE_REQS_MAX; i++)
        if (g_be.reqs[i].rid && g_be.reqs[i].rid == (uint32_t) rid->valuedouble) return &g_be.reqs[i];
    return NULL;
}

static double num_of(const cJSON *o, const char *key, const double def) {
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : def;
}

// ------------------------------------------------------------------ warsClan (Turf Wars)

static void wars_clan_packet(NetWarsClanPacket *p) {
    memset(p, 0, sizeof(*p));
    p->has = (uint8_t) g_be.clan_has;
    if (!g_be.clan_has) return;
    p->clan_id = g_be.clan_id;
    snprintf(p->clan_name, sizeof(p->clan_name), "%s", g_be.clan_name);
    snprintf(p->clan_logo, sizeof(p->clan_logo), "%s", g_be.clan_logo);
    snprintf(p->banner_color, sizeof(p->banner_color), "%s", g_be.clan_color);
}

static void turf_ask(void) {
    g_be.turf_asked = 1;
    BeReq *r = req_new(BE_REQ_TURF);
    if (!r) return;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "map", g_be.turf_map);
    if (req_post(r, "turf_clan", args) != 0) g_be.turf_asked = 0; // asked again once linked (brain_events_tick)
}

// setClanInfo(info) on the server: the clan, and WARS_CLAN [clan_id, clan_name, clan_logo, clan_banner_color] or
// [null] to everyone (:215092-215098)
static void turf_reply(const BeReq *r, const char *err, const cJSON *data) {
    if (r->round != g_be.round || g_be.turf_map < 0) return; // a reply for a round that is over
    if (err) {
        server_log_error("[turf] the clan of map %d: %s", g_be.turf_map, err);
        return;
    }
    const cJSON *name = cJSON_GetObjectItem(data, "clan_name");
    g_be.clan_has = cJSON_IsObject(data) && cJSON_IsString(name) && name->valuestring[0];
    g_be.clan_id = g_be.clan_has ? (int) num_of(data, "clan_id", 0) : 0;
    snprintf(g_be.clan_name, sizeof(g_be.clan_name), "%s", g_be.clan_has ? name->valuestring : "");
    g_be.clan_logo[0] = g_be.clan_color[0] = 0;
    const cJSON *logo = cJSON_GetObjectItem(data, "clan_logo"), *color = cJSON_GetObjectItem(data, "clan_banner_color");
    if (g_be.clan_has && cJSON_IsNumber(logo)) snprintf(g_be.clan_logo, sizeof(g_be.clan_logo), "%.0f", logo->valuedouble);
    if (g_be.clan_has && cJSON_IsString(color)) snprintf(g_be.clan_color, sizeof(g_be.clan_color), "%s", color->valuestring);
    NetWarsClanPacket p;
    wars_clan_packet(&p);
    broadcast_ext(NET_EXT_WARS_CLAN, &p, sizeof(p));
    if (g_be.clan_has) server_log("[turf] map %d is held by [%s]", g_be.turf_map, g_be.clan_name);
}

// ------------------------------------------------------------------ map prize wheels (spinW / unbS)

// parseInt(raw[key]) || 0
static int raw_int(const cJSON *raw, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(raw, key);
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? (int) v->valuedouble : 0;
    if (cJSON_IsString(v) && v->valuestring) return (int) strtol(v->valuestring, NULL, 10);
    return 0;
}

// MapManager.spinners (map generate, :189053-189066): a PRIZE_WHEEL of the map file whose store wheel
// store.wheels[limitMM(parseInt(sty) || 0, 0, 2)] exists and is not ticketOnly, hidden or locked; -1 otherwise. The
// client builds the same list (client/src/prize_wheels.c), so a spinner's uid (its place) is the same on both.
static int spinner_wheel(const Object *o) {
    if (!o || o->prefab != PREFAB_PRIZE_WHEEL || o->spawned || o->dynamic) return -1;
    int sty = raw_int(o->raw, "sty");
    sty = sty < 0 ? 0 : sty > 2 ? 2 : sty;
    const StoreSpins *spins = store_spins_builtin();
    const StoreWheel *w = sty < spins->wheel_count ? &spins->wheels[sty] : NULL;
    return !w || w->ticket_only || w->hide || w->locked ? -1 : sty;
}

static void spinners_build(const Map *map) {
    g_be.map = map;
    g_be.spinner_count = 0;
    for (size_t i = 0; map && i < map->object_count && g_be.spinner_count < BE_SPINNERS_MAX; i++) {
        const int wheel = spinner_wheel(map->objects[i]);
        if (wheel < 0) continue;
        BeSpinner *s = &g_be.spinners[g_be.spinner_count++];
        memset(s, 0, sizeof(*s));
        s->object = (int) i;
        s->wheel = wheel;
        s->item = -1;
    }
}

static BeSpinner *spinner_at(const int uid) {
    if (g_server->game.map != g_be.map) spinners_build(g_server->game.map);
    return uid >= 0 && uid < g_be.spinner_count ? &g_be.spinners[uid] : NULL;
}

// player.collides(spinner, spinner.tRadius) (:197177, :191213-191223). The spinner's GameObject is {x, z, y: the
// object's bottom, w = l = h = sy / 4} (addSpinner, :186543-186560): a box centred on the bottom.
static int spinner_reaches(const BeSpinner *s, const Player *p) {
    const Object *o = g_server->game.map->objects[s->object];
    const vec3 c = o->position;
    const float half = o->scale.y / 4.0f;
    const vec3 h = {half, half, half};
    const float pad = BE_SPINNER_TRADIUS;
    return p->position.x - p->scale < c.x + h.x + pad && p->position.x + p->scale > c.x - h.x - pad &&
           p->position.z - p->scale < c.z + h.z + pad && p->position.z + p->scale > c.z - h.z - pad &&
           p->position.y <= c.y + h.y && p->position.y + p->height >= c.y - h.y;
}

// interact 9's gate: an active spinner in reach, `player.account && account.funds >= store.wheels[wheel].price`.
// KrunkNative also waits for the wheel to stop (v10's updateSpinner restarts a turning wheel and its first spin's unbS
// never comes) and for the player's previous spin to be answered.
static int spinner_offer(const ClientConnection *client, const BeSpinner *s) {
    const StoreSpins *spins = store_spins_builtin();
    const StoreWheel *w = s && s->wheel < spins->wheel_count ? &spins->wheels[s->wheel] : NULL;
    if (!w || s->busy || s->running || !client->player || !spinner_reaches(s, client->player)) return 0;
    if (client->acct.account_id <= 0 || client->acct.join_state != SERVER_JOIN_READY || client->acct.funds < w->price) return 0;
    for (int i = 0; i < BE_REQS_MAX; i++)
        if (g_be.reqs[i].rid && g_be.reqs[i].kind == BE_REQ_SPIN && g_be.reqs[i].conn == client->id) return 0;
    return 1;
}

void brain_events_spinner_interact(ClientConnection *client, const int uid) {
    Game *game = &g_server->game;
    Player *p = client ? client->player : NULL;
    if (!p || !p->active || !game->map || game->end_timer > 0.0f) return;
    player_drain_inputs(p); // io_int: the queued inputs first (game_player_interact_v7's order)
    if (!p->active || game->now - p->last_interact < GAME_INTERACT_TIMER) return; // config.interactTimer
    p->last_interact = game->now;
    if (!brain_linked() || !spinner_offer(client, spinner_at(uid))) return;
    p->pending_spin = uid + 1; // pendingSpin = uid, pendingTrigger = null (:197178-197180)
    p->pending_trigger = 0;
    const NetConfirmPacket packet = {.uid = uid, .kind = 2};
    game_send(game, p->uid, NET_PACKET_CONFIRM_INTERACT, &packet, sizeof(packet)); // send(CONFIRM_INTERACTION, uid, 2)
}

// "cnfm" 2: the server's spin is not in the bundle. KrunkNative checks interact 9's gate again (the player may have
// walked off or spent the KR meanwhile) and spins the wheel through the brain's KR wallet ("map_spin").
void brain_events_spinner_confirm(ClientConnection *client) {
    Player *p = client ? client->player : NULL;
    if (!p || !p->active || !p->pending_spin) return;
    const int uid = p->pending_spin - 1;
    p->pending_spin = 0;
    BeSpinner *s = spinner_at(uid);
    if (!spinner_offer(client, s)) return;
    BeReq *r = req_new(BE_REQ_SPIN);
    if (!r) return;
    r->conn = client->id;
    r->account = client->acct.account_id;
    r->spinner = uid;
    r->owner = p->uid;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "id", client->acct.account_id);
    cJSON_AddNumberToObject(args, "wheel", s->wheel);
    if (req_post(r, "map_spin", args) == 0) s->busy = 1;
}

// updateSpinner(uid, randNum, itemID) (:219408-219431) and SPIN_WHEEL_RESPONSE [socketId, uid, null, randNum, itemID,
// funds] to everyone (:254991: only the spinner's own client takes the funds and the skin)
static void spin_reply(const BeReq *r, const char *err, const cJSON *data) {
    BeSpinner *s = r->round == g_be.round ? spinner_at(r->spinner) : NULL;
    if (s) s->busy = 0;
    ClientConnection *client = client_by_id(r->conn);
    if (err || !cJSON_IsObject(data)) {
        // v10's client shows nothing for a spin error (it returns on `error`, :254992): neither does this server
        server_log("[spin] wheel %d for account %d refused: %s", r->spinner, r->account, err ? err : "no answer");
        return;
    }
    const int skin = (int) num_of(data, "skin", -1);
    const double funds = num_of(data, "funds", 0);
    const float rand = (float) num_of(data, "rand", 0);
    if (client && client->acct.account_id == r->account) {
        client->acct.funds = (int) funds;
        if (skin >= 0 && skin < (int) sizeof(client->acct.skins_owned) * 8)
            client->acct.skins_owned[skin >> 3] |= (uint8_t) (1u << (skin & 7)); // account.addSkin: wearable at once
    }
    server_log("[spin] wheel %d: account %d won item %d (%.0f KR left)", r->spinner, r->account, skin, funds);
    if (!s || skin < 0) return; // the round changed meanwhile: the account has the item, the wheel is gone
    s->spin_val = (float) (M_PI * 2.0 * (rand / 100.0 + 13.0) + M_PI / 2.0);
    s->spin_rotation = 0.0f;
    s->item = skin;
    s->rarity = (int) num_of(data, "rarity", 0);
    s->owner = r->owner; // spinOwner
    s->running = 1;
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *to = &g_server->clients[i];
        const int mine = to == client;
        const NetSpinWheelPacket p = {.funds = mine ? funds : 0.0, .uid = r->spinner, .rand = rand, .skin = skin, .mine = (uint8_t) mine};
        send_ext(to, NET_EXT_SPIN_WHEEL_RESPONSE, &p, sizeof(p));
    }
}

// the server's updateSpinners (`if (iîÏiïíï)`, :222229-222231 every update): the pointer eases in and, once within
// 0.002, UNBOX_MESSSAGE_S [spinOwner, itemID, false] goes to everyone for rarity >= 2, else to the spinner only
// (:219470-219476). The ease is per server update, like v10's (server_bin's tick rate sets how long it turns).
static void spinners_step(void) {
    if (g_server->game.map != g_be.map) return;
    for (int i = 0; i < g_be.spinner_count; i++) {
        BeSpinner *s = &g_be.spinners[i];
        if (!s->running || s->spin_rotation == s->spin_val) continue;
        s->spin_rotation += (s->spin_val - s->spin_rotation) * BE_SPIN_EASE;
        if (s->spin_val - s->spin_rotation > BE_SPIN_STOP) continue;
        s->spin_rotation = s->spin_val;
        s->running = 0;
        NetUnboxMessagePacket p;
        memset(&p, 0, sizeof(p));
        p.uid = s->owner;
        p.item = s->item;
        p.kind = NET_UNBOX_PLAYER;
        if (s->rarity >= BE_UNBS_BROADCAST) {
            broadcast_ext(NET_EXT_UNBOX_MESSAGE, &p, sizeof(p));
        } else {
            for (size_t c = 0; c < g_server->client_count; c++)
                if (g_server->clients[c].player && g_server->clients[c].player->uid == s->owner) send_ext(&g_server->clients[c], NET_EXT_UNBOX_MESSAGE, &p, sizeof(p));
        }
    }
}

// ------------------------------------------------------------------ unb / lotWin (the brain's pushes)

// "unbox" {accountId, item, kr, rarity}: UNBOX_MESSAGE [name, itemIndex, kr] (:254321). KrunkNative's rule (v10's
// server is not in the bundle): the map wheel's (unbS): everyone in the game for rarity >= 2, else the unboxer only.
static void on_unbox(const cJSON *frame) {
    ClientConnection *client = client_by_account((int64_t) num_of(frame, "accountId", 0));
    const int item = (int) num_of(frame, "item", -1);
    if (!client || item < 0) return;
    NetUnboxMessagePacket p;
    memset(&p, 0, sizeof(p));
    p.uid = -1;
    p.item = item;
    p.kr = (int) num_of(frame, "kr", 0);
    p.kind = NET_UNBOX_NAMED;
    snprintf(p.name, sizeof(p.name), "%s", client->name);
    if ((int) num_of(frame, "rarity", 0) >= BE_UNBS_BROADCAST) broadcast_ext(NET_EXT_UNBOX_MESSAGE, &p, sizeof(p));
    else send_ext(client, NET_EXT_UNBOX_MESSAGE, &p, sizeof(p));
}

// "raffle-win" {accountId, lotteryId, drawId, prize {type, ref, quantity}}: LOTTERY_PRIZE_WON [lotteryId, entryId,
// prize, playerId] to everyone (:255154: each client names players.findById(playerId), so a winner without a player in
// the game writes no line)
static void on_raffle_win(const cJSON *frame) {
    const ClientConnection *client = client_by_account((int64_t) num_of(frame, "accountId", 0));
    const cJSON *prize = cJSON_GetObjectItem(frame, "prize");
    if (!client || !client->player || !cJSON_IsObject(prize)) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(prize, "type"));
    const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(prize, "ref"));
    NetLotteryPrizePacket p;
    memset(&p, 0, sizeof(p));
    p.quantity = num_of(prize, "quantity", 1);
    p.uid = client->player->uid;
    p.type = !type ? NET_PRIZE_OTHER : !strcmp(type, "skin") ? NET_PRIZE_SKIN : !strcmp(type, "currency") ? NET_PRIZE_CURRENCY
           : !strcmp(type, "subscription") ? NET_PRIZE_SUBSCRIPTION : NET_PRIZE_OTHER;
    snprintf(p.ref, sizeof(p.ref), "%s", ref ? ref : "");
    broadcast_ext(NET_EXT_LOTTERY_PRIZE_WON, &p, sizeof(p));
}

// ------------------------------------------------------------------ monetized games (chreq)

static void game_ask(void) {
    g_be.game_asked = 1;
    BeReq *r = req_new(BE_REQ_GAME);
    if (!r) return;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "map", g_be.game_map);
    if (req_post(r, "studio_game", args) != 0) g_be.game_asked = 0;
}

static void game_reply(const BeReq *r, const char *err, const cJSON *data) {
    if (r->round != g_be.round || !g_be.game_map[0]) return;
    g_be.monetized = !err && cJSON_IsTrue(cJSON_GetObjectItem(data, "monetized"));
    if (g_be.monetized) server_log("[payments] %s is a monetized studio game", g_be.game_map);
}

int brain_events_monetized(void) {
    const char *map = accounts_published_map();
    return g_be.monetized && map && !strcmp(map, g_be.game_map);
}

int brain_events_can_charge(const ClientConnection *client) {
    return client && client->acct.account_id > 0 && !client->acct.hack && client->acct.join_state == SERVER_JOIN_READY &&
           accounts_published_map() && brain_linked();
}

int brain_events_studio_charge(const ClientConnection *client, const int amount, BrainChargeDone done, void *user) {
    const char *map = accounts_published_map();
    if (!done || !brain_events_can_charge(client) || amount < 1 || !map) return -1;
    BeReq *r = req_new(BE_REQ_CHARGE);
    if (!r) return -1;
    r->conn = client->id;
    r->account = client->acct.account_id;
    r->done = done;
    r->user = user;
    char hex[24], key[48];
    brain_random_hex(hex, sizeof(hex), 8);
    snprintf(key, sizeof(key), "sc:%s", hex); // the brain takes the KR once per key
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "id", client->acct.account_id);
    cJSON_AddStringToObject(args, "map", map);
    cJSON_AddNumberToObject(args, "amount", amount);
    cJSON_AddStringToObject(args, "key", key);
    return req_post(r, "studio_charge", args);
}

// {success, krBalance}: account.funds = krBalance (and v10's UPDATE_FUNDS, which the brain's "ua" push to the menu
// stands for), then the callback (success, balance)
static void charge_reply(const BeReq *r, const char *err, const cJSON *data) {
    const int success = !err && cJSON_IsTrue(cJSON_GetObjectItem(data, "success"));
    const double balance = success ? num_of(data, "krBalance", 0) : 0.0;
    ClientConnection *client = client_by_id(r->conn);
    if (success && client && client->acct.account_id == r->account) client->acct.funds = (int) balance;
    if (err) server_log("[payments] charge for account %d refused: %s", r->account, err);
    else if (success) server_log("[payments] account %d charged %.0f KR (%.0f left)", r->account, num_of(data, "charged", 0), balance);
    if (r->done) r->done(r->user, success, balance);
}

// ------------------------------------------------------------------ lifecycle

static void req_finish(BeReq *r, const char *err, const cJSON *data) {
    const BeReq done = *r;
    r->rid = 0; // freed before the callback: it may ask again
    switch (done.kind) {
        case BE_REQ_TURF: turf_reply(&done, err, data); break;
        case BE_REQ_SPIN: spin_reply(&done, err, data); break;
        case BE_REQ_GAME: game_reply(&done, err, data); break;
        case BE_REQ_CHARGE: charge_reply(&done, err, data); break;
        default: break;
    }
}

int brain_events_on_brain_frame(const cJSON *frame) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "type"));
    if (!type) return 0;
    if (!strcmp(type, "unbox")) {
        on_unbox(frame);
        return 1;
    }
    if (!strcmp(type, "raffle-win")) {
        on_raffle_win(frame);
        return 1;
    }
    if (strcmp(type, "acct-r")) return 0;
    BeReq *r = req_of(frame);
    if (!r) return 0;
    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "err"));
    req_finish(r, err && err[0] ? err : NULL, cJSON_GetObjectItem(frame, "data"));
    return 1;
}

void brain_events_link_down(void) {
    for (int i = 0; i < BE_REQS_MAX; i++)
        if (g_be.reqs[i].rid) req_finish(&g_be.reqs[i], "account service unavailable", NULL);
    g_be.turf_asked = g_be.clan_has ? 1 : 0; // a clan already known stays; an unanswered ask goes again once linked
    if (!g_be.monetized) g_be.game_asked = 0;
}

void brain_events_tick(const float now) {
    for (int i = 0; i < BE_REQS_MAX; i++)
        if (g_be.reqs[i].rid && now >= g_be.reqs[i].deadline) req_finish(&g_be.reqs[i], "no answer", NULL);
    if (brain_linked()) { // the first round starts before the link is up; a reconnect asks again
        if (g_be.turf_map >= 0 && !g_be.turf_asked) turf_ask();
        if (g_be.game_map[0] && !g_be.game_asked) game_ask();
    }
    spinners_step();
}

void brain_events_round_start(const int turf_map) {
    g_be.round++;
    spinners_build(g_server->game.map); // game.init rebuilds MapManager.spinners with the map
    // territorialWars.reset(): clanInfo = null, no packet (the INIT that follows clears it on the clients)
    g_be.clan_has = 0;
    g_be.clan_name[0] = 0;
    g_be.turf_map = brain_enabled() ? turf_map : -1;
    g_be.turf_asked = 0;
    const char *map = accounts_published_map();
    if (!map || strcmp(map, g_be.game_map)) { // a published map's monetization is asked once per hosted game
        snprintf(g_be.game_map, sizeof(g_be.game_map), "%s", map && brain_enabled() ? map : "");
        g_be.game_asked = 0;
        g_be.monetized = 0;
    }
    if (brain_linked()) {
        if (g_be.turf_map >= 0) turf_ask();
        if (g_be.game_map[0] && !g_be.game_asked) game_ask();
    }
}

// init's syncData.warsClan (:220412-220420; the client applies it after the reset, :256999-257010)
void brain_events_send_init(ClientConnection *client) {
    if (!g_be.clan_has) return;
    NetWarsClanPacket p;
    wars_clan_packet(&p);
    send_ext(client, NET_EXT_WARS_CLAN, &p, sizeof(p));
}
