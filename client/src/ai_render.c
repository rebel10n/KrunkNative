// Krunker v7.1.4 AI visuals, see client/include/ai_render.h. References:
//   src/data/AI.js       AI.init (RENDER: genObj3D + loadMesh / loadAnimMesh with the asset conf:
//                        fileExt by the g / q / f letters, emissive "e", alphaTest "a", side, opacity,
//                        noFog, frames / frameT, rAnim), mesh y = (assetID ? 0 : scale) + mYOff,
//                        rotation y = mROff, scale; AI.update (RENDER: interpolation over
//                        1000 / aiSyncT ms, rotation.y = xD, playAnim(didMov ? "Move" : "Idle"),
//                        idle sound); AI.syncAll (bob, forcePos); servAnim ("ana")
//   src/libs/render.js   loadMesh: an asset OBJ is re-centred on its bounding box centre y; the
//                        material's texture strip (frames) steps every frameT ms; playClip
//   src/libs/particles.js physObj of an AI shot: "orb_0" (or the pjA asset), scale * pjMS (pjSC *
//                        pjMS for assets), spin * pjS, transparent / additive like stored[1]
//   src/index.js         gameLoop nameVisRate pass: ai.canBSeen = spectating || canSee == null
// Intentional differences: FBX AI models ("f") are not supported; a built-in kind whose v7 user
// asset is missing (Drowned Sailor, Jack O' Lantern, Mirror Image: not in the offline build) draws
// the v3.1.1 model of that kind (ghost / pumpkin) instead of nothing; map AIs with a missing asset
// draw nothing, as in v7. Side 1 (back faces only) draws both sides.
#include <ai.h>
#include <ai_render.h>
#include <gltf.h>
#include <spectate.h>
#include <audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_RENDER_MAX GAME_AI_MAX
#define AI_ORB_MAX 64
#define AI_ORB_SPIN 0.04f        // stored[1].spn
#define AI_ORB_SCALE 6.1f        // stored[1].scale / length
#define AI_VIS_RATE_S 0.2f       // config.nameVisRate
#define AI_ONESHOT_FALLBACK 1.0  // s a one-shot clip blocks Idle / Move without gltf_instance_clip_running

typedef struct {
    int sid;
    unsigned serial;             // GameAI.serial the mesh was built for, 0 = free slot
    MeshTransform container;     // genObj3D: position, rotation.y = xD
    MeshTransform pivot;         // loadMesh(conf, 0, y offset, 0, mROff, scale): offset, rotation, scale, rAnim spin
    Mesh *mesh;                  // OBJ model (one primitive)
    GltfInstance *gltf;          // glTF / GLB model (loadAnimMesh)
    int frames;                  // texture strip frames (0: none)
    float frame_time;            // ms per frame (0: the synced v3.1.1 tFrame)
    unsigned clip_serial;        // the last GameAI.clip_serial played
    double oneshot_until;        // gltf_clock() until which a one-shot clip blocks the loop clips
    float spin;                  // rAnim accumulated (rad)
} AIMesh;

typedef struct {
    int active;
    int server_id;
    vec3 position;
    vec3 velocity;               // per ms
    float speed, distance, roll, spin;
    MeshTransform container;
    Mesh *mesh;
} AIOrb;

static struct {
    Scene *scene;                // the world scene the meshes were added to
    const Map *map;
    AIMesh meshes[AI_RENDER_MAX];
    AIOrb orbs[AI_ORB_MAX];
    float vis_timer;
} g_air;

// v3.1.1 projectiles.stored[kind] scale (projectile.js); v7 shots are kind 1 (stored[1]).
static const float k_orb_scale[GAME_AI_PROJECTILE_KINDS] = {1.0f, AI_ORB_SCALE, 4.0f, 22.0f, 3.0f};

static unsigned int air_texture(const char *rel) {
    char *full = concat(assets_path(), rel);
    const unsigned int texture = full ? load_texture(full) : 0;
    free(full);
    return texture;
}

static Geometry *air_obj(const char *rel) {
    char *full = concat(assets_path(), rel);
    Geometry *geometry = full ? load_obj_model(full, 0) : NULL; // cached by the loader
    free(full);
    return geometry && geometry->index_count > 0 ? geometry : NULL;
}

static void air_drop_mesh(AIMesh *slot) {
    if (slot->mesh) {
        if (g_air.scene) scene_remove_mesh(g_air.scene, slot->mesh);
        mesh_fini(slot->mesh); // frees the material; geometry / textures stay in the loader caches
    }
    if (slot->gltf) {
        if (g_air.scene) scene_remove_gltf(g_air.scene, slot->gltf);
        gltf_instance_fini(slot->gltf);
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
    g_air.map = NULL;
}

// The per-AI material options of the AI.js conf (side, opacity, alphaTest, noFog, emissive).
static void air_material(BasicMaterial *m, const GameAIData *d, const int emissive) {
    m->double_sided = d->side != 0;
    if (d->alpha_test > 0.0f) m->alpha_test = d->alpha_test;
    if (strchr(d->asset_text, 'a') && m->alpha_test < 0.02f) m->alpha_test = 0.02f;
    if (d->f_trans || d->opacity != 1.0f) {
        m->base.transparent = 1;
        m->color.w = d->opacity;
    }
    if (d->no_fog) m->fog_enabled = 0;
    if (emissive) m->emissive = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
}

// The asset conf of AI.init: "user-assets/<id>/model.<obj|gltf|glb>" (+ model.png / model_e.png).
static int air_load_asset(AIMesh *slot, const GameAI *ai, float *center_y) {
    const GameAIData *d = &ai->data;
    const char *flags = d->asset_text;
    char rel[96];
    if (strchr(flags, 'g') || strchr(flags, 'q')) {
        snprintf(rel, sizeof(rel), "user-assets/%d/model.%s", d->asset, strchr(flags, 'q') ? "glb" : "gltf");
        char *path = concat(assets_path(), rel);
        GltfModel *model = path ? gltf_model_load(path) : NULL;
        free(path);
        slot->gltf = model ? gltf_instance_create(model, d->anim_speed ? d->anim_speed : 1.0f) : NULL;
        if (!slot->gltf) return 0;
        for (size_t i = 0; i < slot->gltf->mesh_count; i++) {
            Material *mat = slot->gltf->meshes[i]->material;
            if (mat && mat->vtable == &basic_material_vtable) air_material((BasicMaterial *) mat, d, 0);
        }
        // startAnim ? playClip(startAnim) : every clip (gltf_instance_create's default)
        gltf_instance_play_clip(slot->gltf, d->start_anim[0] ? d->start_anim : "Idle", 0);
        return 1;
    }
    if (strchr(flags, 'f')) return 0; // FBX
    snprintf(rel, sizeof(rel), "user-assets/%d/model.obj", d->asset);
    Geometry *geometry = air_obj(rel);
    if (!geometry) return 0;
    BasicMaterial *material = basic_material_init();
    if (!material) return 0;
    snprintf(rel, sizeof(rel), "user-assets/%d/model.png", d->asset);
    material->texture = air_texture(rel);
    const int emissive = strchr(flags, 'e') != NULL;
    air_material(material, d, emissive);
    if (emissive) {
        snprintf(rel, sizeof(rel), "user-assets/%d/model_e.png", d->asset);
        material->emissive_texture = air_texture(rel);
    }
    slot->mesh = mesh_init(geometry, (Material *) material);
    if (!slot->mesh) {
        material_fini((Material *) material);
        return 0;
    }
    *center_y = geometry->bound_center[1]; // loadMesh: geometry.translate(0, -boundingBox centre y, 0)
    return 1;
}

// A stock model (assets/models/<src>.obj + textures/<src>.png, glow: "<src>_e" emissiveMap).
static int air_load_stock(AIMesh *slot, const GameAI *ai, const char *src, const int glow) {
    char rel[96];
    snprintf(rel, sizeof(rel), "models/%s.obj", src);
    Geometry *geometry = air_obj(rel);
    if (!geometry) return 0;
    BasicMaterial *material = basic_material_init(); // StrippedLambertMaterial: lit, fogged
    if (!material) return 0;
    snprintf(rel, sizeof(rel), "textures/%s.png", src);
    material->texture = air_texture(rel);
    air_material(material, &ai->data, glow);
    if (glow) {
        snprintf(rel, sizeof(rel), "textures/%s_e.png", src);
        material->emissive_texture = air_texture(rel);
    }
    slot->mesh = mesh_init(geometry, (Material *) material);
    if (!slot->mesh) {
        material_fini((Material *) material);
        return 0;
    }
    return 1;
}

static void air_build(Client *client, AIMesh *slot, const GameAI *ai) {
    const GameAIData *d = &ai->data;
    memset(slot, 0, sizeof(*slot));
    slot->sid = ai->sid;
    slot->serial = ai->serial;
    slot->clip_serial = ai->clip_serial;
    if (d->no_vis) return; // noVis: no mesh

    float center_y = 0.0f;
    int asset = 0, ok = 0;
    if (d->asset) {
        ok = asset = air_load_asset(slot, ai, &center_y);
        // the built-in kinds' v7 assets (AI.js user assets 30171e, 50838e, 50578q) are not in the
        // v7.1.4 tree: tools/ui-import/fetch_live_assets.py installs them from Krunker's live
        // asset host; without them the v3.1.1 models stand in
        if (!ok && ai->index >= 1 && ai->index <= 3 && !strcmp(d->src, "model")) {
            ok = air_load_stock(slot, ai, ai->index == 2 ? "pumpkin_0" : "ghost_0", 1);
        }
    } else {
        ok = air_load_stock(slot, ai, "ghost_0", d->glow);
    }
    if (!ok) return;

    slot->frames = d->frames > 1 ? d->frames : 0;
    slot->frame_time = d->frame_time;
    if (slot->mesh && slot->frames) ((BasicMaterial *) slot->mesh->material)->texture_repeat.x = 1.0f / (float) slot->frames;

    memset(&slot->container, 0, sizeof(slot->container));
    slot->container.scale = (vec3) {1.0f, 1.0f, 1.0f};
    memset(&slot->pivot, 0, sizeof(slot->pivot));
    slot->pivot.parent = &slot->container;
    const float scale = ai->scale;
    slot->pivot.position = (vec3) {0.0f, (asset ? 0.0f : scale) + d->m_y_off - center_y * scale, 0.0f};
    slot->pivot.rotation.y = d->m_r_off * (float) M_PI / 180.0f;
    slot->pivot.scale = (vec3) {scale, scale, scale};
    if (slot->mesh) {
        slot->mesh->transform.parent = &slot->pivot;
        slot->mesh->transform.scale = (vec3) {1.0f, 1.0f, 1.0f};
        slot->mesh->visible = 0;
        scene_add_mesh(client->scene, slot->mesh);
    } else if (slot->gltf) {
        slot->gltf->root.parent = &slot->pivot;
        gltf_instance_set_visible(slot->gltf, 0);
        scene_add_gltf(client->scene, slot->gltf);
    }
}

static AIMesh *air_slot_for(const GameAI *ai) {
    for (int i = 0; i < AI_RENDER_MAX; i++) {
        if (g_air.meshes[i].serial == ai->serial && g_air.meshes[i].sid == ai->sid) return &g_air.meshes[i];
    }
    return NULL;
}

static int air_visible(const Client *client, const GameAI *ai) {
    if (!ai->active) return 0;
    // v7 AI.sync only walks players.list (active or spectating players): a connection in the menu
    // that never spawned gets no "aai" / "ai", so the map's AIs are not drawn behind the menu. The
    // native server also streams AIs to menu connections (NET_TARGET_NON_PLAYERS); keep them
    // hidden until we spawn.
    if (!game_is_authority(&client->game)) return (client->me || spectate_active()) && ai->shown; // syncAll: in the last "ai" list
    // the in-process game: what AI.sync would send this player (active: AIs within 800; spectating: all)
    if (spectate_active()) return 1;
    const Player *me = client->me;
    if (!me || !me->active) return ai->shown;
    const float dx = me->position.x - ai->position.x, dy = me->position.y - ai->position.y, dz = me->position.z - ai->position.z;
    return sqrtf(dx * dx + dy * dy + dz * dz) <= GAME_AI_SYNC_RANGE;
}

// AI.update (RENDER): the mesh at the interpolated position (mirrors) or the AI's own (in-process).
static vec3 air_draw_position(const Client *client, const GameAI *ai) {
    if (game_is_authority(&client->game) || !ai->interpolate) return ai->position;
    const float t = fminf(1.6f, ai->interp_t / (1000.0f * GAME_AI_SYNC_INTERVAL));
    return (vec3) {ai->from.x + (ai->position.x - ai->from.x) * t, ai->from.y + (ai->position.y - ai->from.y) * t,
                   ai->from.z + (ai->position.z - ai->from.z) * t};
}

static void air_clips(AIMesh *slot, GameAI *ai, const double now) {
    if (!slot->gltf || !ai->data.has_anim) return;
    if (ai->clip_serial != slot->clip_serial) { // "ana" / playAnim(ai, clip, 1): once
        slot->clip_serial = ai->clip_serial;
        if (ai->clip[0]) {
            gltf_instance_play_clip(slot->gltf, ai->clip, 1);
            slot->oneshot_until = now + AI_ONESHOT_FALLBACK;
        }
    }
#ifdef GLTF_HAVE_CLIP_RUNNING
    const int busy = gltf_instance_clip_running(slot->gltf);
#else
    const int busy = now < slot->oneshot_until;
#endif
    // playClip(name, 0) waits for a running one-shot
    if (!busy) gltf_instance_play_clip(slot->gltf, ai->did_move ? "Move" : "Idle", 0);
    gltf_instance_update(slot->gltf, now);
}

static void air_update_meshes(Client *client, const float dt_ms) {
    GameAIs *list = client->game.ai;
    const double now = gltf_clock();

    // meshes whose AI is gone (reset, new round, "rai")
    for (int i = 0; i < AI_RENDER_MAX; i++) {
        AIMesh *slot = &g_air.meshes[i];
        if (!slot->serial) continue;
        const GameAI *ai = list ? game_ai_find(&client->game, slot->sid) : NULL;
        if (!ai || ai->serial != slot->serial) air_drop_mesh(slot);
    }
    if (!list) return;
    if (!game_is_authority(&client->game)) game_ai_client_update(&client->game, dt_ms);

    for (int i = 0; i < list->count; i++) {
        GameAI *ai = &list->ais[i];
        if (ai->removed) continue;
        AIMesh *slot = air_slot_for(ai);
        if (!slot) {
            for (int k = 0; k < AI_RENDER_MAX && !slot; k++) {
                if (!g_air.meshes[k].serial) slot = &g_air.meshes[k];
            }
            if (!slot) continue;
            air_build(client, slot, ai);
        }

        const int visible = air_visible(client, ai) && (slot->mesh || slot->gltf);
        if (slot->mesh) slot->mesh->visible = visible;
        if (slot->gltf) gltf_instance_set_visible(slot->gltf, visible);
        ai->shown = (unsigned char) air_visible(client, ai);
        if (!visible) continue; // update() returns early for hidden meshes

        if (ai->bob_d && ai->bob_s) ai->bob_val += dt_ms * ai->bob_s;
        const vec3 at = air_draw_position(client, ai);
        // in-process: syncAll's y + sin(bobVal) * bobD happens here; mirrors got it in game_ai_apply_sync_v7
        const float bob = game_is_authority(&client->game) || !ai->interpolate ? sinf(ai->bob_val) * ai->bob_d : 0.0f;
        slot->container.position = (vec3) {at.x, at.y + bob, at.z};
        slot->container.rotation.y = ai->x_dir;
        if (ai->data.r_anim) { // rAnim: spin about y (or x) per ms
            slot->spin += ai->data.r_anim * dt_ms;
            if (ai->data.r_anim_axis) slot->pivot.rotation.x = slot->spin;
            else slot->pivot.rotation.y = ai->data.m_r_off * (float) M_PI / 180.0f + slot->spin;
        }

        // idleSound: every idlSoundI * randFloat(0.8, 1.2) ms, Game.playerSound(idl, ..., soundR)
        if (ai->data.idle_sound) {
            ai->idle_timer -= dt_ms;
            if (ai->idle_timer <= 0.0f) {
                ai->idle_timer = 5000.0f * (0.8f + 0.4f * (float) rand() / (float) RAND_MAX);
                char name[32];
                snprintf(name, sizeof(name), "asset:%d", ai->data.idle_sound);
                audio_play_3d(name, ai->position, 1.0f, ai->data.sound_range ? ai->data.sound_range : 1.0f, 0.0f, 0.0f);
            }
        }

        // the texture strip: every frameT ms (v7 frameMats), or the synced v3.1.1 tFrame
        if (slot->mesh && slot->frames) {
            const int frame = slot->frame_time > 0.0f ? (int) fmod(now * 1000.0 / slot->frame_time, slot->frames) : ai->frame % slot->frames;
            ((BasicMaterial *) slot->mesh->material)->texture_offset.x = (float) frame;
        }
        air_clips(slot, ai, now);
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
        orb->roll += orb->spin * dt_ms;
        orb->distance -= orb->speed * dt_ms;
        if (orb->distance <= 0.0f) orb->active = 0;
        if (orb->mesh) {
            orb->mesh->visible = orb->active;
            air_orb_pose(orb);
        }
    }
}

// particles.js physObj of an AI shot with the shooter's projOb: scale * pjMS, spin * pjS.
static void air_orb_style(AIOrb *orb, const GameAI *shooter) {
    if (!orb || !shooter) return;
    const GameAIData *d = &shooter->data;
    const float ms = d->pj_model_scale ? d->pj_model_scale : 1.0f;
    const float scale = d->pj_asset[0] ? (d->pj_scale ? d->pj_scale : 1.0f) * ms : AI_ORB_SCALE * ms;
    orb->container.scale = (vec3) {scale, scale, scale};
    orb->spin = AI_ORB_SPIN * d->pj_spin;
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
        Geometry *geometry = air_obj("models/orb_0.obj");
        BasicMaterial *material = geometry ? basic_material_init() : NULL;
        if (!material) return;
        material->texture = air_texture("textures/orb_0.png");
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
    orb->spin = AI_ORB_SPIN;
    orb->container.scale = (vec3) {scale, scale, scale}; // scaleMesh(scale, scale, length)
    orb->mesh->visible = 1;
    // the in-process game knows the shooter of the slot (servers send NET_PACKET_AI_SHOT instead)
    const GameAIs *list = client->game.ai;
    if (game_is_authority(&client->game) && list && packet->id >= 0 && packet->id < GAME_MAX_PROJECTILES && list->shots[packet->id].used) {
        air_orb_style(orb, game_ai_find(&client->game, list->shots[packet->id].sid));
    }
    air_orb_pose(orb);
}

void ai_render_ai_shot(Client *client, const NetAIShotPacket *packet) {
    const GameAI *shooter = game_ai_find(&client->game, packet->sid);
    for (int i = 0; i < AI_ORB_MAX && shooter; i++) {
        AIOrb *orb = &g_air.orbs[i];
        if (orb->active && orb->server_id == packet->projectile) air_orb_style(orb, shooter);
    }
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
