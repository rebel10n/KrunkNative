// Krunker v10.0.3 Stalker outside the mode rules (shared/include/stalker.h, doc/v10/stalker.md).
#include <stalker.h>
#include <modes.h>
#include <net_ext.h>
#include <stdio.h>
#include <string.h>

int player_is_stalker(const Game *game, const Player *player) {
    if (!game || !player || !game->mode || !game->mode->config.stalker_v10 || player->team < 1 || player->team > 2) return 0;
    const char *option = game->mode->config.team_options[player->team - 1];
    return option && !strcmp(option, "Stalker");
}

void stalker_update_hidden(Player *player, const float delta) {
    if (!player) return;
    // velocity.x || velocity.y || velocity.z ? noMovT = 0 : noMovT += delta (deob-10.0.3.js:191767-191771)
    if (player->velocity.x != 0.0f || player->velocity.y != 0.0f || player->velocity.z != 0.0f) player->no_move_time = 0.0f;
    else player->no_move_time += delta;
    player->is_hidden = (unsigned char) ((player_is_stalker(player->game, player) && player->no_move_time >= STALKER_HIDE_TIME) || player->force_hide);
}

int game_player_stalker_taunt(Game *game, Player *player, const int index) {
    // v10's guard reads `p && p.active && i >= 0 && i < 6 && !p.lastTaunt || now - p.lastTaunt >= 0`
    // (deob-10.0.3.js:198333): past the first taunt the active / index checks drop out by precedence.
    // KrunkNative checks them on every taunt (an index past 5 has no sound), and only takes taunts from
    // the stalker (v10's client sends them only then, deob-10.0.3.js:260106).
    if (!game || !player || !player->active || index < 0 || index >= STALKER_TAUNT_COUNT || !game_is_authority(game)) return 0;
    if (!player_is_stalker(game, player)) return 0;
    if (player->taunt_ready_at > 0.0f && game->now < player->taunt_ready_at) return 0;
    player->taunt_ready_at = game->now + STALKER_TAUNT_COOLDOWN;
    // addPlayerSound on the server: a one-shot is broadcast ("asnd") but not kept in playerSounds
    // (`data.loop || !isServer`, deob-10.0.3.js:220429), so a joiner never hears it again. Its dist / roll
    // go first, in the EXT the PLAYER_SOUND has no room for (net_ext.h NET_EXT_PLAYER_SOUND_FALLOFF).
    const NetPlayerSoundFalloffPacket falloff = {.uid = player->uid, .distance = STALKER_TAUNT_DISTANCE, .rolloff = STALKER_TAUNT_ROLLOFF};
    uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(falloff)];
    const uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_PLAYER_SOUND_FALLOFF, &falloff, sizeof(falloff));
    if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, ext, n);
    NetPlayerSoundPacket packet = {.uid = player->uid, .add = 1, .loop = 0, .volume = STALKER_TAUNT_VOLUME, .rate = STALKER_TAUNT_RATE};
    snprintf(packet.name, sizeof(packet.name), "staunt_%d", index);
    game_send(game, NET_TARGET_ALL, NET_PACKET_PLAYER_SOUND, &packet, sizeof(packet));
    return 1;
}

void stalker_send_picked(Game *game, const Player *stalker) {
    if (!game || !stalker || !game_is_authority(game)) return;
    uint8_t buffer[NET_EXT_HEADER_SIZE];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_STALKER_PICKED, NULL, 0);
    if (n) game_send(game, NET_TARGET_EXCEPT(stalker->uid), NET_PACKET_EXT, buffer, n);
}
