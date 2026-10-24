// v10 KrunkScript entities, runtime blocks / assets, moved dyn objects and player locks on the client
// (client/include/entities.h). Line numbers are deob-10.0.3.js (the entity manager :212465-214818, the live
// objects :212096-212411).
//
// The entities themselves are the shared manager's replicated copy (shared/include/ks_entities.h): the records
// land there (applySpawn / applyBaselines with its replay / applyRemove / applyAttach / applyControlledSync),
// and every frame (entities_update) it steps the "sim" entities with the map's scripts.shared on the entity
// clock, runs the client script's entity hooks and keeps the render poses. Its bodies (buildBody's addBlock per part,
// placed at the committed pose, carrying our player) and the live objects ("lo": blocks, assets) are spawned map
// objects it appends after the map's own: collision reads them like any object, and map_post adds their meshes to
// the scene. This file keeps the entity clock (entityClock, :215740-215802), puts the part meshes at the render
// pose (updateVisuals) and turns our look with a ridden entity (the lk yaw lock).
//
// Deliberate differences (documented): model parts ("aid") are hidden collision boxes (no GLTF part models,
// so playPartClip has nothing to play); a body's pitch / roll turn its part meshes but not their offsets;
// soft zones carry nothing on a client but its own player (the shared manager); the entity clock is reset at
// an INIT and runs on the client's frame clock instead of Date.now(). Player poses ("pz",
// NET_EXT_PLAYER_POSE) are routed to player_pose_render.c, which reads every player's hard attachment from
// here (a faceRide pose faces the entity it rides).
#include <entities.h>
#include <ks_entities.h>
#include <net_ext.h>
#include <player_pose_render.h> // "pz"
#include <user_asset.h>         // a live asset's GLB joins the scene

#include <prefabs.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_SAMPLES 256    // entityClock: the samples of the last 2 s, 256 at most

static struct {
    Game *game;              // the client's game (the shared manager's copy lives on it)
    // the NetExtJsonChunk being assembled
    uint16_t chunk_subtype;
    char *chunk;
    size_t chunk_len;
    int chunk_next, chunk_parts;
    // entityClock: (local time, server time - local time) samples, the smoothed offset
    double local_ms;
    double sample_t[CLOCK_SAMPLES], sample_off[CLOCK_SAMPLES];
    int sample_head, sample_count;
    double clock_offset;
    int clock_synced;
    // the camera's ride (entLookEid / entLookYaw)
    int look_eid;
    double look_yaw;
    unsigned spawns, removes, states, events, attaches, lives, moves;
} E = {.look_eid = -1};

void entities_reset(void) {
    free(E.chunk);
    E.chunk = NULL;
    E.chunk_len = 0;
    E.chunk_next = E.chunk_parts = 0;
    if (E.game) ks_entities_client_reset(E.game); // the shared copy, its bodies and live objects (freed with the map)
    player_pose_reset(); // the old session's pending "pz" poses go with its players
    E.sample_head = E.sample_count = 0; // KN: a new INIT starts the entity clock over (v10 keeps its samples)
    E.clock_synced = 0;
    E.clock_offset = 0.0;
    E.look_eid = -1;
    E.spawns = E.removes = E.states = E.events = E.attaches = E.lives = E.moves = 0;
}

// Our player stands on top of `o` (onTopOf: grounded, feet at its top, inside its footprint)
static int me_on_top(const Client *client, const Object *o) {
    const Player *me = client->me;
    if (!me || !me->active || !me->on_ground || !o || o->collision_type == COLLISION_TYPE_NONE) return 0;
    const vec3 s = object_box_size(o);
    const float r = game_constants.player_scale;
    return fabsf(me->position.y - (o->position.y + s.y)) < 0.6f && fabsf(me->position.x - o->position.x) <= s.x * 0.5f + r &&
           fabsf(me->position.z - o->position.z) <= s.z * 0.5f + r;
}

static void carry_me(Client *client, const vec3 d) {
    Player *me = client->me;
    if (!me || (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f)) return;
    me->position.x += d.x;
    me->position.y += d.y;
    me->position.z += d.z;
}

// ---------------------------------------------------------------- the entity clock (entityClock, :215740-215802)

// addSample(serverT, localT): the offsets of the last 2 s (256 at most); the clock takes their largest (the
// least delayed packet), snaps to a jump over 250 ms and otherwise eases a tenth of the way
static void clock_sample(const double server_ms, const double local_ms) {
    if (!isfinite(server_ms) || !isfinite(local_ms)) return;
    const int slot = (E.sample_head + E.sample_count) % CLOCK_SAMPLES;
    if (E.sample_count == CLOCK_SAMPLES) E.sample_head = (E.sample_head + 1) % CLOCK_SAMPLES;
    else E.sample_count++;
    E.sample_t[slot] = local_ms;
    E.sample_off[slot] = server_ms - local_ms;
    while (E.sample_count > 1 && E.sample_t[E.sample_head] < local_ms - 2000.0) {
        E.sample_head = (E.sample_head + 1) % CLOCK_SAMPLES;
        E.sample_count--;
    }
    double best = E.sample_off[E.sample_head];
    for (int i = 1; i < E.sample_count; i++) best = fmax(best, E.sample_off[(E.sample_head + i) % CLOCK_SAMPLES]);
    if (!E.clock_synced) {
        E.clock_offset = best;
        E.clock_synced = 1;
    } else if (best - E.clock_offset > 250.0 || E.clock_offset - best > 250.0) {
        E.clock_offset = best;
    } else {
        E.clock_offset += (best - E.clock_offset) * 0.1;
    }
}

// ---------------------------------------------------------------- meshes

// updateVisuals' meshRoot / pivots (:214673-214684): each part's mesh at the entity's render pose plus its part pose
// (the collision boxes stand at the committed pose: the shared manager's placeBody)
static void meshes_place(Client *client, const KsEntityView *v) {
    const KsEntityPose *r = &v->render;
    const float rsy = (float) sin(r->yaw), rcy = (float) cos(r->yaw);
    for (int k = 0; k < v->part_count; k++) {
        Object *o = ks_entities_part_object(&client->game, v->eid, k);
        Mesh *mesh = o ? o->mesh : NULL;
        KsEntityPose pp;
        double size[3];
        if (!mesh || !ks_entities_part_pose(&client->game, v->eid, k, &pp, size, NULL)) continue;
        const float rx = (float) pp.x * rcy + (float) pp.z * rsy, rz = -(float) pp.x * rsy + (float) pp.z * rcy;
        mesh->transform.position = (vec3) {(float) r->x + rx, (float) (r->y + pp.y), (float) r->z + rz};
        mesh->transform.rotation = (vec3) {(float) (r->pitch + pp.pitch), (float) (r->yaw + pp.yaw), (float) (r->roll + pp.roll)};
        mesh->transform.scale = (vec3) {(float) (size[0] > 0.0 ? size[0] : 0.01), (float) (size[1] > 0.0 ? size[1] : 0.01),
                                        (float) (size[2] > 0.0 ? size[2] : 0.01)};
    }
}

int entities_player_ride_yaw(const int uid, float *yaw) {
    int eid = -1, soft = 0;
    KsEntityView v;
    if (!E.game || !ks_entities_attachment(E.game, uid, &eid, &soft, NULL, NULL, NULL) || soft || !ks_entities_find(E.game, eid, &v)) return 0;
    if (yaw) *yaw = (float) v.render.yaw;
    return 1;
}

void entities_ride_look(Client *client, float *yaw) {
    const Player *me = client->me;
    int eid = -1, soft = 0, lk = 0;
    KsEntityView v;
    if (!me || !yaw || !ks_entities_attachment(&client->game, (int) me->uid, &eid, &soft, NULL, &lk, NULL) || !ks_entities_find(&client->game, eid, &v)) {
        E.look_eid = -1; // entLookEid = undefined
        return;
    }
    const double y = v.render.yaw; // (renderPose || ent).yaw
    if (E.look_eid != eid) {
        E.look_eid = eid;
        E.look_yaw = y;
    }
    double d = y - E.look_yaw;
    E.look_yaw = y;
    if (d > M_PI) d -= 2.0 * M_PI;
    else if (d < -M_PI) d += 2.0 * M_PI;
    if (!soft && lk) *yaw = (float) fmod(y, 2.0 * M_PI); // lk: the controls' yaw is the entity's (lockYaw: no mouse yaw)
    else if (d) *yaw += (float) d;                        // otherwise the look turns with it
}

// game.moveObject(uid, x, y, z): only a dyn object; y is its centre; a rider on top moves with it
static void object_moved(Client *client, const NetObjectMovedPacket *m) {
    Map *map = client->game.ready ? client->game.map : NULL;
    E.moves++;
    if (!map || m->uid < 0 || (size_t) m->uid >= map->object_count) return;
    Object *o = map->objects[m->uid];
    if (!o || !o->dyn) return;
    const vec3 s = object_box_size(o);
    const vec3 to = {m->x, m->y - s.y * 0.5f, m->z};
    const vec3 d = {to.x - o->position.x, to.y - o->position.y, to.z - o->position.z};
    const int rider = me_on_top(client, o);
    o->position = to;
    if (o->mesh) ((Mesh *) o->mesh)->transform.position = to;
    if (rider) carry_me(client, d);
}

// ---------------------------------------------------------------- dispatch

// NetExtJsonChunk assembly: the whole record when its last slice arrived (NULL meanwhile)
static cJSON *json_record(const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (length < sizeof(NetExtJsonChunk)) return NULL;
    NetExtJsonChunk head;
    memcpy(&head, body, sizeof(head));
    const uint8_t *data = body + sizeof(head);
    const size_t n = length - sizeof(head);
    if (!head.parts || head.part >= head.parts) return NULL;
    if (head.parts == 1) return cJSON_ParseWithLength((const char *) data, n);
    if (head.part == 0 || E.chunk_subtype != subtype || head.part != E.chunk_next) { // a new record (or a broken one: restart)
        free(E.chunk);
        E.chunk = NULL;
        E.chunk_len = 0;
        if (head.part != 0) return NULL;
        E.chunk_subtype = subtype;
        E.chunk_parts = head.parts;
    }
    char *grown = realloc(E.chunk, E.chunk_len + n);
    if (!grown) return NULL;
    E.chunk = grown;
    memcpy(E.chunk + E.chunk_len, data, n);
    E.chunk_len += n;
    E.chunk_next = head.part + 1;
    if (E.chunk_next < E.chunk_parts) return NULL;
    cJSON *json = cJSON_ParseWithLength(E.chunk, E.chunk_len);
    free(E.chunk);
    E.chunk = NULL;
    E.chunk_len = 0;
    E.chunk_next = 0;
    return json;
}

int entities_on_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    switch (subtype) {
        case NET_EXT_PLAYER_POSE: // "pz" players.setPose (player_pose_render.c)
            player_pose_on_ext(client, body, length);
            return 1;
        case NET_EXT_PLAYER_LOCKS: {
            NetPlayerLocksPacket p;
            if (length < sizeof(p)) return 1;
            memcpy(&p, body, sizeof(p));
            if (client->me) {
                client->me->lock_shooting = p.disable_shooting ? 1 : 0;
                client->me->lock_melee = p.disable_melee ? 1 : 0;
            }
            return 1;
        }
        case NET_EXT_KN_OFFICIAL_OBJECT_MOVED: {
            NetObjectMovedPacket m;
            if (length < sizeof(m)) return 1;
            memcpy(&m, body, sizeof(m));
            object_moved(client, &m);
            return 1;
        }
        case NET_EXT_ENTITY_SPAWN:
        case NET_EXT_ENTITY_REMOVE:
        case NET_EXT_ENTITY_STATE:
        case NET_EXT_ENTITY_CONTROLLED:
        case NET_EXT_ENTITY_EVENT:
        case NET_EXT_ENTITY_ATTACH:
        case NET_EXT_ENTITY_DETACH:
        case NET_EXT_ENTITY_SEED:
        case NET_EXT_KN_OFFICIAL_LIVE_OBJECT:
        case NET_EXT_KN_OFFICIAL_LIVE_SYNC:
            break;
        default:
            return 0;
    }
    cJSON *rec = json_record(subtype, body, length);
    if (!rec) return 1;
    if (client->game.ready && !game_is_authority(&client->game)) {
        Game *game = &client->game;
        E.game = game;
        switch (subtype) {
            case NET_EXT_ENTITY_SPAWN: E.spawns++; ks_entities_net_spawn(game, rec); break; // applySpawn: the body too
            case NET_EXT_ENTITY_REMOVE: E.removes++; ks_entities_net_remove(game, rec); break;
            case NET_EXT_ENTITY_STATE: // a baseline: the entity clock's sample is its tick (:261724-261731)
                E.states++;
                if (cJSON_IsArray(rec) && cJSON_IsNumber(cJSON_GetArrayItem(rec, 1)))
                    clock_sample(cJSON_GetArrayItem(rec, 1)->valuedouble * 33.333333333333336, E.local_ms);
                ks_entities_net_baseline(game, rec, E.local_ms);
                break;
            case NET_EXT_ENTITY_CONTROLLED: ks_entities_net_controlled(game, rec); break; // our "l"'s tail
            case NET_EXT_ENTITY_EVENT: E.events++; ks_entities_net_event(game, rec); break;
            case NET_EXT_ENTITY_ATTACH:
                E.attaches++;
                ks_entities_net_attach(game, rec, client->me ? (int) client->me->uid : -1);
                break;
            case NET_EXT_ENTITY_DETACH: E.attaches++; ks_entities_net_detach(game, rec); break;
            case NET_EXT_ENTITY_SEED: ks_entities_net_seed(game, rec); break;
            case NET_EXT_KN_OFFICIAL_LIVE_OBJECT: { // syncNewObjects: an asset's GLB joins the scene (map_post adds the carrier)
                E.lives++;
                ks_entities_net_live(game, rec);
                const Object *o = cJSON_IsArray(rec) ? ks_entities_live_object(game, (int) cJSON_GetNumberValue(cJSON_GetArrayItem(rec, 0))) : NULL;
                if (o && o->mesh) user_asset_object_added(o->mesh);
                break;
            }
            case NET_EXT_KN_OFFICIAL_LIVE_SYNC: ks_entities_net_live_sync(game, rec); break;
            default: break;
        }
    }
    cJSON_Delete(rec);
    return 1;
}

void entities_update(Client *client, const float delta) {
    E.local_ms += (double) delta * 1000.0;
    if (!client->game.ready || game_is_authority(&client->game)) return;
    E.game = &client->game;
    // game.update's client half (:222187-222216) once the entity clock has a sample
    if (E.clock_synced) ks_entities_client_frame(&client->game, E.local_ms + E.clock_offset, E.local_ms, (double) delta * 1000.0);
}

void entities_frame(Client *client, const float delta) {
    (void) delta;
    if (!client->game.ready || game_is_authority(&client->game)) return;
    const int n = ks_entities_count(&client->game);
    for (int i = 0; i < n; i++) {
        KsEntityView v;
        if (ks_entities_view(&client->game, i, &v)) meshes_place(client, &v);
    }
}

void entities_describe(const Client *client, char *out, const size_t size) {
    int parts = 0, solid = 0, attached = -1, soft = 0;
    const int count = ks_entities_count(&client->game);
    KsEntityView first, v;
    const int has = count > 0 && ks_entities_view(&client->game, 0, &first);
    for (int i = 0; i < count; i++) {
        if (!ks_entities_view(&client->game, i, &v)) continue;
        for (int k = 0; k < v.part_count; k++) {
            if (!ks_entities_part_object(&client->game, v.eid, k)) continue;
            parts++;
            solid += v.arch->coll > 0;
        }
    }
    if (client->me && ks_entities_attachment(&client->game, (int) client->me->uid, &attached, &soft, NULL, NULL, NULL) && soft) attached = -1;
    snprintf(out, size, "entities=%d parts=%d solid=%d live=%d spawns=%u states=%u removes=%u events=%u attaches=%u lives=%u moves=%u "
                        "attached=%d first=%d@%.2f,%.2f,%.2f/%.2f",
             count, parts, solid, ks_entities_live_count(&client->game), E.spawns, E.states, E.removes, E.events, E.attaches, E.lives, E.moves,
             attached, has ? first.eid : -1, has ? first.pose.x : 0.0, has ? first.pose.y : 0.0, has ? first.pose.z : 0.0, has ? first.pose.yaw : 0.0);
}
