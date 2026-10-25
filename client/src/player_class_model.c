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
// Model builds (player_class_model.h): a build is v10's generateMeshes unless player_class_model_keep() marked it as
// KrunkNative's own rebuild; a generateMeshes through the model branch takes and clears player.assetTxt (:194265), so a
// "uai" swap lasts until the next generateMeshes, exactly as in v10: a spawn (players.add, main.c also for a player
// alive already), a loadout or prop change, another swap. A "uai" for a player this client has not added is dropped
// (findBySid, :257784); KrunkNative's server sends the swap of a spawn's onPlayerSpawn after that SPAWN (player_anim.c).
// The companion maps of the asset letters (applyAssetCompanionMaps, :231687-231752; getMat :232132): "n" model_n.png
// as normalMap (normalScale 1) and "s" model_s.png as specularMap with flipY false (no mapProps), "e" model_e.png as
// a GLTF material's emissiveMap. A GLTF's MeshStandardMaterial has no specularMap; an OBJ's takes the lighting
// setting's material (Phong: the specular strength), its emissive keeps loadTexture's flipped map.
// A missing or broken model or map draws nothing, as v10's loaders leave nothing; while a file downloads nothing is
// drawn and the model is rebuilt (kept) once it lands.
//
// Deliberate differences:
// - An FBX ("f") model draws the default body: KrunkNative has no FBX loader (doc/v10/class-models.md: ufbx was
//   weighed against AGENTS.md's dependency rule and not taken), and an invisible player would be worse.
// - A missing companion map is not bound (three samples an image-less texture black).
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

// A companion map (applyAssetCompanionMaps, deob-10.0.3.js:231711-231752): user-assets/<id>/model<suffix>.png, its
// placeholder while it downloads; 0 when there is none (not bound, see the differences above)
static unsigned int class_companion_map(const char *id, const char *suffix) {
    char rel[96];
    snprintf(rel, sizeof(rel), "user-assets/%s/model%s.png", id, suffix);
    char *path = concat(assets_path(), rel);
    if (!path) return 0;
    FILE *f = ensure_asset_file(path) ? fopen(path, "rb") : NULL;
    unsigned int texture = 0;
    if (f) {
        fclose(f);
        texture = load_texture(path);
    } else if (asset_prefetch(path) == 0) {
        texture = load_texture(path); // still downloading: the placeholder, filled when it lands
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

// 1 built, 0 nothing to draw (still downloading: m->pending; or never: missing, broken, no id), -1 the default body
static int class_build_model(PlayerClassModel *m, const Player *player, const ClassOverrides *o, const char *asset) {
    UserAssetFlags flags;
    if (!user_asset_parse(asset, &flags)) return 0; // no digits: src "/model" loads nothing
    // the conf of generateMeshes reads only g / q / f / e / n / s: no alphaTest ("a"), no lightMap ("l")
    flags.alpha = flags.lightmap = 0;
    const int anim = flags.gltf || flags.glb || flags.fbx;
    if (flags.gltf || flags.glb) flags.fbx = 0; // fileExt: q ? glb : g ? gltf : f ? fbx
    char rel[96];
    snprintf(rel, sizeof(rel), "user-assets/%s/model.%s", flags.id, flags.glb ? "glb" : flags.gltf ? "gltf" : flags.fbx ? "fbx" : "obj");
    char *path = concat(assets_path(), rel);
    if (!path) return 0;
    const int state = asset_prefetch(path); // 1 on disk, 0 downloading, -1 failed
    if (state == 0) snprintf(m->pending, sizeof(m->pending), "%s", path); // loadAnimMesh / loadMesh: nothing until it loads
    free(path);
    if (state <= 0) return 0; // downloading, or the loader's error: an empty meshRef

    UserAssetModel model;
    if (!user_asset_load(&flags, &model)) return 0;
    const int preview = player->uid == -1; // the menu / viewer model (generateMeshes' third argument)
    const float y = (anim ? 0.0f : CLASS_OBJ_Y) + o->model_y_offset + (preview && o->model_pos_y != 0.0f ? o->model_pos_y : 1.0f);
    const float scale = (o->model_scale != 0.0f ? o->model_scale : 1.0f) * (preview && o->model_pos_scale != 0.0f ? o->model_pos_scale : 1.0f);
    m->holder.position = (vec3) {0.0f, y, 0.0f};
    m->holder.rotation = (vec3) {0.0f, o->model_dir * (float) M_PI / 180.0f, 0.0f};
    m->holder.scale = (vec3) {scale, scale, scale};
    // the companion maps of "n" / "s" / "e" (the conf's normal, spec and emissive keys)
    const unsigned int normal_map = flags.normal ? class_companion_map(flags.id, "_n") : 0;
    const unsigned int specular_map = flags.specular ? class_companion_map(flags.id, "_s") : 0;

    if (model.gltf) {
        m->gltf = gltf_instance_create(model.gltf, o->anim_speed != 0.0f ? o->anim_speed : 1.0f); // mixer.timeScale = animS || 1
        if (!m->gltf) return 0;
        m->gltf->root.parent = &m->holder;
        const unsigned int emissive_map = flags.emissive ? class_companion_map(flags.id, "_e") : 0;
        for (size_t i = 0; i < m->gltf->mesh_count; i++) {
            Material *mat = m->gltf->meshes[i]->material;
            if (!mat || mat->vtable != &basic_material_vtable) continue;
            // the model setup (:233655-233663): no env and no texEncode, so every map is encodingTypes[0] (raw)
            BasicMaterial *b = (BasicMaterial *) mat;
            b->texture_srgb = b->emissive_srgb = 0;
            b->gltf_srgb_maps = 0;
            // applyAssetCompanionMaps on every MeshStandardMaterial (:233704): normalMap and emissiveMap replace the
            // GLTF's own (flipY false is the GLTF's orientation here too); it has no specularMap
            if (normal_map) {
                b->normal_texture = normal_map;
                b->normal_scale = 1.0f;
            }
            if (emissive_map) b->emissive_texture = emissive_map;
        }
        // startAnim "Idle" (:194257): playClip("Idle") alone, or no clip at all when the model has none
        gltf_instance_stop_clips(m->gltf);
        class_play(m, "Idle", 0);
        return 1;
    }
    m->mesh = model.mesh;
    m->mesh->transform.parent = &m->holder;
    m->mesh->transform.position.y = -m->mesh->geometry->bound_center[1]; // geometry.translate(0, -boundingBox centre y, 0)
    // getMat's material (the lighting setting's): normalMap and specularMap from applyAssetCompanionMaps, flipY false
    // over the OBJ's flipped base map (sampled at 1 - v)
    BasicMaterial *b = (BasicMaterial *) m->mesh->material;
    if (normal_map) {
        b->normal_texture = normal_map;
        b->normal_scale = 1.0f;
    }
    b->specular_texture = specular_map;
    b->maps_flip_v = normal_map || specular_map;
    return 1;
}

// ---------------------------------------------------------------- build / free

// player_class_model_keep: the next build of that player keeps the model's asset and handle
#define CLASS_KEEP_MAX 16
static struct {
    int uid;
    char asset[32];
    unsigned char from_swap;
    unsigned serial;
} g_keep[CLASS_KEEP_MAX];
static int g_keep_count;
static unsigned g_serial;

void player_class_model_keep(const Player *player) {
    const PlayerMesh *pm = player ? player->mesh : NULL;
    const PlayerClassModel *m = pm ? pm->class_model : NULL;
    if (!m || player->uid == -1) return; // no model: the next build is the same either way
    int slot = 0;
    while (slot < g_keep_count && g_keep[slot].uid != player->uid) slot++;
    if (slot == g_keep_count) {
        if (g_keep_count < CLASS_KEEP_MAX) g_keep_count++;
        else slot = (int) ((unsigned) player->uid % CLASS_KEEP_MAX);
    }
    g_keep[slot].uid = player->uid;
    snprintf(g_keep[slot].asset, sizeof(g_keep[slot].asset), "%s", m->asset);
    g_keep[slot].from_swap = m->from_swap;
    g_keep[slot].serial = m->serial;
}

void player_class_model_reset(void) {
    g_keep_count = 0;
}

static int class_take_keep(const Player *player, char *asset, const size_t n, unsigned char *from_swap, unsigned *serial) {
    for (int i = 0; i < g_keep_count; i++) {
        if (g_keep[i].uid != player->uid) continue;
        snprintf(asset, n, "%s", g_keep[i].asset);
        *from_swap = g_keep[i].from_swap;
        *serial = g_keep[i].serial;
        g_keep[i] = g_keep[--g_keep_count];
        return 1;
    }
    return 0;
}

PlayerClassModel *player_class_model_create(Player *player, const int render_you, const int third_person) {
    if (!player || !player->game) return NULL;
    const ClassOverrides *o = class_of(player);
    // KrunkNative's own rebuild (player_class_model_keep): the asset and handle the model had
    char kept[32] = "";
    unsigned char kept_swap = 0;
    unsigned kept_serial = 0;
    const int keep = player->uid != -1 && class_take_keep(player, kept, sizeof(kept), &kept_swap, &kept_serial);
    // IÍïíiií (:194099) also counts the class's own camera offset (config.camOffX / Y / Z)
    const int class_camera = o->cam_offset[0] != 0.0f || o->cam_offset[1] != 0.0f || o->cam_offset[2] != 0.0f;
    const int shown = o->model_fp || third_person || class_camera || !render_you;
    if (shown && !o->model) return NULL; // the default body

    PlayerClassModel *m = calloc(1, sizeof(PlayerClassModel));
    if (!m) return NULL;
    m->holder.scale = (vec3) {1.0f, 1.0f, 1.0f};
    m->mdl = shown ? o->model : 0;
    m->class_mdl = o->model;
    m->has_anims = (unsigned char) player_class_has_anims(player);
    m->serial = keep && kept_serial ? kept_serial : ++g_serial;
    if (o->model) {
        if (keep) { // not a generateMeshes: a waiting assetTxt stays waiting
            snprintf(m->asset, sizeof(m->asset), "%s", kept[0] || kept_swap ? kept : o->asset_id);
            m->from_swap = kept_swap;
        } else if (shown && player->asset_txt[0]) { // IIíììiî = assetTxt || aID, then assetTxt = null (:194230, :194265)
            snprintf(m->asset, sizeof(m->asset), "%s", player->asset_txt);
            m->from_swap = 1;
            player->asset_txt[0] = 0;
        } else {
            snprintf(m->asset, sizeof(m->asset), "%s", o->asset_id);
        }
    }
    if (!shown) return m; // mdlFP off in first person: nothing at all (assetTxt waits)

    int built = 0;
    if (o->model == 1) built = class_build_sprite(m, m->asset);
    else if (o->model == 2) built = class_build_model(m, player, o, m->asset);
    // an mdl past "Model" (2): neither of v10's branches builds anything, and the default branch is skipped
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
    ks_scene_class_model_freed(m); // getAsset's handle loses it (ks_scene.c)
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
    // Vehicle.mov (:19145-19152), `if (meshRef)`: a Model's mesh (loadAnimMesh or loadMesh; a sprite has no meshRef)
    // leans by angVel * vhtL, angVel as that mov found it (MovementSchemeState.lean_ang_vel). mdlDr % 180 != 0: Euler
    // "YXZ" and rotation.x; else Euler "ZYX" and rotation.z, which with rotation.x 0 and a yaw of 0 or 180 degrees is
    // the default XYZ with z * cos(yaw). Only where mov runs for the player: our own (replays included); the others'
    // models never lean on a client, as in v10 (their angVel only reaches their owner, SYNC_PLAYER's sendVals).
    if (m->mdl == 2 && player_movement_scheme(player) == MOVEMENT_SCHEME_VEHICLE) {
        const ClassOverrides *o = class_of(player);
        const float lean = player->scheme.lean_ang_vel * o->vehicle_turn_lean;
        const float dir = o->model_dir * (float) M_PI / 180.0f;
        if (fmodf(o->model_dir, 180.0f) != 0.0f) {
            m->holder.rotation_order = ROTATION_ORDER_YXZ;
            m->holder.rotation = (vec3) {lean, dir, 0.0f};
        } else {
            m->holder.rotation_order = ROTATION_ORDER_INTRINSIC;
            m->holder.rotation = (vec3) {0.0f, dir, lean * cosf(dir)};
        }
    }
    if (!m->gltf) return;

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
    const size_t o = (size_t) snprintf(out, n, "body=%s mdl=%d asset=%s clip=%s%s%s", body, m->class_mdl, m->asset[0] ? m->asset : "-",
                                       m->clip[0] ? m->clip : "-", m->gltf && gltf_instance_clip_running(m->gltf) ? "*" : "",
                                       m->from_swap ? " swap=1" : "");
    size_t at = o < n ? o : n;
    // the companion maps bound (n normal, s specular, e emissive; flip: sampled at 1 - v)
    const Mesh *first = m->mesh ? m->mesh : m->gltf && m->gltf->mesh_count ? m->gltf->meshes[0] : NULL;
    const BasicMaterial *b = first && first->material && first->material->vtable == &basic_material_vtable ? (const BasicMaterial *) first->material : NULL;
    if (b && at < n && (b->normal_texture || b->specular_texture || b->emissive_texture)) {
        const int w = snprintf(out + at, n - at, " maps=%s%s%s%s", b->normal_texture ? "n" : "", b->specular_texture ? "s" : "",
                               b->emissive_texture ? "e" : "", b->maps_flip_v ? ",flip" : "");
        at = w > 0 && at + (size_t) w < n ? at + (size_t) w : n;
    }
    // the Vehicle lean (holder rotation x / z, YXZ or the default order)
    if (at < n && (m->holder.rotation.x != 0.0f || m->holder.rotation.z != 0.0f))
        snprintf(out + at, n - at, " lean=%.4f,%.4f%s", m->holder.rotation.x, m->holder.rotation.z, m->holder.rotation_order == ROTATION_ORDER_YXZ ? ",YXZ" : "");
}

int player_class_model_set_asset(Player *player, const char *asset_txt) {
    if (!player || !asset_txt || !asset_txt[0] || !class_of(player)->model) return 0;
    snprintf(player->asset_txt, sizeof(player->asset_txt), "%s", asset_txt);
    return 1;
}

int player_class_model_stale(const Player *player, const PlayerClassModel *m) {
    if (!player || !player->mesh) return 0;
    const ClassOverrides *o = class_of(player);
    // an assetTxt set with no regenMeshes after it yet (a client script's player.assetID: ks_client_host.c rebuilds at
    // once, this catches any other way): the generateMeshes that takes it
    if (player->active && o->model && player->asset_txt[0] && (!m || m->mdl)) return 1;
    // KrunkNative only: a model built before its class config came (GAME_CONFIG after the SPAWN) or under an older
    // one; v10's INIT carries the config before any player is added
    if ((m ? m->class_mdl : 0) != o->model) return 1;
    if (m && !m->from_swap && o->model && strcmp(m->asset, o->asset_id) != 0) return 1;
    // the file arrived (or failed) since the build: the same model again, with it
    if (m && m->pending[0] && asset_prefetch(m->pending) != 0) return 2;
    return 0;
}

int player_class_model_spawn_rebuild(const Player *player) {
    const PlayerMesh *pm = player ? player->mesh : NULL;
    const PlayerClassModel *m = pm ? pm->class_model : NULL;
    return (m && m->from_swap) || (player && player->asset_txt[0] && class_of(player)->model);
}

PlayerClassModel *player_class_model_find(const Game *game, const int uid, const unsigned serial) {
    for (size_t i = 0; game && i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p || p->uid != uid) continue;
        const PlayerMesh *pm = p->mesh;
        PlayerClassModel *m = pm ? pm->class_model : NULL;
        return player_class_model_replaces_body(m) && m->serial == serial ? m : NULL;
    }
    return NULL;
}
