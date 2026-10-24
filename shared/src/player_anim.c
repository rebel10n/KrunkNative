// v10.0.3 player clips (shared/include/player_anim.h). Line numbers: deob-10.0.3.js.
#include <player_anim.h>
#include <game_config.h> // class_overrides: the class's aID
#include <modes.h>       // mode.viewDis
#include <net_ext.h>
#include <string.h>

int player_class_has_anims(const Player *player) {
    const Game *game = player ? player->game : NULL;
    if (!game || !game->classes || player->class_index < 0 || (size_t) player->class_index >= game->class_count) return 0;
    const ClassOverrides *o = class_overrides(&game->classes[player->class_index]);
    // regenMeshes / players.add: hasAnims = aID.includes("g") || includes("q") || includes("f") (:194876-194877)
    return o && (strchr(o->asset_id, 'g') || strchr(o->asset_id, 'q') || strchr(o->asset_id, 'f'));
}

void player_play_clip(Player *player, const int clip) {
    Game *game = player ? player->game : NULL;
    if (!game || !game_is_authority(game) || !player->active || clip < 0 || clip >= NET_PLAYER_CLIP_COUNT) return;
    if (!player_class_has_anims(player)) return; // playClip: `player.active && player.hasAnims` (:193807)
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
