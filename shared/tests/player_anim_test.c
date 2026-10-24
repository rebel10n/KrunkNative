// player_anim_test: v10.0.3 PLAYER_ANIM ("c") from the authority (shared/src/player_anim.c; players.playClip's isServer
// branch, deob-10.0.3.js:193806-193815). A player whose class has an animated model (aID with "g" / "q" / "f") sends
// NET_EXT_PLAYER_ANIM [sid, clip] to the spectators (NET_TARGET_NON_PLAYERS) and to the other active players within
// mode.viewDis || 600; nobody else, and nothing for a class without one, an inactive player or a non-authority game.
// The jump and the melee call it (player.c). Run from the repository root.
#include <shared.h>
#include <player_anim.h>
#include <game_config.h>
#include <modes.h>
#include <net_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_SENT 64
typedef struct {
    int uid;
    NetPlayerAnimPacket anim;
} Sent;
static Sent g_sent[MAX_SENT];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    uint16_t subtype = 0, body_length = 0;
    const uint8_t *body = NULL;
    if (type != NET_PACKET_EXT || !net_ext_unpack(payload, length, &subtype, &body, &body_length)) return;
    if (subtype != NET_EXT_PLAYER_ANIM || body_length != sizeof(NetPlayerAnimPacket) || g_sent_count >= MAX_SENT) return;
    g_sent[g_sent_count].uid = uid;
    memcpy(&g_sent[g_sent_count].anim, body, sizeof(NetPlayerAnimPacket));
    g_sent_count++;
}

static int sent_to(const int uid) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].uid == uid;
    return n;
}

static const cJSON *g_test_maps[1];

static Player *add_player(Game *game, const int uid, const float x) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player->active = 1;
    player->class_index = 0;
    player->position = (vec3) {x, 0.0f, 0.0f};
    return player;
}

int main(void) {
    Game game;
    memset(&game, 0, sizeof(game));
    int *modes = calloc(1, sizeof(int));
    modes[0] = MODE_ID_TDM;
    game_configure(&game, NULL, g_test_maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = test_send;
    game_init(&game, 0, 0, 1); // is_local: the authority

    Player *actor = add_player(&game, 1, 0.0f);
    add_player(&game, 2, 100.0f);  // within 600
    add_player(&game, 3, 900.0f);  // past 600
    Player *idle = add_player(&game, 4, 50.0f);
    idle->active = 0;              // not active (dead / in the menu)

    // the default class: no model asset, no clips
    player_play_clip(actor, NET_PLAYER_CLIP_JUMP);
    CHECK(g_sent_count == 0 && !player_class_has_anims(actor), "no aID: nothing sent");

    // cls[0] mdl 2 with an animated GLTF asset (aID "4821g")
    ClassConfig *cls = &game.classes[actor->class_index];
    cls->ext.set = 1;
    cls->ext.model = 2;
    snprintf(cls->ext.asset_id, sizeof(cls->ext.asset_id), "%s", "4821g");
    CHECK(player_class_has_anims(actor), "aID 4821g: hasAnims");
    player_play_clip(actor, NET_PLAYER_CLIP_JUMP);
    CHECK(g_sent_count == 2 && sent_to(NET_TARGET_NON_PLAYERS) == 1 && sent_to(2) == 1 && !sent_to(1) && !sent_to(3) && !sent_to(4),
          "Jump: the spectators and player 2 only (%d sent)", g_sent_count);
    CHECK(g_sent[0].anim.sid == 1 && g_sent[0].anim.clip == NET_PLAYER_CLIP_JUMP && g_sent[0].anim.reps == 1.0f, "Jump: [sid 1, clip 3] x1");
    g_sent_count = 0;

    // mode.viewDis widens / narrows the reach
    if (game.mode) {
        game.mode->config.view_dis = 1000.0f;
        player_play_clip(actor, NET_PLAYER_CLIP_MELEE);
        CHECK(sent_to(2) == 1 && sent_to(3) == 1 && g_sent[g_sent_count - 1].anim.clip == NET_PLAYER_CLIP_MELEE, "viewDis 1000: player 3 too");
        game.mode->config.view_dis = 0.0f;
        g_sent_count = 0;
    }

    // "q" (GLB) and "f" (FBX) count; other letters do not
    snprintf(cls->ext.asset_id, sizeof(cls->ext.asset_id), "%s", "77q");
    CHECK(player_class_has_anims(actor), "aID 77q: hasAnims");
    snprintf(cls->ext.asset_id, sizeof(cls->ext.asset_id), "%s", "77en");
    CHECK(!player_class_has_anims(actor), "aID 77en: an OBJ model, no clips");
    snprintf(cls->ext.asset_id, sizeof(cls->ext.asset_id), "%s", "4821g");

    // an inactive actor, a clip past the list, a non-authority game (a client mirror): nothing
    actor->active = 0;
    player_play_clip(actor, NET_PLAYER_CLIP_JUMP);
    actor->active = 1;
    player_play_clip(actor, NET_PLAYER_CLIP_COUNT);
    game.is_local = 0;
    game.server = NULL;
    player_play_clip(actor, NET_PLAYER_CLIP_JUMP);
    CHECK(g_sent_count == 0, "inactive / bad clip / not the authority: nothing (%d)", g_sent_count);

    printf("player_anim_test: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
