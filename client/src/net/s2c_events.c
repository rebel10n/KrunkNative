// The client side of the v10.0.3 S2C packets doc/v10/s2c-coverage.md added (client/include/s2c_events.h). Line
// numbers: deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js).
#include <s2c_events.h>
#include <net_ext.h>
#include <player_anim.h> // player_class_has_anims
#include <player_class_model.h> // the drawn class model: its clips and its asset
#include <spectate.h>    // client_find_player
#include <ui_logic.h>    // menu_show_speech_bubble, hud_ui_unboxed / raffle_won, s2c_ui_*
#include <backend_ui.h>  // account_set_funds / account_add_skin ("spinW")
#include <prize_wheels.h>
#include <audio.h>
#include <klog.h>
#include "../viewer/viewer_store.h" // viewer_skin_define
#include <cJSON.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- "c" PLAYER_ANIM

// iíîiiìï (:257767-257770) -> players.playClip(player, animList[clip], reps || 1) (:193806): an active player whose class
// has an animated model (hasAnims: the class's aID holds "g", "q" or "f", :194877) plays the clip on its drawn model,
// the GLTF of config.mdl 2's loadAnimMesh (:194251; client/src/player_class_model.c)
static void on_player_anim(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetPlayerAnimPacket)) return;
    NetPlayerAnimPacket p;
    memcpy(&p, body, sizeof(p));
    const char *clip = net_player_clip_name(p.clip);
    Player *player = client_find_player(client, p.sid);
    if (!clip || !player || !player->active || !player_class_has_anims(player)) return;
    KN_LOG_DEBUG1("[anim] player %d clip %s x%g%s\n", p.sid, clip, (double) p.reps, player_class_model_gltf(player) ? "" : " (no model drawn)");
    player_class_model_play_clip(player, clip, p.reps != 0.0f ? p.reps : 1.0f);
}

// ---------------------------------------------------------------- "uai" UPDATE_ASSET_ID

// iíiïiiï (:257782-257786) -> players.updateAssetID (:197964-197975): with config.mdl and a non-empty text the player's
// model takes that asset (player.assetTxt) and an active player is rebuilt at once (regenMeshes)
static void on_player_asset_id(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetPlayerAssetIdPacket)) return;
    NetPlayerAssetIdPacket p;
    memcpy(&p, body, sizeof(p));
    p.asset[sizeof(p.asset) - 1] = 0;
    Player *player = client_find_player(client, p.sid);
    if (!player) { // before its SPAWN (our server's onPlayerSpawn runs first): its first model takes it
        player_class_model_hold_asset(p.sid, p.asset);
        return;
    }
    if (!player_class_model_set_asset(player, p.asset)) return;
    KN_LOG_DEBUG1("[anim] player %d asset %s\n", p.sid, p.asset);
    if (!player->active || !player->mesh) return; // a dead player's next spawn builds it
    const int render_you = player->render_you;
    client_drop_player_mesh(client, player);
    client_add_player_mesh(client, player, render_you);
}

// ---------------------------------------------------------------- "sb" SHOW_SPEECH_BUBBLE

// iíììiîì (:261181): txtBubble.innerHTML = html (shown for ms || 2500), a sigh first when asked. The bubble here takes
// text (menu_show_speech_bubble escapes it): the server's tags are dropped, its text kept.
static void on_speech_bubble(const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetSpeechBubblePacket)) return;
    NetSpeechBubblePacket p;
    memcpy(&p, body, sizeof(p));
    p.text[sizeof(p.text) - 1] = 0;
    char text[NET_SPEECH_BUBBLE_TEXT];
    size_t n = 0;
    int in_tag = 0;
    for (const char *c = p.text; *c && n + 1 < sizeof(text); c++) {
        if (*c == '<') in_tag = 1;
        else if (*c == '>' && in_tag) in_tag = 0;
        else if (!in_tag) text[n++] = *c;
    }
    text[n] = 0;
    if (!text[0]) return;
    if (p.sigh) audio_play(rand() % 2 ? "sigh_1" : "sigh_0", 0.2f, 0, 1.0f); // ["sigh_0", "sigh_1"][randInt(0, 1)], 0.2
    menu_show_speech_bubble(text, p.ms ? (double) p.ms : 2500.0);
}

// ---------------------------------------------------------------- "sdc" SKIN_DEFINITION_CHANGED

// NetExtJsonChunk assembly of one [skinId, skin] record (the slices of one record come in order, nothing between)
static struct {
    char *data;
    size_t len;
    int next, parts;
} g_chunk;

void s2c_events_reset(void) {
    free(g_chunk.data);
    memset(&g_chunk, 0, sizeof(g_chunk));
}

static cJSON *json_record(const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetExtJsonChunk)) return NULL;
    NetExtJsonChunk head;
    memcpy(&head, body, sizeof(head));
    const uint8_t *data = body + sizeof(head);
    const size_t n = length - sizeof(head);
    if (!head.parts || head.part >= head.parts) return NULL;
    if (head.parts == 1) return cJSON_ParseWithLength((const char *) data, n);
    if (head.part == 0 || head.part != g_chunk.next) { // a new record (or a broken one: restart)
        s2c_events_reset();
        if (head.part != 0) return NULL;
        g_chunk.parts = head.parts;
    }
    char *grown = realloc(g_chunk.data, g_chunk.len + n);
    if (!grown) return NULL;
    g_chunk.data = grown;
    memcpy(g_chunk.data + g_chunk.len, data, n);
    g_chunk.len += n;
    g_chunk.next = head.part + 1;
    if (g_chunk.next < g_chunk.parts) return NULL;
    cJSON *json = cJSON_ParseWithLength(g_chunk.data, g_chunk.len);
    s2c_events_reset();
    return json;
}

// store.skins[skinId] = skin (:261712): the viewer store's definition of that index (viewer_skin_define.c). Like
// v10, models already built keep their look until they are built again (a respawn, a loadout change).
static void on_skin_definition(const uint8_t *body, const uint16_t length) {
    cJSON *record = json_record(body, length);
    const cJSON *id = cJSON_GetArrayItem(record, 0), *skin = cJSON_GetArrayItem(record, 1);
    if (cJSON_IsNumber(id) && id->valuedouble >= 0 && id->valuedouble <= VIEWER_SKIN_INDEX_MAX) {
        if (viewer_skin_define((int) id->valuedouble, skin))
            KN_LOG_DEBUG1("[store] skin %d redefined (sdc)\n", (int) id->valuedouble);
    }
    cJSON_Delete(record);
}

// ---------------------------------------------------------------- "p" / "mp" the spectated player's bars

// interactProg / interactMProg as iìììîíí / iiííïïi set them (:259394-259400): the HUD draws them while we spectate
// (ui_interact.c, build_ui.c); our own bars come from our player otherwise.
static struct {
    float value[NET_INTER_BAR_COUNT];
    int spectating;
} g_bars;

static void on_inter_prog(const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetInterProgPacket)) return;
    NetInterProgPacket p;
    memcpy(&p, body, sizeof(p));
    if (p.bar >= NET_INTER_BAR_COUNT) return;
    g_bars.value[p.bar] = p.prog > 0.0f && isfinite(p.prog) ? p.prog : 0.0f; // `prog || 0`, display = prog > 0
}

float s2c_events_spectate_bar(const int bar) {
    return bar >= 0 && bar < NET_INTER_BAR_COUNT && spectate_active() ? g_bars.value[bar] : 0.0f;
}

// ---------------------------------------------------------------- "unb" / "unbS" / "lotWin" chat lines

static void on_unbox(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetUnboxMessagePacket)) return;
    NetUnboxMessagePacket p;
    memcpy(&p, body, sizeof(p));
    p.name[sizeof(p.name) - 1] = 0;
    hud_ui_unboxed(client, p.kind == NET_UNBOX_PLAYER ? p.uid : -1, p.name, p.item, p.kr);
}

static void on_lottery_won(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetLotteryPrizePacket)) return;
    NetLotteryPrizePacket p;
    memcpy(&p, body, sizeof(p));
    p.ref[sizeof(p.ref) - 1] = 0;
    hud_ui_raffle_won(client, p.uid, p.type, p.ref, p.quantity);
}

// ---------------------------------------------------------------- "spinW" map prize wheels

// iiîïiiì (:254991): ours (socketId == ours, with an account): account.funds = funds, account.addSkin(skin); every
// client then spins the wheel (game.updateSpinner)
static void on_spin_wheel(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetSpinWheelPacket)) return;
    NetSpinWheelPacket p;
    memcpy(&p, body, sizeof(p));
    if (p.mine && active_account()) {
        account_set_funds(p.funds); // updateMenuAccountData
        if (p.skin >= 0) account_add_skin(p.skin);
    }
    prize_wheels_spin(client, p.uid, isfinite(p.rand) ? p.rand : 0.0f, p.skin);
}

// ---------------------------------------------------------------- "chreq" / "warsClan"

static void on_charge_request(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetChargeConsentRequestPacket)) return;
    NetChargeConsentRequestPacket p;
    memcpy(&p, body, sizeof(p));
    s2c_ui_charge_request(client, p.id, isfinite(p.amount) ? p.amount : 0.0, p.modal);
}

static void on_wars_clan(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetWarsClanPacket)) return;
    NetWarsClanPacket p;
    memcpy(&p, body, sizeof(p));
    p.clan_name[sizeof(p.clan_name) - 1] = 0;
    s2c_ui_wars_clan(client, p.has, p.clan_name);
}

// ---------------------------------------------------------------- "edMp" the editor

// NetExtBlobChunk assembly of the map JSON (its slices come in order)
static struct {
    char *data;
    size_t len;
    int next, parts;
} g_blob;

static void blob_reset(void) {
    free(g_blob.data);
    memset(&g_blob, 0, sizeof(g_blob));
}

static void on_edit_map(const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetExtBlobChunk)) return;
    NetExtBlobChunk head;
    memcpy(&head, body, sizeof(head));
    const size_t n = length - sizeof(head);
    if (!head.parts || head.part >= head.parts) return;
    if (head.part == 0 || head.part != g_blob.next || head.parts != g_blob.parts) {
        blob_reset();
        if (head.part != 0) return;
        g_blob.parts = head.parts;
    }
    char *grown = realloc(g_blob.data, g_blob.len + n + 1);
    if (!grown) {
        blob_reset();
        return;
    }
    g_blob.data = grown;
    memcpy(g_blob.data + g_blob.len, body + sizeof(head), n);
    g_blob.len += n;
    g_blob.data[g_blob.len] = 0;
    g_blob.next = head.part + 1;
    if (g_blob.next < g_blob.parts) return;
    s2c_ui_edit_map(g_blob.data, g_blob.len);
    blob_reset();
}

// ---------------------------------------------------------------- dispatch

int s2c_events_on_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    switch (subtype) {
        case NET_EXT_PLAYER_ANIM: on_player_anim(client, body, length); return 1;
        case NET_EXT_PLAYER_ASSET_ID: on_player_asset_id(client, body, length); return 1;
        case NET_EXT_SPEECH_BUBBLE: on_speech_bubble(body, length); return 1;
        case NET_EXT_SKIN_DEFINITION_CHANGED: on_skin_definition(body, length); return 1;
        case NET_EXT_PLAYER_INTER_PROG: on_inter_prog(body, length); return 1;
        case NET_EXT_UNBOX_MESSAGE: on_unbox(client, body, length); return 1;
        case NET_EXT_LOTTERY_PRIZE_WON: on_lottery_won(client, body, length); return 1;
        case NET_EXT_SPIN_WHEEL_RESPONSE: on_spin_wheel(client, body, length); return 1;
        case NET_EXT_CHARGE_CONSENT_REQUEST: on_charge_request(client, body, length); return 1;
        case NET_EXT_WARS_CLAN: on_wars_clan(client, body, length); return 1;
        case NET_EXT_KN_OFFICIAL_EDIT_MAP: on_edit_map(body, length); return 1;
        default: return 0;
    }
}

void s2c_events_on_init(Client *client) {
    player_class_model_reset(); // "uai" swaps held for players not seen yet
    memset(&g_bars, 0, sizeof(g_bars));
    blob_reset();
    prize_wheels_reset();
    s2c_ui_on_init(client);
}

void s2c_events_tick(Client *client) {
    const int spectating = spectate_active();
    if (spectating && !g_bars.spectating) memset(g_bars.value, 0, sizeof(g_bars.value)); // a new spectate: no bar yet
    g_bars.spectating = spectating;
    prize_wheels_tick(client);
    s2c_ui_tick(client);
}
