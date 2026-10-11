// Krunker v3.1.1 AI visuals, see client/include/ai_render.h. References:
//   src/data/ai.js      AI.init (RENDER: genObj3D + loadMesh conf), AI.update (RENDER branch:
//                       bob, rotation.y = xD, map.offset.x = (tFrame % frames) / frames)
//   src/libs/render.js  loadMesh / getMat: StrippedLambertMaterial, emissive 0xffffff with the
//                       "<src>_e" emissiveMap (glowText), map.repeat.x = 1 / frames; materials
//                       are cached by their options, so AIs of one kind (umid only when
//                       scale > 4) share the texture frame: the last AI whose tFrame changed sets it
//   src/libs/particles.js physObj "orb_0": transparent, depthWrite false, AdditiveBlending,
//                       DoubleSide, loadMesh(conf, 0, 0, 0, PI / 2, 1), lookAt + rotateX(spin)
//   src/index.js        gameLoop nameVisRate pass: ai.canBSeen = spectating || canSee == null
// User-asset AIs (aid) draw the ghost: the native client has no user assets.
#include <ai.h>
#include <ai_render.h>
#include <spectate.h>
#include <audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_RENDER_MAX GAME_AI_MAX
#define AI_ORB_MAX 64
#define AI_ORB_SPIN 0.04f        // stored[1..4].spn
#define AI_VIS_RATE_S 0.2f       // config.nameVisRate

typedef struct {
    int sid;
    unsigned serial;             // GameAI.serial the mesh was built for, 0 = free slot
    char key[48];                // getMat cache key: AIs sharing it share the texture frame
    int conf_frames;             // map.repeat.x = 1 / conf_frames (0: no repeat)
    int last_frame;
    MeshTransform container;     // genObj3D: position, rotation.y = xD
    Mesh *mesh;
} AIMesh;

typedef struct {
    char key[48];
    float offset;                // three.js map.offset.x
} AIFrameKey;

typedef struct {
    int active;
    int server_id;
    vec3 position;
    vec3 velocity;               // per ms
    float speed, distance, roll;
    float scale, length;
    MeshTransform container;
    Mesh *mesh;
} AIOrb;

static struct {
    Scene *scene;                // the world scene the meshes were added to
    const Map *map;
    AIMesh meshes[AI_RENDER_MAX];
    AIFrameKey keys[AI_RENDER_MAX];
    int key_count;
    AIOrb orbs[AI_ORB_MAX];
    float vis_timer;
} g_air;

// projectiles.stored[kind] scale / length (projectile.js)
static const float k_orb_scale[GAME_AI_PROJECTILE_KINDS] = {1.0f, 6.1f, 4.0f, 22.0f, 3.0f};

static unsigned int air_texture(const char *src) {
    char path[96];
    snprintf(path, sizeof(path), "textures/%s.png", src);
    char *full = concat(assets_path(), path);
    const unsigned int texture = full ? load_texture(full) : 0;
    free(full);
    return texture;
}

static Geometry *air_model(const char *src) {
    char path[96];
    snprintf(path, sizeof(path), "models/%s.obj", src);
    char *full = concat(assets_path(), path);
    Geometry *geometry = full ? load_obj_model(full, 0) : NULL; // cached by the loader
    free(full);
    return geometry;
}

static void air_drop_mesh(AIMesh *slot) {
    if (slot->mesh) {
        if (g_air.scene) scene_remove_mesh(g_air.scene, slot->mesh);
        mesh_fini(slot->mesh); // frees the material; geometry / textures stay in the loader caches
    }
    memset(slot, 0, sizeof(*slot));
}

static void air_drop_orb(AIOrb *orb) {
    if (orb->mesh) {
        if (g_air.scene) scene_remove_mesh(g_air.scene, orb->mesh);
        mesh_fini(orb->mesh);
    }
    memset(orb, 0, sizeof(*orb));
}

void ai_render_reset(Client *client) {
    (void) client;
    for (int i = 0; i < AI_RENDER_MAX; i++) air_drop_mesh(&g_air.meshes[i]);
    for (int i = 0; i < AI_ORB_MAX; i++) air_drop_orb(&g_air.orbs[i]);
    g_air.key_count = 0;
    g_air.map = NULL;
}

// The mesh conf of ai.js init: dat ? {side 2, umid, frames, emissive (ghost_0), src} : c[index].obj
static void air_build(Client *client, AIMesh *slot, const GameAI *ai) {
    const GameAIData *data = &ai->data;
    const char *src = ai->has_data || !data->src[0] ? "ghost_0" : data->src; // dat: assetID ? asset : "ghost_0"
    const int conf_frames = ai->has_data ? ai->frames : data->frames;
    const int glow = ai->has_data ? !strcmp(src, "ghost_0") : data->glow;

    Geometry *geometry = air_model(src);
    if (!geometry) return;
    BasicMaterial *material = basic_material_init(); // StrippedLambertMaterial: lit, fogged
    if (!material) return;
    material->texture = air_texture(src);
    material->double_sided = 1; // side: 2
    if (glow) {
        char emissive[64];
        snprintf(emissive, sizeof(emissive), "%s_e", src);
        material->emissive = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
        material->emissive_texture = air_texture(emissive);
    }
    if (conf_frames) material->texture_repeat.x = 1.0f / (float) conf_frames;

    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return;
    }

    slot->sid = ai->sid;
    slot->serial = ai->serial;
    slot->conf_frames = conf_frames;
    slot->last_frame = 0;
    if (ai->has_data && ai->scale > 4.0f) snprintf(slot->key, sizeof(slot->key), "%s|%d|u%d", src, conf_frames, ai->sid);
    else snprintf(slot->key, sizeof(slot->key), "%s|%d|%d", src, conf_frames, ai->has_data ? -1 : ai->index);

    memset(&slot->container, 0, sizeof(slot->container));
    slot->container.scale = (vec3) {1.0f, 1.0f, 1.0f};
    mesh->transform.parent = &slot->container;
    mesh->transform.position = (vec3) {0.0f, ai->scale, 0.0f}; // loadMesh(conf, 0, scale, 0, 0, scale)
    mesh->transform.scale = (vec3) {ai->scale, ai->scale, ai->scale};
    mesh->visible = 0;
    slot->mesh = mesh;
    scene_add_mesh(client->scene, mesh);
}

static AIMesh *air_slot_for(const GameAI *ai) {
    for (int i = 0; i < AI_RENDER_MAX; i++) {
        if (g_air.meshes[i].serial == ai->serial && g_air.meshes[i].sid == ai->sid) return &g_air.meshes[i];
    }
    return NULL;
}

static AIFrameKey *air_key(const char *key) {
    for (int i = 0; i < g_air.key_count; i++) {
        if (!strcmp(g_air.keys[i].key, key)) return &g_air.keys[i];
    }
    if (g_air.key_count >= AI_RENDER_MAX) return NULL;
    AIFrameKey *entry = &g_air.keys[g_air.key_count++];
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->offset = 0.0f;
    return entry;
}

static int air_visible(const Client *client, const GameAI *ai) {
    if (!ai->active) return 0;
    if (!game_is_authority(&client->game)) return ai->shown; // syncAll: in the last "ai" list
    // the in-process game: what AI.sync would send this player (in range, or spectating)
    const Player *me = client->me;
    if (spectate_active() || !me) return 1;
    const float dx = me->position.x - ai->position.x, dy = me->position.y - ai->position.y, dz = me->position.z - ai->position.z;
    return sqrtf(dx * dx + dy * dy + dz * dz) <= GAME_AI_SYNC_RANGE;
}

static void air_update_meshes(Client *client, const float dt_ms) {
    GameAIs *list = client->game.ai;

    // meshes whose AI is gone (reset, new round)
    for (int i = 0; i < AI_RENDER_MAX; i++) {
        AIMesh *slot = &g_air.meshes[i];
        if (!slot->serial) continue;
        const GameAI *ai = list ? game_ai_find(&client->game, slot->sid) : NULL;
        if (!ai || ai->serial != slot->serial) air_drop_mesh(slot);
    }
    if (!list) return;

    for (int i = 0; i < list->count; i++) {
        GameAI *ai = &list->ais[i];
        AIMesh *slot = air_slot_for(ai);
        if (!slot) {
            for (int k = 0; k < AI_RENDER_MAX && !slot; k++) {
                if (!g_air.meshes[k].serial) slot = &g_air.meshes[k];
            }
            if (!slot) continue;
            air_build(client, slot, ai);
            if (!slot->mesh) {
                memset(slot, 0, sizeof(*slot));
                continue;
            }
        }

        const int visible = air_visible(client, ai);
        slot->mesh->visible = visible;
        ai->shown = (unsigned char) visible;
        if (!visible) continue; // update() returns early for hidden meshes

        if (ai->bob_d && ai->bob_s) ai->bob_val += dt_ms * ai->bob_s;
        slot->container.position = (vec3) {ai->position.x, ai->position.y + sinf(ai->bob_val) * ai->bob_d, ai->position.z};
        slot->container.rotation.y = ai->x_dir;

        // idleSound: every idlSoundI * randFloat(0.8, 1.2) ms, Game.playerSound(idl, 0.2 (the sid
        // slot), x, y, z, 1, 1, false, true, true) -> play3D at the AI, volume 1, asset sound
        if (ai->data.idle_sound) {
            ai->idle_timer -= dt_ms;
            if (ai->idle_timer <= 0.0f) {
                ai->idle_timer = 5000.0f * (0.8f + 0.4f * (float) rand() / (float) RAND_MAX);
                char name[32];
                snprintf(name, sizeof(name), "asset:%d", ai->data.idle_sound);
                audio_play_3d(name, ai->position, 1.0f, 1.0f, 0.0f, 0.0f);
            }
        }

        if (ai->frames > 1 && ai->frame != slot->last_frame) {
            slot->last_frame = ai->frame;
            AIFrameKey *key = air_key(slot->key);
            if (key) key->offset = (float) (ai->frame % ai->frames) / (float) ai->frames;
        }
    }

    // the shared materials' map.offset.x, in KrunkNative's repeat units (offset / repeat)
    for (int i = 0; i < AI_RENDER_MAX; i++) {
        AIMesh *slot = &g_air.meshes[i];
        if (!slot->mesh) continue;
        const AIFrameKey *key = air_key(slot->key);
        BasicMaterial *material = (BasicMaterial *) slot->mesh->material;
        material->texture_offset.x = key ? key->offset * (slot->conf_frames ? (float) slot->conf_frames : 1.0f) : 0.0f;
    }
}

// gameLoop (every config.nameVisRate): canBSeen for the nametags
static void air_update_sight(Client *client, const float delta) {
    GameAIs *list = client->game.ai;
    g_air.vis_timer -= delta;
    if (!list || g_air.vis_timer > 0.0f) return;
    g_air.vis_timer = AI_VIS_RATE_S;
    const int spectating = spectate_active();
    const Player *me = client->me;
    for (int i = 0; i < list->count; i++) {
        GameAI *ai = &list->ais[i];
        if (!ai->active) continue;
        ai->can_be_seen = (unsigned char) (spectating || (me && game_ai_player_can_see(&client->game, me, ai->position)));
    }
}

// lookAt(position + velocity) then the accumulated rotateX(spin) (YXZ like projectiles_render.c)
static void air_orb_pose(AIOrb *orb) {
    const float length = sqrtf(orb->velocity.x * orb->velocity.x + orb->velocity.y * orb->velocity.y + orb->velocity.z * orb->velocity.z);
    const vec3 f = length > 1e-6f ? (vec3) {orb->velocity.x / length, orb->velocity.y / length, orb->velocity.z / length} : (vec3) {0.0f, 0.0f, 1.0f};
    orb->container.position = orb->position;
    orb->container.rotation.y = atan2f(f.x, f.z);
    orb->container.rotation.x = -asinf(fmaxf(-1.0f, fminf(1.0f, f.y))) + orb->roll;
    orb->container.rotation.z = 0.0f;
}

static void air_update_orbs(const float dt_ms) {
    for (int i = 0; i < AI_ORB_MAX; i++) {
        AIOrb *orb = &g_air.orbs[i];
        if (!orb->active) continue;
        orb->position.x += orb->velocity.x * dt_ms;
        orb->position.y += orb->velocity.y * dt_ms;
        orb->position.z += orb->velocity.z * dt_ms;
        orb->roll += AI_ORB_SPIN * dt_ms;
        orb->distance -= orb->speed * dt_ms;
        if (orb->distance <= 0.0f) orb->active = 0;
        if (orb->mesh) {
            orb->mesh->visible = orb->active;
            air_orb_pose(orb);
        }
    }
}

void ai_render_projectile(Client *client, const NetProjectilePacket *packet) {
    if (!packet->ai_kind || packet->ai_kind >= GAME_AI_PROJECTILE_KINDS || !client->scene) return;
    if (g_air.scene && g_air.scene != client->scene) ai_render_reset(client);
    g_air.scene = client->scene;

    AIOrb *orb = NULL;
    for (int i = 0; i < AI_ORB_MAX && !orb; i++) {
        if (!g_air.orbs[i].active) orb = &g_air.orbs[i];
    }
    if (!orb) return;

    if (!orb->mesh) { // physObjs are pooled per src: build the orb mesh once per slot
        Geometry *geometry = air_model("orb_0");
        BasicMaterial *material = geometry ? basic_material_init() : NULL;
        if (!material) return;
        material->texture = air_texture("orb_0");
        material->base.transparent = 1;
        material->additive_blend = 1;  // blending: 2
        material->no_depth_write = 1;  // depthWrite: false
        material->double_sided = 1;    // side: 2
        Mesh *mesh = mesh_init(geometry, (Material *) material);
        if (!mesh) {
            material_fini((Material *) material);
            return;
        }
        memset(&orb->container, 0, sizeof(orb->container));
        orb->container.rotation_order = ROTATION_ORDER_YXZ;
        mesh->transform.parent = &orb->container;
        mesh->transform.rotation.y = (float) M_PI / 2.0f; // loadMesh(conf, 0, 0, 0, PI / 2, 1)
        mesh->transform.scale = (vec3) {1.0f, 1.0f, 1.0f};
        orb->mesh = mesh;
        scene_add_mesh(client->scene, mesh);
    }

    const float scale = k_orb_scale[packet->ai_kind];
    orb->active = 1;
    orb->server_id = packet->id;
    orb->position = (vec3) {packet->position[0], packet->position[1], packet->position[2]};
    orb->velocity = (vec3) {packet->velocity[0], packet->velocity[1], packet->velocity[2]};
    orb->speed = sqrtf(orb->velocity.x * orb->velocity.x + orb->velocity.y * orb->velocity.y + orb->velocity.z * orb->velocity.z);
    orb->distance = packet->range;
    orb->roll = 0.0f;
    orb->container.scale = (vec3) {scale, scale, scale}; // scaleMesh(scale, scale, length)
    orb->mesh->visible = 1;
    air_orb_pose(orb);
}

void ai_render_projectile_end(Client *client, const NetProjectileEndPacket *packet) {
    (void) client;
    for (int i = 0; i < AI_ORB_MAX; i++) {
        AIOrb *orb = &g_air.orbs[i];
        if (!orb->active || orb->server_id != packet->id) continue;
        orb->active = 0; // disablePhys: not ours, no linger
        if (orb->mesh) orb->mesh->visible = 0;
    }
}

void ai_render_tick(Client *client, const float delta) {
    const Map *map = client->game.ready ? client->game.map : NULL;
    if (map != g_air.map || (g_air.scene && g_air.scene != client->scene)) {
        ai_render_reset(client);
        g_air.map = map;
    }
    g_air.scene = client->scene;
    if (!map || !client->scene) return;

    const float dt_ms = fminf(fmaxf(delta, 0.0f), 0.25f) * 1000.0f;
    air_update_meshes(client, dt_ms);
    air_update_sight(client, delta);
    air_update_orbs(dt_ms);
}
