// Krunker v10.0.3 class models, see client/include/player_class_model.h and doc/v10/class-models.md. Line numbers:
// deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js).
//
// players.generateMeshes (:194228-194266), for a class whose config.mdl is set:
//   - mdl 1 Sprite: RENDER.loadSprite({src: digits ? digits + "/texture" : "sprite_0", SpriteMaterial, depthWrite,
//     transparent, alphaTest 0.02, asset: digits != null}, objInstances): a camera-facing 11 x 11 sprite at y 5.5
//     (:234089-234099; digits = parseInt(aID without its letters) || null)
//   - mdl 2 Model: RENDER[g / q / f ? "loadAnimMesh" : "loadMesh"]({isMdl, fileExt q ? glb : g ? gltf : f ? fbx,
//     src: digits + "/model", emissive e ? #ffffff, normal n, spec s, asset: digits, startAnim g / q / f ? "Idle",
//     animS}, 0, (anim ? 0 : 5.5) + (mdlYO || 0) + (preview && mdPY || 1), 0, rad(mdlDr || 0),
//     (mdlS || 1) * (preview && mdPS || 1), objInstances): a user asset (user_asset.c), an OBJ re-centred on its
//     bounding box's y (loadMesh :233140-233144), a GLTF / GLB with its own materials playing "Idle"
//   - the asset text is player.assetTxt (a "uai" swap) || config.aID; assetTxt is nulled after the build (:194265)
//   - the whole build sits in `if (config.mdlFP || IÍïíiií && !config.mdlFP)`: our own first-person model of a
//     class with mdlFP off has no model at all, not even the default body and its viewmodel arms (:194228)
//   - nothing of the default body is built: no arms, weapons, cosmetics; the pet still follows (:194849-194852)
// players.updateMsh (:193662-193681): no step bob for any mdl, no step sway for mdl 2, no turn for a sprite.
// players.update (:191184-191187): every frame an interpolated player or our own plays crouchVal ? "Crouch" :
// moved > 0.1 ? "Move" : "Idle" (playClip with no reps: the looping main clip, which waits for a one-shot).
// Vehicle.mov (:19145-19152): meshRef.rotation x (mdlDr % 180 != 0, order YXZ) or z (order ZYX) = angVel * vhtL.
//
// Deliberate differences:
// - An asset this build cannot draw (an FBX "f" model, as KrunkNative has no FBX loader; a model file missing or
//   broken) draws the default body (default_body): v10 draws nothing for it (its loader fails), which here would
//   leave the player invisible for good. While the file downloads nothing is drawn, as in v10, and the model is
//   rebuilt once it lands (player_class_model_stale).
// - A "uai" / player.assetID swap lasts until the player dies (client/src/net/game_events.c), not until the next model
//   build: v10 nulls assetTxt in generateMeshes, so any later regenMeshes (an items or loadout update) brings the
//   class's aID back, while KrunkNative also rebuilds models for reasons of its own (a skin download, the Stalker
//   look, the third-person toggle). A swap that reaches a dead player waits for its spawn, as in v10.
// - A client-side player.assetID (KrunkScript) is drawn at the next model refresh (main.c, every 30 frames), not in
//   the same frame; "uai" from the server rebuilds at once (s2c_events.c).
// - Not drawn: the normal / specular companion maps of the n / s letters (BasicMaterial has none for user assets);
//   the OBJ's emissive map is (user_asset_load).
#include <gl.h>
#include <player_class_model.h>
#include <player_anim.h>  // player_class_has_anims
#include <game_config.h>  // class_overrides
#include <user_asset.h>
#include <pets.h>         // g_pet_world_scene: the world scene
#include <net_ext.h>      // NET_PLAYER_ASSET_TEXT
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLASS_SPRITE_Y 5.5f     // loadSprite: position (0, 5.5, 0)
#define CLASS_SPRITE_SCALE 11.0f // loadSprite: scale (iïiîïîì 11, :21182)
#define CLASS_OBJ_Y 5.5f        // generateMeshes: a loadMesh model sits 5.5 higher than a loadAnimMesh one
#define CLASS_MOVE_STEP 0.1f    // players.update: dist3(old, now) > 0.1 plays "Move"

static const ClassOverrides *class_of(const Player *player) {
    const Game *game = player ? player->game : NULL;
    if (!game || !game->classes || player->class_index < 0 || (size_t) player->class_index >= game->class_count) return &g_default_class_overrides;
    return class_overrides(&game->classes[player->class_index]);
}

// The asset text the build reads: assetTxt || config.aID || "" (:194230-194231), "" when the class has no model
static void class_asset_text(const Player *player, char *out, const size_t n) {
    const ClassOverrides *o = class_of(player);
    out[0] = 0;
    if (!o->model) return;
    snprintf(out, n, "%s", player->asset_txt[0] ? player->asset_txt : o->asset_id);
}

int player_class_model_replaces_body(const PlayerClassModel *m) {
    return m && !m->default_body;
}

// ---------------------------------------------------------------- the sprite (mdl 1)

static Geometry *g_sprite_geometry; // THREE.Sprite's XY unit quad (basic.vert's sprite path faces it to the camera)

static Geometry *class_sprite_geometry(void) {
    if (g_sprite_geometry) return g_sprite_geometry;
    static const vertex vertices[] = {{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}}, {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}},
                                      {{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}}, {{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}}};
    static const unsigned int indices[] = {0, 1, 2, 0, 3, 1};
    Geometry *g = calloc(1, sizeof(Geometry));
    if (!g) return NULL;
    unsigned int vbo;
    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &g->ebo);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    g->index_count = 6;
    g->bound_radius = 0.0f; // never culled (the sprite is laid out in view space)
    g_sprite_geometry = g;
    return g;
}

// A texture by assets-relative path: the loader's placeholder while it downloads, the checker when it does not exist
static unsigned int class_texture(const char *rel) {
    char *path = concat(assets_path(), rel);
    if (!path) return 0;
    FILE *f = ensure_asset_file(path) ? fopen(path, "rb") : NULL;
    unsigned int texture;
    if (f) {
        fclose(f);
        texture = load_texture(path);
    } else {
        texture = asset_prefetch(path) == 0 ? load_texture(path) : texture_checker();
    }
    free(path);
    return texture;
}

static int class_build_sprite(PlayerClassModel *m, const char *asset) {
    // parseInt(asset.replace(/[^\d]/g, "")) || null: the digits as a number, 0 and none are "sprite_0"
    char digits[24];
    size_t n = 0;
    for (const char *p = asset; *p && n + 1 < sizeof(digits); p++)
        if (*p >= '0' && *p <= '9') digits[n++] = *p;
    digits[n] = 0;
    const long id = n ? strtol(digits, NULL, 10) : 0;
    char rel[64];
    if (id > 0) snprintf(rel, sizeof(rel), "user-assets/%ld/texture.png", id); // asset: the user-asset host's <id>/texture
    else snprintf(rel, sizeof(rel), "textures/sprite_0.png");
    Geometry *geometry = class_sprite_geometry();
    BasicMaterial *material = geometry ? basic_material_init() : NULL;
    if (!material) return 0;
    m->mesh = mesh_init(geometry, (Material *) material);
    if (!m->mesh) {
        material_fini((Material *) material);
        return 0;
    }
    // SpriteMaterial: unlit, colour #ffffff, transparent, alphaTest 0.02, depthWrite true, fog on
    material->sprite = 1;
    material->unlit = 1;
    material->base.transparent = 1;
    material->alpha_test = 0.02f;
    material->double_sided = 1;
    material->texture = class_texture(rel);
    m->mesh->transform.parent = &m->holder;
    m->holder.position = (vec3) {0.0f, CLASS_SPRITE_Y, 0.0f};
    m->holder.scale = (vec3) {CLASS_SPRITE_SCALE, CLASS_SPRITE_SCALE, 1.0f};
    return 1;
}

// ---------------------------------------------------------------- the model (mdl 2)

// gltf_instance_play_clip, noting the clip when it took (a known name that started or changed the playing clip)
static void class_play(PlayerClassModel *m, const char *clip, const int reps) {
    GltfInstance *g = m->gltf;
    if (!g || !clip) return;
    const int mode = g->clip_mode, index = g->clip;
    const double start = g->clip_start;
    gltf_instance_play_clip(g, clip, reps);
    if (g->clip_mode != mode || g->clip != index || g->clip_start != start) snprintf(m->clip, sizeof(m->clip), "%s", clip);
}

// 1 built, 0 nothing to draw yet (m->pending: the file downloads), -1 this build cannot draw it (the default body)
static int class_build_model(PlayerClassModel *m, const Player *player, const ClassOverrides *o, const char *asset) {
    UserAssetFlags flags;
    if (!user_asset_parse(asset, &flags)) return -1; // no digits: src "/model" loads nothing
    // the conf of generateMeshes reads only g / q / f / e / n / s: no alphaTest ("a"), no lightMap ("l")
    flags.alpha = flags.lightmap = 0;
    const int anim = flags.gltf || flags.glb || flags.fbx;
    if (flags.gltf || flags.glb) flags.fbx = 0; // fileExt: q ? glb : g ? gltf : f ? fbx
    if (flags.fbx) return -1;                  // KrunkNative has no FBX loader
    char rel[96];
    snprintf(rel, sizeof(rel), "user-assets/%s/model.%s", flags.id, flags.glb ? "glb" : flags.gltf ? "gltf" : "obj");
    char *path = concat(assets_path(), rel);
    if (!path) return -1;
    const int state = asset_prefetch(path); // 1 on disk, 0 downloading, -1 failed
    if (state == 0) {
        snprintf(m->pending, sizeof(m->pending), "%s", path); // loadAnimMesh / loadMesh: nothing until it loads
        free(path);
        return 0;
    }
    free(path);
    if (state < 0) return -1;

    UserAssetModel model;
    if (!user_asset_load(&flags, &model)) return -1;
    const int preview = player->uid == -1; // the menu / viewer model (generateMeshes' third argument)
    const float y = (anim ? 0.0f : CLASS_OBJ_Y) + o->model_y_offset + (preview && o->model_pos_y != 0.0f ? o->model_pos_y : 1.0f);
    const float scale = (o->model_scale != 0.0f ? o->model_scale : 1.0f) * (preview && o->model_pos_scale != 0.0f ? o->model_pos_scale : 1.0f);
    m->holder.position = (vec3) {0.0f, y, 0.0f};
    m->holder.rotation = (vec3) {0.0f, o->model_dir * (float) M_PI / 180.0f, 0.0f};
    m->holder.scale = (vec3) {scale, scale, scale};

    if (model.gltf) {
        m->gltf = gltf_instance_create(model.gltf, o->anim_speed != 0.0f ? o->anim_speed : 1.0f); // mixer.timeScale = animS || 1
        if (!m->gltf) return -1;
        m->gltf->root.parent = &m->holder;
        for (size_t i = 0; i < m->gltf->mesh_count; i++) {
            Material *mat = m->gltf->meshes[i]->material;
            if (!mat || mat->vtable != &basic_material_vtable) continue;
            // the model setup (:233655-233663): no env and no texEncode, so every map is encodingTypes[0] (raw)
            BasicMaterial *b = (BasicMaterial *) mat;
            b->texture_srgb = b->emissive_srgb = 0;
            b->gltf_srgb_maps = 0;
        }
        // startAnim "Idle" (:194257): playClip("Idle") alone, or no clip at all when the model has none
        gltf_instance_stop_clips(m->gltf);
        class_play(m, "Idle", 0);
        return 1;
    }
    m->mesh = model.mesh;
    m->mesh->transform.parent = &m->holder;
    m->mesh->transform.position.y = -m->mesh->geometry->bound_center[1]; // geometry.translate(0, -boundingBox centre y, 0)
    return 1;
}

// ---------------------------------------------------------------- build / free

// "uai" swaps that came before their player (player_class_model_hold_asset)
#define CLASS_HELD_MAX 16
static struct {
    int uid;
    char asset[NET_PLAYER_ASSET_TEXT];
} g_held[CLASS_HELD_MAX];
static int g_held_count;

void player_class_model_hold_asset(const int uid, const char *asset_txt) {
    if (!asset_txt || !asset_txt[0]) return;
    int slot = 0;
    while (slot < g_held_count && g_held[slot].uid != uid) slot++;
    if (slot == g_held_count) {
        if (g_held_count < CLASS_HELD_MAX) g_held_count++;
        else slot = (int) ((unsigned) uid % CLASS_HELD_MAX); // full: an old one goes
    }
    g_held[slot].uid = uid;
    snprintf(g_held[slot].asset, sizeof(g_held[slot].asset), "%s", asset_txt);
}

void player_class_model_reset(void) {
    g_held_count = 0;
}

static void class_take_held(Player *player) {
    for (int i = 0; i < g_held_count; i++) {
        if (g_held[i].uid != player->uid) continue;
        player_class_model_set_asset(player, g_held[i].asset); // updateAssetID's own test (config.mdl)
        g_held[i] = g_held[--g_held_count];
        return;
    }
}

PlayerClassModel *player_class_model_create(Player *player, const int render_you, const int third_person) {
    if (!player || !player->game) return NULL;
    if (g_held_count && player->uid != -1) class_take_held(player);
    const ClassOverrides *o = class_of(player);
    // IÍïíiií (:194099) also counts the class's own camera offset (config.camOffX / Y / Z)
    const int class_camera = o->cam_offset[0] != 0.0f || o->cam_offset[1] != 0.0f || o->cam_offset[2] != 0.0f;
    const int shown = o->model_fp || third_person || class_camera || !render_you;
    if (shown && !o->model) return NULL; // the default body

    PlayerClassModel *m = calloc(1, sizeof(PlayerClassModel));
    if (!m) return NULL;
    m->holder.scale = (vec3) {1.0f, 1.0f, 1.0f};
    m->mdl = shown ? o->model : 0;
    m->class_mdl = o->model;
    class_asset_text(player, m->asset, sizeof(m->asset));
    m->has_anims = (unsigned char) player_class_has_anims(player);
    if (!shown) return m; // mdlFP off in first person: nothing at all

    int built = 0;
    if (o->model == 1) built = class_build_sprite(m, m->asset) ? 1 : -1;
    else if (o->model == 2) built = class_build_model(m, player, o, m->asset);
    else built = -1; // an mdl past "Model": none of v10's branches; the default body
    if (built < 0) {
        if (m->mesh) mesh_fini(m->mesh);
        if (m->gltf) gltf_instance_fini(m->gltf);
        m->mesh = NULL;
        m->gltf = NULL;
        m->default_body = 1;
    }
    return m;
}

void player_class_model_free(PlayerClassModel *m) {
    if (!m) return;
    if (m->world && g_pet_world_scene) scene_remove_class_model(g_pet_world_scene, m, 1);
    if (m->mesh) mesh_fini(m->mesh); // its material; the geometry / textures stay in the loader caches
    if (m->gltf) gltf_instance_fini(m->gltf);
    free(m);
}

// ---------------------------------------------------------------- scene

static void class_scene_add(Scene *scene, PlayerClassModel *m) {
    if (m->mesh) scene_add_mesh(scene, m->mesh);
    scene_add_gltf(scene, m->gltf);
}

static void class_scene_remove(Scene *scene, PlayerClassModel *m) {
    if (m->mesh) scene_remove_mesh(scene, m->mesh);
    scene_remove_gltf(scene, m->gltf);
}

void scene_add_class_model(Scene *scene, PlayerClassModel *m, const int render_you) {
    if (!m || !scene) return;
    if (render_you && g_pet_world_scene) { // drawn with the world: once, whichever scene the model goes to
        if (m->world) return;
        class_scene_add(g_pet_world_scene, m);
        m->world = 1;
        return;
    }
    class_scene_add(scene, m);
}

void scene_remove_class_model(Scene *scene, PlayerClassModel *m, const int render_you) {
    if (!m) return;
    if (m->world) {
        if (render_you && g_pet_world_scene) class_scene_remove(g_pet_world_scene, m);
        m->world = 0;
        return;
    }
    if (scene) class_scene_remove(scene, m);
}

// ---------------------------------------------------------------- per frame

void player_class_model_update(Player *player, PlayerClassModel *m, MeshTransform *anchor, const float step_yaw) {
    if (!player_class_model_replaces_body(m)) return;
    // updateMsh: IÎìîìíî (the step bob) is 0 for any mdl (:193662-193664); objInstances turns by xDire for mdl 2 with
    // no step sway (IÏíiïîí 0, :193668-193670) and not at all for a sprite (:193673)
    if (anchor) {
        anchor->position = player->position;
        if (m->mdl == 2) anchor->rotation.y -= step_yaw;
        else anchor->rotation.y = 0.0f;
    }
    if (!m->gltf) return;

    // Vehicle.mov (:19145-19152): the model leans with the turn, about x when mdlDr is not a multiple of 180 (Euler
    // YXZ), else about z (Euler ZYX, which with rotation.x 0 and rotation.y 0 or PI is XYZ with z * cos(y))
    if (player_movement_scheme(player) == MOVEMENT_SCHEME_VEHICLE) {
        const ClassOverrides *o = class_of(player);
        const float lean = player->scheme.ang_vel * o->vehicle_turn_lean;
        const float dir = o->model_dir * (float) M_PI / 180.0f;
        if (fmodf(o->model_dir, 180.0f) != 0.0f) {
            m->holder.rotation_order = ROTATION_ORDER_YXZ;
            m->holder.rotation = (vec3) {lean, dir, 0.0f};
        } else {
            m->holder.rotation_order = ROTATION_ORDER_INTRINSIC;
            m->holder.rotation = (vec3) {0.0f, dir, lean * cosf(dir)};
        }
    }

    // players.update (:191184-191187): the frame's own move picks the looping clip
    const vec3 p = player->position;
    float moved = 0.0f;
    if (m->has_last) {
        const float dx = p.x - m->last_position.x, dy = p.y - m->last_position.y, dz = p.z - m->last_position.z;
        moved = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    m->last_position = p;
    m->has_last = 1;
    if (player->uid != -1 && player->active && m->has_anims)
        class_play(m, player->crouch_val != 0.0f ? "Crouch" : moved > CLASS_MOVE_STEP ? "Move" : "Idle", 0);
    gltf_instance_update(m->gltf, gltf_clock());
}

// ---------------------------------------------------------------- clips, swaps

GltfInstance *player_class_model_gltf(const Player *player) {
    const PlayerMesh *pm = player ? player->mesh : NULL;
    const PlayerClassModel *m = pm ? pm->class_model : NULL;
    return player_class_model_replaces_body(m) ? m->gltf : NULL;
}

static PlayerClassModel *class_drawn(const Player *player) {
    const PlayerMesh *pm = player ? player->mesh : NULL;
    PlayerClassModel *m = pm ? pm->class_model : NULL;
    return player_class_model_replaces_body(m) && m->gltf ? m : NULL;
}

void player_class_model_play_clip(Player *player, const char *clip, const float reps) {
    PlayerClassModel *m = class_drawn(player);
    // playClip: `player.active && player.hasAnims`, then `meshRef && meshRef.visible && meshRef.playClip`
    if (!clip || !m || !player->active || !m->has_anims) return;
    // setLoop(LoopRepeat, reps == -1 ? Infinity : reps): a fraction plays one more pass
    class_play(m, clip, reps < 0.0f ? 1 << 30 : reps > 0.0f ? (int) ceilf(reps) : 0);
}

void player_class_model_script_clip(const Player *player, const char *clip, const int reps) {
    PlayerClassModel *m = class_drawn(player);
    if (!m) return;
    if (clip) {
        class_play(m, clip, reps < 0 ? 1 << 30 : reps); // -1: Infinity
    } else {
        gltf_instance_stop_clips(m->gltf); // stopAllClips: mainAnim / currAnim null
        m->clip[0] = 0;
    }
}

void player_class_model_describe(const Player *player, char *out, const size_t n) {
    const PlayerMesh *pm = player ? player->mesh : NULL;
    const PlayerClassModel *m = pm ? pm->class_model : NULL;
    if (!m) { // no mesh (a dead player): body=-
        snprintf(out, n, "body=%s mdl=0 asset=- clip=-%s", pm ? "default" : "-", pm && pm->stalker ? " stalker=1" : ""); // the Stalker look
        return;
    }
    const char *body = m->default_body ? "fallback" : m->mesh || m->gltf ? "model" : "none";
    snprintf(out, n, "body=%s mdl=%d asset=%s clip=%s%s", body, m->class_mdl, m->asset[0] ? m->asset : "-", m->clip[0] ? m->clip : "-",
             m->gltf && gltf_instance_clip_running(m->gltf) ? "*" : "");
}

int player_class_model_set_asset(Player *player, const char *asset_txt) {
    if (!player || !asset_txt || !asset_txt[0] || !class_of(player)->model) return 0;
    snprintf(player->asset_txt, sizeof(player->asset_txt), "%s", asset_txt);
    return 1;
}

int player_class_model_stale(const Player *player, const PlayerClassModel *m) {
    if (!player || !player->mesh) return 0;
    char want[sizeof(m->asset)];
    class_asset_text(player, want, sizeof(want));
    if (strcmp(want, m ? m->asset : "") != 0) return 1;
    if ((m ? m->class_mdl : 0) != class_of(player)->model) return 1; // the class's Player Type changed (a new config)
    return m && m->pending[0] && asset_prefetch(m->pending) != 0; // the file arrived (or failed: the default body)
}
