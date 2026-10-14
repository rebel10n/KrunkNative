// KrunkNative Purple Drank (shared/include/drank.h): the puddle slow and the laced event. The
// stream, trace and puddles are the Piss's (shared/src/piss.c, player.c player_stream_trace).
#include <drank.h>
#include <net_ext.h>
#include <pcg_basic.h>
#include <piss.h>
#include <ruleset.h>
#include <math.h>
#include <string.h>

static const WeaponStream *drank_weapon_stream(const Game *game, const int weapon_id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_TABLE_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    const Weapon *weapon = weapon_id >= 0 && (size_t) weapon_id < count ? all[weapon_id] : NULL;
    return weapon ? weapon->stream : NULL;
}

// ---------------------------------------------------------------- authority

static void drank_set_laced(Player *player, const float seconds) {
    player->laced_timer = seconds;
    player->laced_duration = seconds;
}

int drank_pour(Player *player) {
    Game *game = player ? player->game : NULL;
    const WeaponStream *stream = player && player->weapon ? player->weapon->stream : NULL;
    if (!game || !stream || stream->laced_chance <= 0.0f || stream->laced_ms <= 0.0f || !game_is_authority(game)) return 0;
    // one bounded draw per pour on the shared stream: a seeded game replays the same rolls
    const uint32_t threshold = (uint32_t) lroundf(fminf(1.0f, stream->laced_chance) * (float) DRANK_LACED_ROLL_DENOM);
    if (pcg32_boundedrand(DRANK_LACED_ROLL_DENOM) >= threshold) return 0;

    drank_set_laced(player, stream->laced_ms / 1000.0f);
    const NetLacedPacket body = {.uid = player->uid, .duration_ms = (uint32_t) stream->laced_ms};
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_KN_LACED, &body, sizeof(body));
    if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buffer, n); // the pourer too: its own post-effect
    return 1;
}

// ---------------------------------------------------------------- both sides

void drank_tick(Game *game, const float delta) {
    if (!game || delta <= 0.0f) return;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || player->laced_timer <= 0.0f) continue;
        player->laced_timer = fmaxf(0.0f, player->laced_timer - delta);
        if (!player->laced_timer) player->laced_duration = 0.0f;
    }
}

int drank_apply_laced(Game *game, const void *body, const uint16_t length) {
    if (!game || game_is_authority(game) || !body || length != sizeof(NetLacedPacket)) return 0;
    NetLacedPacket packet;
    memcpy(&packet, body, sizeof(packet));
    Player *player = game_find_player(game, packet.uid);
    if (!player || packet.duration_ms == 0 || packet.duration_ms > 600000u) return 0; // at most 10 minutes
    drank_set_laced(player, (float) packet.duration_ms / 1000.0f);
    return 1;
}

int drank_is_laced(const Player *player) {
    return player && player->laced_timer > 0.0f;
}

// updateMustardPuddles' reach (piss.c puddle_damage_tick) with the slow instead of the damage:
// never the owner, only those the owner may damage; the strongest puddle underfoot, no stacking.
float drank_puddle_slow(const Game *game, const Player *player) {
    float slow = 1.0f;
    if (!game || !player || !game->puddles.count) return slow;
    for (int k = game->puddles.count - 1; k >= 0; k--) {
        const GamePuddle *p = &game->puddles.items[k];
        const WeaponStream *stream = drank_weapon_stream(game, p->weapon_id);
        if (!stream || stream->puddle_slow <= 0.0f || stream->puddle_slow >= slow) continue;
        const float dx = player->position.x - p->position.x, dz = player->position.z - p->position.z;
        if (fabsf(player->position.y - p->position.y) > PISS_PUDDLE_REACH_Y || dx * dx + dz * dz > p->radius * p->radius) continue;
        const Player *owner = game_find_player((Game *) game, p->owner);
        if (!owner || owner == player || !player_can_damage(owner, player)) continue;
        slow = stream->puddle_slow;
    }
    return slow;
}
