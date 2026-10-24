// v10.0.3 player clips (shared/include/player_anim.h). Line numbers: deob-10.0.3.js.
#include <player_anim.h>
#include <game_config.h> // class_overrides: the class's aID
#include <modes.h>       // mode.viewDis
#include <net_ext.h>
#include <stdio.h>
#include <string.h>
#ifdef KRUNKNATIVE_CLIENT
#include <player_class_model.h> // the client's branch: the drawn class model
#endif

static int g_replaying; // net_reconcile is replaying pending inputs (procInputs recon)

static const ClassOverrides *player_class(const Player *player) {
    const Game *game = player ? player->game : NULL;
    if (!game || !game->classes || player->class_index < 0 || (size_t) player->class_index >= game->class_count) return NULL;
    return class_overrides(&game->classes[player->class_index]);
}

int player_class_has_anims(const Player *player) {
    const ClassOverrides *o = player_class(player);
    // regenMeshes / players.add: hasAnims = aID.includes("g") || includes("q") || includes("f") (:194876-194877)
    return o && (strchr(o->asset_id, 'g') || strchr(o->asset_id, 'q') || strchr(o->asset_id, 'f'));
}

void player_anim_set_replay(const int replaying) {
    g_replaying = replaying;
}

void player_play_clip(Player *player, const int clip) {
    Game *game = player ? player->game : NULL;
    if (!game || !player->active || clip < 0 || clip >= NET_PLAYER_CLIP_COUNT) return;
    if (!player_class_has_anims(player)) return; // playClip: `player.active && player.hasAnims` (:193807)
    if (!game_is_authority(game)) {
#ifdef KRUNKNATIVE_CLIENT
        // the client's branch (:193817-193819): meshRef.playClip(name, reps) on the drawn model; players.jump plays
        // nothing in a reconciliation replay (`if (!recon)`, :195271), and replays never melee or shoot here
        if (!g_replaying) player_class_model_play_clip(player, net_player_clip_name(clip), 1.0f);
#endif
        return;
    }
    // send(PLAYER_ANIM, sid, animList.indexOf(clip)): two arguments, so the client's reps is `undefined || 1`
    const NetPlayerAnimPacket p = {.sid = player->uid, .clip = (uint8_t) clip, .reps = 1.0f};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(p)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_PLAYER_ANIM, &p, sizeof(p));
    if (!n) return;
    // spectators: the connections without a player (KrunkNative keeps spectators out of Game.players)
    game_send(game, NET_TARGET_NON_PLAYERS, NET_PACKET_EXT, buf, n);
    // dist3(other, player) < (mode.viewDis || 600) for every other active player (:193811)
    const float view = game->mode && game->mode->config.view_dis > 0.0f ? game->mode->config.view_dis : 600.0f;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *other = game->players[i];
        if (!other || other == player || !other->active) continue;
        const float dx = other->position.x - player->position.x, dy = other->position.y - player->position.y,
                    dz = other->position.z - player->position.z;
        if (dx * dx + dy * dy + dz * dz < view * view) game_send(game, other->uid, NET_PACKET_EXT, buf, n);
    }
}

void player_update_asset_id(Player *player, const char *asset_txt) {
    Game *game = player ? player->game : NULL;
    const ClassOverrides *o = player_class(player);
    // `if (config.mdl && assetTxt.length)` (:197966)
    if (!game || !o || !o->model || !asset_txt || !asset_txt[0]) return;
    if (game_is_authority(game)) {
        // isServer: broadcast("uai", sid, assetTxt) (:197968-197970). The server's own regenMeshes builds no model;
        // its side effects there (updateHeadSize, swapWeapon(player, 0, true)) are not repeated.
        NetPlayerAssetIdPacket p = {.sid = player->uid};
        snprintf(p.asset, sizeof(p.asset), "%s", asset_txt);
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(p)];
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_PLAYER_ASSET_ID, &p, sizeof(p));
        if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buf, n);
        return;
    }
#ifdef KRUNKNATIVE_CLIENT
    // a client script: assetTxt on our copy, the model rebuilt with it (player_class_model_stale)
    player_class_model_set_asset(player, asset_txt);
#endif
}
