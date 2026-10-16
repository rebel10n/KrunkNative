// Prop Hunt: the props, a player's disguise and rotation, the Seeker penalty and the taunt timer
// (shared/include/prop_hunt.h).
#include <prop_hunt.h>
#include "mode_internal.h"
#include <prefabs.h>
#include <prefab_flags.h>
#include <social_v7.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// prefabs.js CP / CPY of the 14 props (the generated prefab table keeps neither): CP shrinks the
// player size updateProp gives, CPY moves the model; 0 for the others
static const struct {
    int id;
    float cp, cpy;
} k_prop_offsets[] = {
    {2, -1.0f, 0.0f},   // BARREL
    {16, -2.0f, -0.5f}, // CONE
    {28, -3.6f, -3.0f}, // TEDDY
    {36, -2.0f, -2.5f}, // CARDB
    {37, 0.0f, -5.0f},  // PALLET
    {51, -1.0f, 0.0f},  // EBARREL
    {59, 0.0f, 4.0f},   // KNIGHT
};

static void prop_offsets(const int id, float *cp, float *cpy) {
    *cp = *cpy = 0.0f;
    for (size_t i = 0; i < sizeof(k_prop_offsets) / sizeof(k_prop_offsets[0]); i++) {
        if (k_prop_offsets[i].id != id) continue;
        *cp = k_prop_offsets[i].cp;
        *cpy = k_prop_offsets[i].cpy;
        return;
    }
}

int prop_is_valid(const int prop_id) {
    return prop_id >= 0 && prop_id < PREFAB_TABLE_COUNT && prefab_has_flag(prop_id, PF_ISPROP);
}

int prop_count(void) {
    int n = 0;
    for (int id = 0; id < PREFAB_TABLE_COUNT; id++) n += prop_is_valid(id);
    return n;
}

int prop_at(const int index) {
    for (int id = 0, n = 0; id < PREFAB_TABLE_COUNT; id++) {
        if (!prop_is_valid(id)) continue;
        if (n++ == index) return id;
    }
    return -1;
}

int prop_index_of(const int prop_id) {
    if (!prop_is_valid(prop_id)) return -1;
    int n = 0;
    for (int id = 0; id < prop_id; id++) n += prop_is_valid(id);
    return n;
}

const char *prop_name(const int prop_id) {
    const PrefabData *data = prop_is_valid(prop_id) ? prefab_data(prop_id) : NULL;
    return data && data->name ? data->name : "";
}

void prop_model_src(const int prop_id, char *out, const size_t size) {
    // loadMesh({src: prefab.name.toLowerCase() + "_0"}) (:171320)
    size_t n = 0;
    for (const char *s = prop_name(prop_id); *s && n + 3 < size; s++) out[n++] = (char) tolower((unsigned char) *s);
    if (n + 2 < size) {
        out[n++] = '_';
        out[n++] = '0';
    }
    if (size) out[n < size ? n : size - 1] = 0;
}

float prop_model_scale(const int prop_id) {
    const PrefabData *data = prop_is_valid(prop_id) ? prefab_data(prop_id) : NULL;
    return data && data->scale > 0.0f ? data->scale : 1.0f;
}

float prop_model_y(const int prop_id) {
    float cp, cpy;
    prop_offsets(prop_id, &cp, &cpy);
    return prop_model_scale(prop_id) + cpy;
}

float prop_size(const int prop_id) {
    float cp, cpy;
    prop_offsets(prop_id, &cp, &cpy);
    return prop_model_scale(prop_id) + cp;
}

int prop_random(void) {
    const int n = prop_count();
    return n > 0 ? prop_at(mode_rand_int(0, n - 1)) : PROP_DEFAULT_ID;
}

int game_player_is_seeker(const Game *game, const Player *player) {
    if (!game || !game->mode || !player || player->team < 1 || player->team > 2) return 0;
    const char *option = game->mode->config.team_options[player->team - 1];
    return option && !strcmp(option, "Seekers");
}

float prop_model_yaw(const Player *player) {
    if (!player) return 0.0f;
    return player->prop_rot ? player->direction.y : player->prop_rot_saved_set ? player->prop_rot_saved : 0.0f;
}

void prop_reset(Player *player) {
    if (!player) return;
    player->prop_id = PROP_DEFAULT_ID;
    player->prop_switches = 0;
    player->prop_rot = 0;
    player->prop_rot_saved_set = 0;
    player->prop_rot_saved = 0.0f;
    player->scale = game_constants.player_scale;
}

void prop_apply(Player *player, const int prop_id) {
    if (!player || !prop_is_valid(prop_id)) return;
    player->prop_id = prop_id;
    player->scale = prop_size(prop_id);  // the collision / hit box half width
    player->height = prop_size(prop_id); // updateHeight puts it back on the next tick, as in v10
}

// ---------------------------------------------------------------- the authority

uint16_t prop_pack_update(const Player *player, const int limit, uint8_t *out, const size_t size) {
    if (!player) return 0;
    const NetPropUpdatePacket body = {.uid = player->uid, .prop_id = (int16_t) player->prop_id, .skip = 0,
                                      .switches = (int8_t) (player->prop_switches > 127 ? 127 : player->prop_switches), .limit = (uint8_t) !!limit};
    return net_ext_pack(out, size, NET_EXT_PROP_UPDATE, &body, sizeof(body));
}

uint16_t prop_pack_rotation(const Player *player, const int reset, uint8_t *out, const size_t size) {
    if (!player) return 0;
    const NetPropRotationPacket body = {.uid = player->uid, .rot = player->prop_rot, .saved_set = player->prop_rot_saved_set,
                                        .reset = (uint8_t) !!reset, .saved = player->prop_rot_saved};
    return net_ext_pack(out, size, NET_EXT_PROP_ROTATION, &body, sizeof(body));
}

void game_prop_send(Game *game, const Player *player, const int target) {
    if (!game || !player || !game->mode || !game->mode->config.prop || !game_player_is_prop(game, player)) return;
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(NetPropRotationPacket) + sizeof(NetPropUpdatePacket)];
    uint16_t n = prop_pack_update(player, 0, buffer, sizeof(buffer));
    if (n) game_send(game, target, NET_PACKET_EXT, buffer, n);
    if (player->prop_rot || player->prop_rot_saved_set) {
        n = prop_pack_rotation(player, 0, buffer, sizeof(buffer));
        if (n) game_send(game, target, NET_PACKET_EXT, buffer, n);
    }
}

void game_prop_spawned(Game *game, Player *player) {
    if (!game || !player) return;
    prop_reset(player);
    if (!game_is_authority(game) || !game->mode || !game->mode->config.prop || !game_player_is_prop(game, player)) return;
    prop_apply(player, prop_random());
}

int game_prop_pick(Game *game, Player *player, const int prop_id) {
    if (!game || !player || !player->active || !game_is_authority(game) || !game->mode || !game->mode->config.prop) return 0;
    if (!game_player_is_prop(game, player) || !prop_is_valid(prop_id) || player->prop_id == prop_id) return 0;
    const int max = game->mode->config.prop_switches;
    if (max > 0 && player->prop_switches >= max) {
        uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(NetPropUpdatePacket)];
        const uint16_t n = prop_pack_update(player, 1, buffer, sizeof(buffer)); // "modes.prophunt.limit" for the picker
        if (n) game_send(game, player->uid, NET_PACKET_EXT, buffer, n);
        return 0;
    }
    player->prop_switches++;
    prop_apply(player, prop_id);
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(NetPropUpdatePacket)];
    const uint16_t n = prop_pack_update(player, 0, buffer, sizeof(buffer)); // broadcast('up', sid, propID, false, propSwitches)
    if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buffer, n);
    return 1;
}

int game_prop_rotate(Game *game, Player *player, const int reset) {
    if (!game || !player || !player->active || !game_is_authority(game) || !game->mode || !game->mode->config.prop) return 0;
    if (!game_player_is_prop(game, player)) return 0;
    if (reset) {
        player->prop_rot = 0;
        player->prop_rot_saved_set = 0;
        player->prop_rot_saved = 0.0f;
    } else if (player->prop_rot) { // off: the model keeps the current look
        player->prop_rot = 0;
        player->prop_rot_saved_set = 1;
        player->prop_rot_saved = player->direction.y;
    } else {
        player->prop_rot = 1;
    }
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(NetPropRotationPacket)];
    const uint16_t n = prop_pack_rotation(player, reset, buffer, sizeof(buffer));
    if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buffer, n);
    return 1;
}

void game_prop_seeker_hit(Game *game, Player *dealer, const Object *object) {
    if (!game || !dealer || !object || !game_is_authority(game) || !game->mode || !game->mode->config.prop) return;
    if (!game_player_is_seeker(game, dealer) || !prop_is_valid((int) object->prefab)) return;
    if (player_change_health(dealer, NULL, PROP_SEEKER_PENALTY, 0)) player_kill(dealer, NULL, NULL, 0);
}

// ---------------------------------------------------------------- taunts

static void prop_push_taunt_timer(Game *game, const float overflow) {
    const ModeWaitTimer timer = {
        .time = PROP_TAUNT_INTERVAL - overflow,
        .msg = "next taunt in ",
        .trigger = mode_trigger_next_taunt,
        .cont_time = 1, // canDMG too: a running clock (mode_waiting) lets players hurt each other
    };
    mode_push_wait_timer(game, &timer, 1); // nextTaunt(time) without push: waitTimers = [timer]
}

void mode_trigger_next_taunt(Game *game, const float overflow) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->active && game_player_is_prop(game, player))
            game_player_taunt_ex(game, player, mode_rand_int(0, TAUNT_COUNT - 1), 1); // players.taunt(p, rand, true)
    }
    prop_push_taunt_timer(game, overflow);
}

void mode_trigger_release_seekers_prop(Game *game, const float overflow) {
    mode_trigger_release_seeker(game, overflow);
    prop_push_taunt_timer(game, 0.0f); // nextTaunt(null, null, true): the timer only
}
