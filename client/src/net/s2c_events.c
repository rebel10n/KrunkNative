// The client side of the v10.0.3 S2C packets doc/v10/s2c-coverage.md added (client/include/s2c_events.h). Line
// numbers: deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js).
#include <s2c_events.h>
#include <net_ext.h>
#include <game_config.h> // class_overrides: the class's model and asset (aID)
#include <spectate.h>    // client_find_player
#include <ui_logic.h>    // menu_show_speech_bubble
#include <audio.h>
#include <klog.h>
#include "../viewer/viewer_store.h" // viewer_skin_define
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- "c" PLAYER_ANIM

// players.playClip (:193806): only an active player whose class has an animated model plays a clip (hasAnims: the
// class's aID holds "g", "q" or "f", :194877; the model is config.mdl 2's loadAnimMesh, :194251), on its meshRef.
static int player_has_anims(const Player *player) {
    const Game *game = player->game;
    if (!game || !game->classes || player->class_index < 0 || (size_t) player->class_index >= game->class_count) return 0;
    const ClassOverrides *o = class_overrides(&game->classes[player->class_index]);
    return o && o->model == 2 && (strchr(o->asset_id, 'g') || strchr(o->asset_id, 'q') || strchr(o->asset_id, 'f'));
}

// Known gap (doc/v10/s2c-coverage.md): this client draws no class model (config.mdl 1 / 2: player_mesh.c builds the
// default body for every class), so a clip has no model to play on yet; it is checked and logged. Once a class model
// is drawn as a GLTF instance, this is where gltf_instance_play_clip(model, name, reps) goes.
static void on_player_anim(Client *client, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetPlayerAnimPacket)) return;
    NetPlayerAnimPacket p;
    memcpy(&p, body, sizeof(p));
    const char *clip = net_player_clip_name(p.clip);
    Player *player = client_find_player(client, p.sid);
    if (!clip || !player || !player->active || !player_has_anims(player)) return;
    KN_LOG_DEBUG1("[anim] player %d clip %s x%g: no class model drawn (mdl 2)\n", p.sid, clip, (double) p.reps);
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

int s2c_events_on_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    switch (subtype) {
        case NET_EXT_PLAYER_ANIM: on_player_anim(client, body, length); return 1;
        case NET_EXT_SPEECH_BUBBLE: on_speech_bubble(body, length); return 1;
        case NET_EXT_SKIN_DEFINITION_CHANGED: on_skin_definition(body, length); return 1;
        default: return 0;
    }
}
