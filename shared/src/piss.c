// KrunkNative Piss: the stream's source and arc, and the puddles (shared/include/piss.h). The trace
// itself is player_stream_trace (shared/src/player.c, beside the hitscan trace it reuses).
#include <piss.h>
#include <object_manager.h>
#include <weapons.h>
#include <math.h>
#include <string.h>
#include <ruleset.h>

vec3 piss_stream_source(const Player *player, const WeaponStream *stream, const float pitch, const float yaw) {
    const float forward = stream->pelvis_forward;
    return (vec3) {
        player->position.x + sinf(yaw + (float) M_PI) * cosf(pitch) * forward,
        player->position.y + stream->pelvis_height - player->crouch_val * PISS_CROUCH_DROP + sinf(pitch) * PISS_PITCH_RISE,
        player->position.z + cosf(yaw + (float) M_PI) * cosf(pitch) * forward,
    };
}

// particles.js physObj: x += v.x * dt, v.y -= grav * dt (dt ms); the closed form of that flight
vec3 piss_stream_at(const vec3 source, const vec3 velocity, const float gravity, const float ms) {
    return (vec3) {source.x + velocity.x * ms, source.y + velocity.y * ms - 0.5f * gravity * ms * ms, source.z + velocity.z * ms};
}

static const WeaponStream *piss_weapon_stream(const Game *game, const int weapon_id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_TABLE_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    const Weapon *weapon = weapon_id >= 0 && (size_t) weapon_id < count ? all[weapon_id] : NULL;
    return weapon ? weapon->stream : NULL;
}

// ---------------------------------------------------------------- packets

static void puddle_send(Game *game, const int target, const int op, const GamePuddle *p) {
    uint8_t buffer[sizeof(NetSpawnedObjPacket) + sizeof(NetPuddlePayload)];
    const NetSpawnedObjPacket head = {.op = (uint8_t) op, .kind = OM_KIND_PUDDLE, .id = p->id, .owner = p->owner};
    memcpy(buffer, &head, sizeof(head));
    uint16_t length = sizeof(head);
    if (op == OM_SPAWNED_ADD) {
        const NetPuddlePayload body = {
            .position = {p->position.x, p->position.y, p->position.z},
            .radius = p->radius,
            .color = p->color,
            .weapon_id = (int16_t) p->weapon_id,
        };
        memcpy(buffer + length, &body, sizeof(body));
        length = (uint16_t) (length + sizeof(body));
    }
    game_send(game, target, NET_PACKET_SPAWNED_OBJ, buffer, length);
}

// Drops the puddle at `index` (the order stays oldest first); the authority tells everyone.
static void puddle_remove_at(Game *game, const int index) {
    GamePuddles *store = &game->puddles;
    if (index < 0 || index >= store->count) return;
    const GamePuddle gone = store->items[index];
    memmove(&store->items[index], &store->items[index + 1], (size_t) (store->count - index - 1) * sizeof(GamePuddle));
    store->count--;
    if (game_is_authority(game)) puddle_send(game, NET_TARGET_ALL, OM_SPAWNED_REMOVE, &gone);
}

static int puddle_index(const Game *game, const int id) {
    for (int i = 0; i < game->puddles.count; i++)
        if (game->puddles.items[i].id == id) return i;
    return -1;
}

// ---------------------------------------------------------------- authority

int piss_puddle_count(const Game *game, const int owner) {
    int n = 0;
    for (int i = 0; i < game->puddles.count; i++) n += owner < 0 || game->puddles.items[i].owner == owner;
    return n;
}

int piss_puddle_spawn(Game *game, const Player *owner, const int weapon_id, const vec3 at) {
    const WeaponStream *stream = piss_weapon_stream(game, weapon_id);
    if (!game_is_authority(game) || !owner || !stream || stream->puddle_radius <= 0.0f || stream->puddle_life <= 0.0f) return -1;
    GamePuddles *store = &game->puddles;

    // KN: one of the owner's puddles close by takes the new life instead (the branch made one per
    // 55 ms shot: overlapping puddles stacked their damage, and a packet each)
    const float merge = stream->puddle_radius * PISS_PUDDLE_MERGE;
    for (int i = store->count - 1; i >= 0; i--) {
        GamePuddle *p = &store->items[i];
        const float dx = p->position.x - at.x, dz = p->position.z - at.z;
        if (p->owner != owner->uid || dx * dx + dz * dz > merge * merge || fabsf(p->position.y - at.y) > 1.0f) continue;
        p->life = stream->puddle_life;
        return p->id;
    }

    // the caps: the owner's oldest, then the game's oldest (fireMustard: mustardPuddles.shift())
    const int per_player = stream->puddles_per_player > 0 ? stream->puddles_per_player : GAME_PUDDLES_MAX;
    const int per_game = stream->puddles_max > 0 && stream->puddles_max < GAME_PUDDLES_MAX ? stream->puddles_max : GAME_PUDDLES_MAX;
    while (piss_puddle_count(game, owner->uid) >= per_player) {
        int oldest = 0;
        while (store->items[oldest].owner != owner->uid) oldest++;
        puddle_remove_at(game, oldest);
    }
    while (store->count >= per_game) puddle_remove_at(game, 0);

    if (!store->count) store->tick = 0.0f; // the damage clock starts with the first puddle
    store->tick_period = stream->puddle_tick > 0.0f ? stream->puddle_tick : 250.0f;
    GamePuddle *p = &store->items[store->count++];
    *p = (GamePuddle) {
        .id = store->next_id++,
        .owner = owner->uid,
        .weapon_id = weapon_id,
        .position = at,
        .radius = stream->puddle_radius,
        .dps = stream->puddle_dps,
        .life = stream->puddle_life,
        .color = stream->color,
    };
    if (store->next_id < 0) store->next_id = 0;
    puddle_send(game, NET_TARGET_ALL, OM_SPAWNED_ADD, p);
    return p->id;
}

// updateMustardPuddles: a player in a puddle (x / z within the radius, y within 2.8) takes
// dps x tick / 1000 from its owner. KN: once per tick, from the newest puddle it stands in.
static void puddle_damage_tick(Game *game, const float tick_ms) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *victim = game->players[i];
        if (!victim || !victim->active) continue;
        for (int k = game->puddles.count - 1; k >= 0; k--) {
            const GamePuddle *p = &game->puddles.items[k];
            const float dx = victim->position.x - p->position.x, dz = victim->position.z - p->position.z;
            if (fabsf(victim->position.y - p->position.y) > PISS_PUDDLE_REACH_Y || dx * dx + dz * dz > p->radius * p->radius) continue;
            Player *owner = game_find_player(game, p->owner);
            // KN: never its owner (the branch's puddles hurt everyone in them), and only those the
            // owner may damage (teams, noPVP zones)
            if (!owner || owner == victim || !player_can_damage(owner, victim)) continue;
            const int weapon_id = p->weapon_id;
            if (player_change_health(victim, owner, p->dps * tick_ms / 1000.0f, 0) && victim->active) {
                PlayerKillInfo info = {.weapon_id = weapon_id};
                player_kill(victim, owner, &info, 0);
            }
            break;
        }
    }
}

void piss_tick(Game *game, const float delta) {
    GamePuddles *store = &game->puddles;
    if (!game_is_authority(game) || !store->count) return;
    const float ms = delta * 1000.0f;

    for (int i = store->count - 1; i >= 0; i--) {
        GamePuddle *p = &store->items[i];
        p->life -= ms;
        p->age += ms;
        if (p->life <= 0.0f || !game_find_player(game, p->owner)) puddle_remove_at(game, i);
    }

    const float period = store->tick_period > 0.0f ? store->tick_period : 250.0f;
    store->tick += ms;
    while (store->count && store->tick >= period - 1e-3f) {
        store->tick -= period;
        puddle_damage_tick(game, period);
    }
    if (!store->count) store->tick = 0.0f;
}

void piss_send_state(Game *game, const int uid) {
    if (!game_is_authority(game)) return;
    for (int i = 0; i < game->puddles.count; i++) puddle_send(game, uid, OM_SPAWNED_ADD, &game->puddles.items[i]);
}

void piss_player_left(Game *game, const Player *player) {
    if (!player) return;
    for (int i = game->puddles.count - 1; i >= 0; i--)
        if (game->puddles.items[i].owner == player->uid) puddle_remove_at(game, i);
}

// ---------------------------------------------------------------- both sides

void piss_round_start(Game *game) {
    memset(&game->puddles, 0, sizeof(game->puddles));
}

int piss_apply_spawned(Game *game, const void *payload, const uint16_t length) {
    if (!game || game_is_authority(game) || length < sizeof(NetSpawnedObjPacket)) return 0;
    NetSpawnedObjPacket head;
    memcpy(&head, payload, sizeof(head));
    if (head.kind != OM_KIND_PUDDLE) return 0;
    const int at = puddle_index(game, head.id);
    if (head.op == OM_SPAWNED_REMOVE) {
        if (at >= 0) puddle_remove_at(game, at);
        return 1;
    }
    if (head.op != OM_SPAWNED_ADD || length != sizeof(head) + sizeof(NetPuddlePayload)) return 0;
    NetPuddlePayload body;
    memcpy(&body, (const uint8_t *) payload + sizeof(head), sizeof(body));
    if (!isfinite(body.position[0]) || !isfinite(body.position[1]) || !isfinite(body.position[2]) || !isfinite(body.radius)) return 0;
    GamePuddles *store = &game->puddles;
    if (at >= 0) puddle_remove_at(game, at); // a stale copy of that id
    if (store->count >= GAME_PUDDLES_MAX) puddle_remove_at(game, 0);
    store->items[store->count++] = (GamePuddle) {
        .id = head.id,
        .owner = head.owner,
        .weapon_id = body.weapon_id,
        .position = {body.position[0], body.position[1], body.position[2]},
        .radius = fminf(fmaxf(body.radius, 0.0f), 64.0f),
        .color = body.color,
    };
    return 1;
}

void piss_client_tick(Game *game, const float delta) {
    if (game_is_authority(game)) return; // the authority ages them in piss_tick
    for (int i = 0; i < game->puddles.count; i++) game->puddles.items[i].age += delta * 1000.0f;
}
