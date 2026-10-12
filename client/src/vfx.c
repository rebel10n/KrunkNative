// Combat visual effects: a port of Krunker v3.1.1's particle manager and the
// effect spawns around it. References (Krunker.Unpacked-v3.1.1):
//   88.js  particle manager: sprites (add/effect/area), physObj boxes (tracers,
//          casings, projectiles), sniper trails, ExplosionManager debris
//   95.js  shoot(): muzzle sprites, screen flash, casings, tracers, impacts
//   91.js  overlay canvas: screen muzzle flash (img/muzflash.png), blood overlay
//   index.js g7/g8/gb: remote shots ("9"), projectile end ("pre"), explosions ("ex")
//   Projectiles themselves (physObj with a projectile config) live in projectiles_render.c;
//   their boxes are batched here with the tracers.
//   134.js settings: particles, particlesDist, showTrails, yourTrails, muzzleFlash,
//          bulletTracers, bulletTracerCol, useDamageOverlay
// Krunker simulates these in milliseconds; this file keeps its units (positions
// in world units, velocities per ms, gravity per ms^2, lifetimes in ms).
#include <gl.h>
#include <vfx.h>
#include <projectiles_render.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <render_settings.h>
#include <math.h>
#include <pcg_basic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VFX_TEXTURE_UNIT 0 // vfx.frag `tex`

#define VFX_MAX_SPRITES 1024
#define VFX_MAX_BOXES 256
#define VFX_MAX_TRAILS 32
#define VFX_MAX_DEBRIS 256
#define VFX_MAX_AREAS 64
#define VFX_MAX_DELTA_MS 100.0f

// Krunker shows the muzzle sprite and the overlay flash for exactly one rendered
// frame (Sprite life 1 ms / overlay `ai = 1`). At native frame rates far above
// the browser's that is invisible, so both stay up for at least one 60 Hz frame.
#define VFX_FLASH_MIN_MS (1000.0f / 60.0f)
// ...and never outlive this, even when no frame drew them (scoped, menu open)
#define VFX_FLASH_MAX_MS 100.0f

// 1.js
#define VFX_TRACER_MIN_DST 20.0f
#define VFX_TRACER_MAX_DST 500.0f
// 95.js: the hit point is pulled back 0.12 along the ray before effects spawn
#define VFX_HIT_PULLBACK 0.12f
// 95.js: scoped tracers start 7 units behind the muzzle
#define VFX_SCOPED_TRACER_BACK 7.0f
// index.js gb: ExplosionManager.explodeAt(x, y, z, range * 0.08)
#define VFX_EXPLOSION_SCALE 0.08f
// 91.js: the overlay muzzle flash is drawn 1.6 x canvas width wide
#define VFX_SCREEN_FLASH_SIZE 1.6f

// 91.js speed lines (`ae`, pool `ad` of 15, drawn first on the overlay canvas while the
// horizontal speed * 500 is at least 50). Units are the overlay canvas' logical pixels
// (canvas size / scaleBase's c7), lengths and speeds per ms.
#define VFX_SPEED_LINES 15
#define VFX_SPEED_LINE_MIN 50.0f
// Krunker shrinks alpha and size by life/startLife once per rendered frame; at a native
// frame rate above the browser's that would fade them faster, so the factor is applied
// per 60 Hz frame of elapsed time instead (identical at 60 fps).
#define VFX_SPEED_LINE_FRAME_MS (1000.0f / 60.0f)

typedef enum {
    VFX_BLEND_OPAQUE,
    VFX_BLEND_NORMAL,      // THREE.NormalBlending (1)
    VFX_BLEND_ADDITIVE,    // THREE.AdditiveBlending (2)
    VFX_BLEND_SUBTRACTIVE, // THREE.SubtractiveBlending (3)
} VfxBlend;

typedef enum {
    VFX_TEX_NONE = -1,
    VFX_TEX_PART_0,       // particles/0.png  impact smoke
    VFX_TEX_PART_1,       // particles/1.png  bullet hole (subtractive)
    VFX_TEX_PART_2,       // particles/2.png  muzzle flash (muzID default 2)
    VFX_TEX_PART_3,       // particles/3.png  alien/blaster muzzle (muzID 3)
    VFX_TEX_DEFAULT,      // particles/default.png
    VFX_TEX_GRASS,        // particles/grass_0.png
    VFX_TEX_FOG,          // particles/fog_0.png
    VFX_TEX_EMBER,        // particles/ember_0.png
    VFX_TEX_FEATHER,      // particles/feather.png (v7 item effect 5)
    VFX_TEX_GLOW,         // particles/glow_0.png  (v7 item effect 6)
    VFX_TEX_STAR,         // particles/star_0.png  (v7 item effect 7)
    VFX_TEX_SCREEN_FLASH, // img/muzflash.png
    VFX_TEX_BLOOD,        // img/blood.png
    VFX_TEX_COUNT,
} VfxTexture;

static const char *const vfx_texture_paths[VFX_TEX_COUNT] = {
    "textures/particles/0.png",
    "textures/particles/1.png",
    "textures/particles/2.png",
    "textures/particles/3.png",
    "textures/particles/default.png",
    "textures/particles/grass_0.png",
    "textures/particles/fog_0.png",
    "textures/particles/ember_0.png",
    "textures/particles/feather.png",
    "textures/particles/glow_0.png",
    "textures/particles/star_0.png",
    "img/muzflash.png",
    "img/blood.png",
};

// ---- per-weapon effect data (27.js). shared/ Weapon has none of these fields.
typedef struct {
    float spd;        // units per ms
    float grav;       // units per ms^2
    float scale;
    float length;
    int color;
    float range;
    float explode;    // explosion range, 0 = none
    int active;       // projectile.disable unset: fired as a projectile
} VfxProjectile;  // documentary: projectiles_render.c draws projectiles (flight values from shared/)

typedef struct {
    const char *src;
    float muz_off, muz_off_y, muz_mlt;
    int muz_id;       // particles/<muzID>.png, default 2
    int no_muzzle;    // nMuz
    int no_case;      // nCase
    int case_ind;     // casing config index, default 1
    float case_y_off, case_z_off;
    float x_off;
    int akimbo, scope, trail; // scope: documentary, Weapon.scope (shared) is used
    int shots;        // -1: weapon.shots unset (one pellet)
    float phys_pow;
    VfxProjectile projectile;
} VfxWeaponFx;

#define VFX_BULLET(spd_, grav_, range_) {spd_, grav_, 1.4f, 10.0f, 0xffffdb, range_, 0.0f, 0}

static const VfxWeaponFx vfx_weapon_fx[] = {
    {"weapon_1", 8.0f, 0.0f, 1.6f, 2, 0, 0, 1, 0.0f, 0.0f, 0.8f, 0, 1, 1, -1, 0.0f, VFX_BULLET(2.5f, 0.0001f, 7000.0f)},
    {"weapon_2", 5.1f, -0.05f, 1.4f, 2, 0, 0, 1, -0.2f, -1.7f, 0.7f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.6f, 0.00011f, 3000.0f)},
    {"weapon_3", 1.5f, 0.0f, 0.95f, 2, 0, 0, 1, 0.0f, 0.2f, 1.2f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.3f, 0.0001f, 1600.0f)},
    {"weapon_4", 2.15f, 0.1f, 1.0f, 2, 0, 0, 1, -0.15f, -0.4f, 0.85f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.4f, 0.00011f, 2700.0f)},
    {"weapon_5", 2.75f, 0.2f, 0.95f, 2, 0, 1, 1, 0.0f, 0.0f, 0.7f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.3f, 0.0001f, 2600.0f)},
    {"weapon_6", 6.0f, 0.0f, 1.8f, 2, 0, 1, 1, 0.0f, 0.0f, 0.95f, 0, 0, 0, 5, 0.09f, {0}},
    {"weapon_7", 5.5f, -0.14f, 1.7f, 2, 0, 0, 2, -0.1f, -0.5f, 0.95f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.3f, 0.0001f, 3000.0f)},
    {"weapon_8", 4.0f, -0.05f, 1.1f, 2, 0, 0, 1, 0.0f, -1.3f, 0.8f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.5f, 0.0001f, 4000.0f)},
    {"weapon_9", 5.0f, 0.0f, 1.5f, 2, 0, 1, 1, 0.0f, 0.0f, 0.95f, 0, 0, 0, 0, 0.095f, {0.72f, 0.00008f, 1.4f, 10.0f, 0xffffdb, 1500.0f, 35.0f, 1}},
    {"weapon_10", 3.6f, 0.0f, 1.0f, 2, 0, 0, 1, -0.15f, -0.4f, 1.5f, 1, 0, 0, -1, 0.0f, {0}},
    {"weapon_11", 2.0f, 0.0f, 1.1f, 2, 0, 0, 1, 0.0f, 0.0f, 1.0f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.3f, 0.0001f, 1500.0f)},
    {"weapon_12", 2.2f, 0.1f, 1.1f, 3, 0, 1, 1, 0.0f, 0.0f, 1.3f, 0, 0, 0, -1, 0.0f, {0}},
    {"weapon_14", 2.2f, 0.1f, 1.1f, 3, 1, 1, 1, 0.0f, 0.0f, 1.0f, 0, 0, 0, -1, 0.0f, {0}},
    {"weapon_15", 4.9f, -0.05f, 1.4f, 2, 0, 0, 1, -0.2f, -1.7f, 1.0f, 0, 0, 0, -1, 0.0f, VFX_BULLET(2.4f, 0.0001f, 3000.0f)},
    {"weapon_16", 6.0f, 0.0f, 1.8f, 2, 0, 1, 1, 0.0f, 0.0f, 0.95f, 0, 0, 0, 5, 0.1f, {0}},
    {"weapon_17", 2.0f, 0.0f, 1.1f, 2, 0, 0, 1, 0.0f, 0.0f, 1.2f, 0, 0, 0, -1, 0.0f, {0}},
    {"weapon_18", 0.0f, 0.0f, 1.0f, 2, 1, 1, 1, 0.0f, 0.0f, 0.0f, 0, 0, 0, -1, 0.0f, {0}},
    {"weapon_19", 5.1f, -0.08f, 1.4f, 3, 0, 1, 1, -0.2f, -1.7f, 0.7f, 0, 0, 0, -1, 0.0f, {0}},
};

static const VfxWeaponFx vfx_weapon_fx_default = {NULL, 0.0f, 0.0f, 1.0f, 2, 0, 0, 1, 0.0f, 0.0f, 0.0f, 0, 0, 0, -1, 0.0f, {0}};

// 88.js `C`: physObj configs. 0 = bullet tracer, 1/2 = shell casings (caseInd).
typedef struct {
    float spd[2];     // [min, max] units per ms (max 0: fixed)
    int rand_roll;    // `rand`: random initial roll
    int carry_v;      // add the shooter's velocity
    float grav;
    float spin[2];
    float scale;
    float length;
    int color;
    int lit;          // MeshBasicMaterial (0) or the default Lambert material (1)
} VfxPhysConfig;

static const VfxPhysConfig vfx_phys_configs[] = {
    {{1.7f, 0.0f}, 0, 0, 0.0f, {0.0f, 0.0f}, 1.0f, 13.0f, 0xffffdb, 0},
    {{0.03f, 0.031f}, 1, 1, 0.0003f, {0.04f, 0.05f}, 0.15f, 0.5f, 0x9a7b3e, 1},
    {{0.03f, 0.031f}, 1, 1, 0.0003f, {0.03f, 0.05f}, 0.1f, 0.4f, 0x9a7b3e, 1},
};

// 88.js `F[0]`: bullet impact (effect 0). The other entries are unused in v3.1.1.
#define VFX_IMPACT_COUNT 2
#define VFX_IMPACT_GRAV (-0.00002f)
#define VFX_IMPACT_SCALE_MIN 5.0f
#define VFX_IMPACT_SCALE_MAX 9.0f
#define VFX_IMPACT_SPEED_MAX 0.025f
#define VFX_IMPACT_SPREAD 0.4f
#define VFX_IMPACT_LIFE_MIN 300
#define VFX_IMPACT_LIFE_MAX 500
#define VFX_HOLE_SIZE_MIN 0.4f
#define VFX_HOLE_SIZE_MAX 1.0f
#define VFX_HOLE_LIFE 4000.0f

// 88.js `z`: sniper trail
#define VFX_TRAIL_SCALE 0.45f
#define VFX_TRAIL_SHRINK 0.00036f // per ms
#define VFX_TRAIL_COLOR 0xededed
#define VFX_TRAIL_OPACITY 0.21f

// 88.js `A` ExplosionManager
#define VFX_EXPLOSION_LARGE 8
#define VFX_EXPLOSION_SMALL 15
#define VFX_EXPLOSION_COLOR 0xbbbbbb
#define VFX_EXPLOSION_EMISSIVENESS 0.55f
#define VFX_EXPLOSION_DAMPING 3.0f
#define VFX_EXPLOSION_LIFE_MIN 2.2f
#define VFX_EXPLOSION_LIFE_RANGE 3.0f

// 88.js `E`: sprite rotations, picked with randInt(0, 2) (index 2 = undefined = 0)
static const float vfx_sprite_rotations[3] = {(float) M_PI / 3.0f, (float) -M_PI / 3.0f, 0.0f};

typedef struct {
    unsigned char active;
    unsigned char blend;
    unsigned char hidden;   // area particle outside the particle distance / wrapped
    int texture;
    vec3 position;
    vec3 velocity;          // per ms
    float base_vy;          // area particles: vertical speed restored on wrap
    float grav;             // per ms^2, subtracted from velocity.y
    float size;
    float rotation;
    float life;             // ms; Krunker hides once it runs out after one update
    int updates;
    vec4 color;
    int area;               // index into areas, -1 for none
    float vis_timer;
} VfxSprite;

typedef struct {
    unsigned char active;
    unsigned char lit;
    unsigned char fog;
    unsigned char look_dir;
    vec3 position;
    vec3 velocity;          // per ms
    vec3 forward;           // lookAt direction (local +z)
    float roll;             // accumulated rotateX
    float spd;
    float grav;
    float dst;
    float spin;
    vec3 size;              // scale.x, scale.y, length
    vec4 color;
} VfxBox;

typedef struct {
    unsigned char active;
    vec3 start;
    vec3 forward;
    float len;
    float dst;
    float spd;
    float scale;
} VfxTrail;

typedef struct {
    unsigned char active;
    vec3 position;
    vec3 velocity;          // per second (ExplosionManager works in seconds)
    float life;
    float life_speed;
    float size;
} VfxDebris;

typedef struct {
    vec3 position;          // x/z centre, y base
    vec3 half;              // half width, height, half length
} VfxArea;

typedef struct {
    unsigned char active;
    int texture;
    float size;
    float rotation;
    vec3 local;             // relative to the held weapon's arm anchor
    float age;
    int frames;
} VfxMuzzle;

typedef struct {
    float life;
    float start_life;
    vec2 center;
    float speed;
    float alpha;
    float angle;
    float size;
    float in_radius;
    float out_radius;
} VfxSpeedLine;

typedef struct {
    int particles;
    float particle_distance;
    int show_trails;
    int your_trails;
    int muzzle_flash;
    int tracers;
    int damage_overlay;
    vec4 tracer_color;
} VfxSettings;

typedef struct {
    float position[3];
    float tex_coord[2];
    float color[4];
} VfxVertex;

typedef struct {
    unsigned char blend;
    unsigned char fog;
    unsigned char depth_write;
    unsigned char use_texture;
    unsigned char cull;     // boxes: three.js FrontSide (camera inside a box sees nothing)
    unsigned int texture;
} VfxDrawState;

typedef struct {
    VfxDrawState state;
    int first;
    int count;
} VfxDraw;

typedef struct {
    float depth;
    int kind;   // 0 sprite, 1 trail
    int index;
} VfxSortItem;

static struct {
    int initialized;
    unsigned int program;
    unsigned int vao;
    unsigned int vbo;
    unsigned int textures[VFX_TEX_COUNT];
    unsigned char texture_failed[VFX_TEX_COUNT];

    VfxSprite sprites[VFX_MAX_SPRITES];
    VfxBox boxes[VFX_MAX_BOXES];
    VfxTrail trails[VFX_MAX_TRAILS];
    VfxDebris debris[VFX_MAX_DEBRIS];
    VfxArea areas[VFX_MAX_AREAS];
    int area_count;
    VfxMuzzle muzzles[2];
    VfxSpeedLine speed_lines[VFX_SPEED_LINES];
    float frame_ms;         // this frame's effect delta, for the overlay layer

    float screen_flash_age;
    int screen_flash_frames;
    int screen_flash_active;
    vec2 screen_flash_pos;  // 0..1, y down (canvas convention)

    const void *map;        // map the areas were built for
    int map_index;

    const Player *me;       // fire detection baseline
    int me_uid;
    int me_loadout_index;
    float me_reload;
    const Player *proj_me;        // projectile prediction baseline
    int proj_me_uid;
    unsigned int proj_shot_count; // Player.shot_count (shotCntP): a projectile or knife throw

    VfxSettings settings;
    float time_ms;

    VfxVertex *vertices;
    size_t vertex_count;
    size_t vertex_capacity;
    VfxDraw *draws;
    size_t draw_count;
    size_t draw_capacity;
    VfxSortItem *sort_items;
    size_t sort_capacity;

    int log;              // KRUNKNATIVE_VFX_LOG=1: print spawns to stderr
    float time_scale;     // KRUNKNATIVE_VFX_TIMESCALE: slow motion for captures (debug)
} vfx = {.map_index = -1, .me_loadout_index = -1};

// ---------------------------------------------------------------- helpers

static float rand_unit(void) {
    return (float) pcg32_random() / 4294967296.0f;
}

// 2.js randFloat / randInt (inclusive, floor-based like Krunker's)
static float rand_float(const float min, const float max) {
    return rand_unit() * (max - min) + min;
}

static float rand_int(const float min, const float max) {
    return floorf(rand_unit() * (max - min + 1.0f)) + min;
}

static vec3 v3(const float x, const float y, const float z) {
    return (vec3) {x, y, z};
}

static vec3 v3_add(const vec3 a, const vec3 b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static vec3 v3_sub(const vec3 a, const vec3 b) {
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static vec3 v3_scale(const vec3 a, const float s) {
    return v3(a.x * s, a.y * s, a.z * s);
}

static float v3_dot(const vec3 a, const vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3 v3_cross(const vec3 a, const vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static float v3_length(const vec3 a) {
    return sqrtf(v3_dot(a, a));
}

static vec3 v3_normalize(const vec3 a) {
    const float length = v3_length(a);
    return length > 0.0f ? v3_scale(a, 1.0f / length) : a;
}

static vec4 color_from_hex(const int hex, const float alpha) {
    vec4 color = hex_to_vec(hex);
    color.w = alpha;
    return color;
}

// Krunker's shot direction for yaw/pitch (95.js: sin(yaw + PI) * cos(pitch), ...).
static vec3 direction_from_angles(const float yaw, const float pitch) {
    return v3(sinf(yaw + (float) M_PI) * cosf(pitch), sinf(pitch), cosf(yaw + (float) M_PI) * cosf(pitch));
}

// THREE Object3D.lookAt basis for a non-camera object: +z points at the target.
static void look_basis(const vec3 forward, vec3 *x, vec3 *y, vec3 *z) {
    *z = v3_normalize(forward);
    if (v3_length(*z) == 0.0f) *z = v3(0.0f, 0.0f, 1.0f);

    vec3 right = v3_cross(v3(0.0f, 1.0f, 0.0f), *z);

    if (v3_length(right) < 1e-6f) {
        z->z += 0.0001f;
        *z = v3_normalize(*z);
        right = v3_cross(v3(0.0f, 1.0f, 0.0f), *z);
    }

    *x = v3_normalize(right);
    *y = v3_cross(*z, *x);
}

static unsigned int vfx_texture(const int index) {
    if (index < 0 || index >= VFX_TEX_COUNT) return 0;
    if (vfx.textures[index] || vfx.texture_failed[index]) return vfx.textures[index];

    char *path = concat(assets_path(), vfx_texture_paths[index]);
    if (path) {
        vfx.textures[index] = load_texture(path);
        free(path);
    }

    if (!vfx.textures[index]) vfx.texture_failed[index] = 1;
    return vfx.textures[index];
}

static int setting_flag(const char *key) {
    return !ui_setting_is_false(key);
}

static void vfx_read_settings(void) {
    VfxSettings *s = &vfx.settings;

    s->particles = setting_flag("particles");
    s->show_trails = setting_flag("showTrails");
    s->your_trails = setting_flag("yourTrails");
    s->muzzle_flash = setting_flag("muzzleFlash");
    s->tracers = setting_flag("bulletTracers");
    s->damage_overlay = setting_flag("useDamageOverlay");

    double distance = 160.0;
    if (!ui_setting_number("particlesDist", &distance)) distance = 160.0;
    s->particle_distance = (float) distance;

    s->tracer_color = hex_to_vec(0xffffdb);
    const cJSON *color = ui_setting_value("bulletTracerCol");
    if (cJSON_IsString(color)) parse_hex_color(cJSON_GetStringValue(color), &s->tracer_color);
    else if (cJSON_IsNumber(color)) s->tracer_color = hex_to_vec((int) cJSON_GetNumberValue(color));
    s->tracer_color.w = 1.0f;

    static int logged;
    if (vfx.log && !logged) {
        logged = 1;
        char *text = color ? cJSON_PrintUnformatted(color) : NULL;
        fprintf(stderr, "[vfx] bulletTracerCol %s -> %.3f %.3f %.3f\n", text ? text : "(unset)", s->tracer_color.x, s->tracer_color.y, s->tracer_color.z);
        free(text);
    }
}

static const VfxWeaponFx *weapon_fx(const Weapon *weapon) {
    if (!weapon || !weapon->src) return &vfx_weapon_fx_default;

    for (size_t i = 0; i < sizeof(vfx_weapon_fx) / sizeof(vfx_weapon_fx[0]); i++) {
        if (!strcmp(vfx_weapon_fx[i].src, weapon->src)) return &vfx_weapon_fx[i];
    }

    return &vfx_weapon_fx_default;
}

// Objects a bullet stops at: Krunker skips `noShoot` objects (zones, triggers,
// pickups, flags) and inactive ones; KrunkNative has no noShoot flag, so the
// equivalent prefab flags are filtered here.
static int object_is_shootable(const Object *object) {
    if (!object->active || object->no_shoot || object->collision_type == COLLISION_TYPE_NONE) return 0;
    // TRIGGERs are shootable except onEnter ones (no_shoot), as in player.c combat_object_shootable
    if (object->score_zone || object->team_zone || object->teleporter || object->checkpoint) return 0;
    if (object->pickup || object->flag || object->objective || object->bomb_site || object->kill) return 0;
    if (object->premium || object->verified) return 0;
    return 1;
}

// Nearest shootable object along origin + ray (ray = direction * range); returns
// the ray fraction (0..1] or -1 and the object hit.
static float ray_hit(const Game *game, const vec3 origin, const vec3 ray, const Object **hit_object) {
    float best = -1.0f;
    *hit_object = NULL;
    if (!game->map) return best;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!object_is_shootable(object)) continue;

        const float t = line_in_rect(origin, ray, object->position, object->scale);
        if (t <= 0.0f || t > 1.0f) continue;

        if (best < 0.0f || t < best) {
            best = t;
            *hit_object = object;
        }
    }

    return best;
}

// 95.js: impact face angles from where the (pulled back) hit point sits relative
// to the object box (KrunkNative objects: x/z centred, y at the base).
static void impact_angles(const Object *object, const vec3 hit, float *yaw, float *pitch) {
    *yaw = 0.0f;
    *pitch = 0.0f;

    if (hit.y >= object->position.y + object->scale.y) *pitch = (float) M_PI / 2.0f;
    else if (hit.y <= object->position.y) *pitch = (float) -M_PI / 2.0f;
    else if (hit.x <= object->position.x - object->scale.x * 0.5f) *yaw = (float) -M_PI / 2.0f;
    else if (hit.x >= object->position.x + object->scale.x * 0.5f) *yaw = (float) M_PI / 2.0f;
    else if (hit.z <= object->position.z - object->scale.z * 0.5f) *yaw = (float) M_PI;
}

// ---------------------------------------------------------------- spawning

static VfxSprite *sprite_alloc(void) {
    for (int i = 0; i < VFX_MAX_SPRITES; i++) {
        if (!vfx.sprites[i].active) return &vfx.sprites[i];
    }

    return NULL;
}

// 88.js add() + Sprite.init: `blend` 0 means NormalBlending like Krunker's `P.blend || 0`.
static VfxSprite *sprite_add(const vec3 position, const int texture, const VfxBlend blend, const float size, const vec3 velocity,
                             const float grav, const float life, const int random_rotation) {
    VfxSprite *sprite = sprite_alloc();
    if (!sprite) return NULL;

    memset(sprite, 0, sizeof(*sprite));
    sprite->active = 1;
    sprite->texture = texture;
    sprite->blend = (unsigned char) blend;
    sprite->position = position;
    sprite->velocity = velocity;
    sprite->base_vy = velocity.y;
    sprite->grav = grav;
    sprite->size = size;
    sprite->life = life;
    sprite->rotation = random_rotation ? vfx_sprite_rotations[(int) rand_int(0.0f, 2.0f)] : 0.0f;
    sprite->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    sprite->area = -1;
    return sprite;
}

// 88.js effect(x, y, z, yaw, pitch, 0): bullet impact puffs plus the hole decal.
static void spawn_impact(const vec3 position, const float yaw, const float pitch) {
    if (!vfx.settings.particles) return;

    for (int i = 0; i < VFX_IMPACT_COUNT; i++) {
        const float speed = rand_float(0.0f, VFX_IMPACT_SPEED_MAX);
        const float u = yaw + rand_float(-VFX_IMPACT_SPREAD, VFX_IMPACT_SPREAD);
        const float v = pitch + rand_float(-VFX_IMPACT_SPREAD, VFX_IMPACT_SPREAD);
        const vec3 velocity = {speed * sinf(u) * cosf(v), speed * sinf(v), speed * cosf(u) * cosf(v)};

        sprite_add(position, VFX_TEX_PART_0, VFX_BLEND_NORMAL, rand_float(VFX_IMPACT_SCALE_MIN, VFX_IMPACT_SCALE_MAX), velocity,
                   VFX_IMPACT_GRAV, rand_int(VFX_IMPACT_LIFE_MIN, VFX_IMPACT_LIFE_MAX), 1);
    }

    sprite_add(position, VFX_TEX_PART_1, VFX_BLEND_SUBTRACTIVE, rand_float(VFX_HOLE_SIZE_MIN, VFX_HOLE_SIZE_MAX), v3(0.0f, 0.0f, 0.0f),
               0.0f, VFX_HOLE_LIFE, 1);
}

void vfx_impact(Client *client, const vec3 position, const float yaw, const float pitch) {
    (void) client;
    spawn_impact(position, yaw, pitch);
}

// v7.1.4 particles.js effects 5-7 (store items' `particle`: Moonfeather 5, Ember Slides 6): count
// sprites of src, speed 0.005-0.007 per ms in a random direction (spread -PI..PI on yaw and pitch),
// no gravity, colour picked from colr, life 1000-1800 ms. dstVis 70 (visible within particlesDist * 70)
// keeps them on at any distance a map allows.
typedef struct {
    int texture, count;
    VfxBlend blend;
    float scale[2];
    int colors[7], color_count;
} VfxItemEffect;

static const VfxItemEffect vfx_item_effects[3] = {
    {VFX_TEX_FEATHER, 1, VFX_BLEND_NORMAL, {2.0f, 3.0f}, {0x764CAD, 0xffffc2, 0x51ff99, 0xffa0b3}, 4},
    {VFX_TEX_GLOW, 2, VFX_BLEND_ADDITIVE, {0.5f, 0.8f}, {0xffd666, 0xffbd66}, 2},
    {VFX_TEX_STAR, 5, VFX_BLEND_ADDITIVE, {0.2f, 1.0f}, {0x0A0068, 0x26007B, 0x42008D, 0x5E00A0, 0x7A00B2, 0xffffff, 0x000000}, 7},
};

void vfx_item_effect(const vec3 position, const int effect) {
    if (!vfx.settings.particles || effect < 5 || effect > 7) return;
    const VfxItemEffect *e = &vfx_item_effects[effect - 5];
    for (int i = 0; i < e->count; i++) {
        const float speed = rand_float(0.005f, 0.007f);
        const float u = rand_float((float) -M_PI, (float) M_PI);
        const float v = rand_float((float) -M_PI, (float) M_PI);
        const vec3 velocity = {speed * sinf(u) * cosf(v), speed * sinf(v), speed * cosf(u) * cosf(v)};
        VfxSprite *sprite = sprite_add(position, e->texture, e->blend, rand_float(e->scale[0], e->scale[1]), velocity, 0.0f,
                                       rand_int(1000.0f, 1800.0f), 1);
        if (!sprite) break;
        const int col = e->colors[(int) rand_int(0.0f, (float) (e->color_count - 1))];
        sprite->color = (vec4) {(float) ((col >> 16) & 255) / 255.0f, (float) ((col >> 8) & 255) / 255.0f, (float) (col & 255) / 255.0f, 1.0f};
    }
}

static VfxBox *box_alloc(void) {
    for (int i = 0; i < VFX_MAX_BOXES; i++) {
        if (!vfx.boxes[i].active) return &vfx.boxes[i];
    }

    return NULL;
}

// 88.js D.init: a box flying from `position` along yaw/pitch for `dst` units.
static VfxBox *box_spawn(const vec3 position, const vec3 direction, const float dst, const float spd, const float grav,
                         const vec3 carry, const vec3 size, const vec4 color, const int lit, const int look_dir) {
    VfxBox *box = box_alloc();
    if (!box) return NULL;

    memset(box, 0, sizeof(*box));
    box->active = 1;
    box->position = position;
    box->spd = spd;
    box->velocity = v3_add(v3_scale(v3_normalize(direction), spd), carry);
    box->forward = box->velocity;
    box->grav = grav;
    box->dst = dst;
    box->size = size;
    box->color = color;
    box->lit = (unsigned char) lit;
    box->fog = (unsigned char) lit; // MeshBasicMaterial configs are created with fog: false
    box->look_dir = (unsigned char) look_dir;
    return box;
}

static void spawn_trail(const VfxBox *tracer) {
    for (int i = 0; i < VFX_MAX_TRAILS; i++) {
        VfxTrail *trail = &vfx.trails[i];
        if (trail->active) continue;

        trail->active = 1;
        trail->start = tracer->position;
        trail->forward = v3_normalize(tracer->velocity);
        trail->len = 0.0f;
        trail->dst = tracer->dst;
        trail->spd = tracer->spd;
        trail->scale = VFX_TRAIL_SCALE;
        return;
    }
}

// physObj(..., C[0]): bullet tracer from `start` to `end`; `trail` adds the sniper trail.
static void spawn_tracer(const vec3 start, const vec3 end, const float dst, const int trail) {
    const VfxPhysConfig *config = &vfx_phys_configs[0];
    const vec3 size = {config->scale, config->scale, config->length};

    VfxBox *box = box_spawn(start, v3_sub(end, start), dst, config->spd[0], 0.0f, v3(0.0f, 0.0f, 0.0f), size,
                            vfx.settings.tracer_color, 0, 0);

    if (box && trail && vfx.settings.show_trails) spawn_trail(box);
}

static void spawn_casing(const vec3 position, const float yaw, const float pitch, const int case_ind, const vec3 carry) {
    const int index = case_ind >= 1 && case_ind <= 2 ? case_ind : 1;
    const VfxPhysConfig *config = &vfx_phys_configs[index];
    const float spd = rand_float(config->spd[0], config->spd[1]);
    const vec3 size = {config->scale, config->scale, config->length};

    VfxBox *box = box_spawn(position, direction_from_angles(yaw, pitch), 25.0f, spd, config->grav, carry, size,
                            color_from_hex(config->color, 1.0f), config->lit, 0);
    if (!box) return;

    if (config->rand_roll) box->roll = rand_float((float) -M_PI, (float) M_PI);
    box->spin = rand_float(config->spin[0], config->spin[1]);
}

// 88.js ExplosionManager.explodeAt(x, y, z, scale)
static void spawn_debris(const vec3 position, const float seed, const float size_min, const float size_range,
                         const float velocity_min, const float velocity_range) {
    for (int i = 0; i < VFX_MAX_DEBRIS; i++) {
        VfxDebris *debris = &vfx.debris[i];
        if (debris->active) continue;

        const float rank = powf(seed, 2.5f);
        const float speed = (1.0f - rank) * velocity_range + velocity_min;
        const vec3 direction = v3_normalize(v3(rand_unit() - 0.5f, rand_unit() - 0.5f, rand_unit() - 0.5f));

        debris->active = 1;
        debris->position = position;
        debris->velocity = v3_scale(direction, speed);
        debris->life = 0.0f;
        debris->life_speed = rank * VFX_EXPLOSION_LIFE_RANGE + VFX_EXPLOSION_LIFE_MIN;
        debris->size = rank * size_range + size_min;
        return;
    }
}

void vfx_explosion(Client *client, const vec3 position, const float radius) {
    (void) client;
    const float scale = radius * VFX_EXPLOSION_SCALE;
    if (scale <= 0.0f) return;

    const float size_min = scale * 0.35f;
    const float size_range = scale * 1.8f;
    const float velocity_min = scale * 30.0f;
    const float velocity_range = scale * 17.0f;

    for (int i = 0; i < VFX_EXPLOSION_LARGE; i++) {
        spawn_debris(position, rand_unit() * 0.1f + 0.9f, size_min, size_range, velocity_min, velocity_range);
    }

    for (int i = 0; i < VFX_EXPLOSION_SMALL; i++) {
        spawn_debris(position, rand_unit() * 0.9f, size_min, size_range, velocity_min, velocity_range);
    }

    if (vfx.log) fprintf(stderr, "[vfx] explosion %.1f %.1f %.1f r=%.1f\n", position.x, position.y, position.z, radius);
}

// ---------------------------------------------------------------- local player

static PlayerArms *held_arms(const Player *player) {
    const PlayerMesh *mesh = player ? player->mesh : NULL;
    if (!mesh || !mesh->arms || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return NULL;
    return mesh->arms[player->loadout_index];
}

// World position of a point given relative to the held weapon's arm anchor (Krunker's
// per-weapon container `ac`, which parents the muzzle sprites and casing anchors).
static int arms_point(const Player *player, const vec3 local, vec3 *out) {
    PlayerArms *arms = held_arms(player);
    if (!arms) return 0;

    Mesh point = {0};
    point.transform.parent = &arms->anchor;
    point.transform.position = local;
    point.transform.scale = v3(1.0f, 1.0f, 1.0f);
    mesh_update_transform_matrix(&point);

    *out = v3(point.transform_matrix[3], point.transform_matrix[7], point.transform_matrix[11]);
    return 1;
}

static vec3 muzzle_local(const Player *player, const VfxWeaponFx *fx, const int index) {
    const float x = index ? fx->x_off * -2.0f * (player->left_handed ? -1.0f : 1.0f) : 0.0f;
    return v3(x, fx->muz_off_y, -fx->muz_off);
}

static int camera_offset_active(const Client *client) {
    const Map *map = client->game.map;
    return client->game.config.third_person || (map && (map->config.cam_offset.x || map->config.cam_offset.y || map->config.cam_offset.z));
}

static int project_to_screen(Camera *camera, const vec3 position, vec2 *out) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[3] <= 0) return 0;

    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);

    const float p[4] = {position.x, position.y, position.z, 1.0f};
    float view[4], clip[4];

    for (int r = 0; r < 4; r++) {
        view[r] = camera->world_inverse_matrix[r * 4] * p[0] + camera->world_inverse_matrix[r * 4 + 1] * p[1] +
                  camera->world_inverse_matrix[r * 4 + 2] * p[2] + camera->world_inverse_matrix[r * 4 + 3] * p[3];
    }

    for (int r = 0; r < 4; r++) {
        clip[r] = camera->projection_matrix[r * 4] * view[0] + camera->projection_matrix[r * 4 + 1] * view[1] +
                  camera->projection_matrix[r * 4 + 2] * view[2] + camera->projection_matrix[r * 4 + 3] * view[3];
    }

    if (clip[3] == 0.0f) return 0;

    // THREE Vector3.project keeps going behind the camera; so does Krunker's flash.
    out->x = (clip[0] / clip[3] + 1.0f) * 0.5f;
    out->y = 1.0f - (clip[1] / clip[3] + 1.0f) * 0.5f;
    return 1;
}

// 95.js shoot(), client half: muzzle sprites, overlay flash, casings, predicted
// projectile, then per pellet the impact effect, tracer and trail.
static void local_shot(Client *client, Player *me) {
    const Weapon *weapon = me->weapon;
    if (!weapon || weapon->melee) return;

    const VfxWeaponFx *fx = weapon_fx(weapon);
    const int scoped = weapon->scope && me->aim_val >= 1.0f;
    const int sprite_model = client->game.map && client->game.map->config.model == MODEL_TYPE_SPRITE;
    const int muzzle_count = fx->akimbo ? 2 : 1;

    if (vfx.log) fprintf(stderr, "[vfx] shot %s scoped=%d\n", weapon->src ? weapon->src : "?", scoped);

    PlayerArms *arms = held_arms(me);

    // !isWeaponHidden: showWeapon/Sec/Mel off means no muzzle sprites, overlay flash or casings
    if (me->render_you && !sprite_model && ui_controls_weapon_visible(client)) {
        if (vfx.settings.muzzle_flash && !fx->no_muzzle && !camera_offset_active(client)) {
            for (int i = 0; i < muzzle_count; i++) {
                VfxMuzzle *muzzle = &vfx.muzzles[i];
                if (arms) arms->muzzle[i] = muzzle_local(me, fx, i); // muzzles[i].init(...)

                muzzle->active = 1;
                muzzle->texture = fx->muz_id == 3 ? VFX_TEX_PART_3 : VFX_TEX_PART_2;
                muzzle->size = fx->muz_mlt * 4.0f;
                muzzle->rotation = vfx_sprite_rotations[(int) rand_int(0.0f, 2.0f)];
                muzzle->local = muzzle_local(me, fx, i);
                muzzle->age = 0.0f;
                muzzle->frames = 0;
            }

            vec3 world;
            vec2 screen;

            if (!scoped && arms_point(me, vfx.muzzles[0].local, &world) && project_to_screen(&client->camera, world, &screen)) {
                vfx.screen_flash_active = 1;
                vfx.screen_flash_age = 0.0f;
                vfx.screen_flash_frames = 0;
                vfx.screen_flash_pos = screen;
            }
        }

        if (!fx->no_case && vfx.settings.particles && !scoped) {
            for (int i = 0; i < muzzle_count; i++) {
                vec3 world;
                const vec3 local = {i ? -fx->x_off * 2.0f : 0.0f, fx->case_y_off, fx->case_z_off};
                if (!arms_point(me, local, &world)) continue;

                const float pitch = (float) M_PI / 5.0f * rand_float(0.75f, 1.2f) * (rand_int(0.0f, 1.0f) ? 1.4f : 0.8f);
                const float yaw = me->direction.y - (float) M_PI / 2.0f * (i ? -1.0f : 1.0f);
                spawn_casing(world, yaw, pitch, fx->case_ind, me->velocity);
            }
        }
    }

    if (me->render_you && !vfx.settings.your_trails && !fx->phys_pow) return;

    const float eye_y = me->position.y + me->height - game_constants.camera_height;
    const vec3 origin = {me->position.x, eye_y, me->position.z};
    const float base_pitch = me->direction.x + me->recoil_anim_y * game_constants.recoil_mlt;
    const float base_yaw = me->direction.y;
    const float spread_range = (me->spread + weapon->inaccuracy) * game_constants.spread_adjustment;
    // Same test as player.c: the projectile itself is predicted by projectiles_render.c
    // (detect_local_shot sees shot_count move), only a physPow ray stays hitscan.
    const int is_projectile = weapon->projectile && (!weapon->projectile_disable || client->game.config.bullet_drop);
    if (is_projectile && !fx->phys_pow) return;

    const int shots = fx->shots < 0 ? 1 : fx->shots;
    const int ammo = (int) me->ammo[me->loadout_index];

    for (int i = 0; i < shots; i++) {
        float yaw, pitch;

        if (weapon->custom_spread && ammo >= 0 && (unsigned int) ammo < weapon->ammo) {
            const vec2 spread = weapon->custom_spread[ammo * shots + i];
            const float mlt = rand_float(0.3f, 0.32f);
            yaw = base_yaw + spread.x * mlt;
            pitch = base_pitch + spread.y * mlt;
        } else {
            yaw = base_yaw + rand_float(-spread_range, spread_range);
            pitch = base_pitch + rand_float(-spread_range, spread_range);
        }

        const float range = weapon->range;
        const vec3 direction = direction_from_angles(yaw, pitch);
        const Object *object = NULL;
        const float t = ray_hit(&client->game, origin, v3_scale(direction, range), &object);

        float distance = t > 0.0f ? range * t : range;
        distance -= VFX_HIT_PULLBACK;
        const vec3 hit = v3_add(origin, v3_scale(direction, distance));

        if (object) {
            float normal_yaw, normal_pitch;
            impact_angles(object, hit, &normal_yaw, &normal_pitch);
            spawn_impact(hit, normal_yaw, normal_pitch);
        }

        if (distance >= VFX_TRACER_MIN_DST && vfx.settings.tracers) {
            // muzzles[i].getWorldPosition(): the barrel tip only once a muzzle flash has placed the
            // sprite there; with muzzleFlash off (or the weapon hidden, camOffset, nMuz) v3.1.1's
            // tracer starts at the weapon anchor origin instead.
            vec3 start;
            const int muzzle = muzzle_count > 1 ? (int) rand_int(0.0f, (float) muzzle_count - 1.0f) : 0;

            if (sprite_model || !arms || !arms_point(me, arms->muzzle[muzzle], &start)) {
                start = me->position;
                start.y += me->height / 2.0f;
            }

            const float back = scoped ? VFX_SCOPED_TRACER_BACK : 0.0f;
            start = v3_sub(start, v3_scale(direction, back));
            spawn_tracer(start, hit, fminf(distance + back, VFX_TRACER_MAX_DST), fx->trail);
            if (vfx.log) {
                fprintf(stderr, "[vfx] tracer start %.2f %.2f %.2f dst %.2f (camera %.2f %.2f %.2f fov %.2f)\n", start.x, start.y, start.z,
                        fminf(distance + back, VFX_TRACER_MAX_DST), client->camera.position.x, client->camera.position.y,
                        client->camera.position.z, client->camera.fov);
            }
        }
    }
}

// Shots are not events in shared/ yet: a fresh fire timer (reloads[slot] jumping
// up) on the held slot means player_shoot() ran during the last game tick.
static void detect_local_shot(Client *client) {
    Player *me = client->me;

    if (!me || !me->active || !me->reloads || me->loadout_index < 0 || me->loadout_index >= me->loadout_size) {
        vfx.me = NULL;
        return;
    }

    const float reload = me->reloads[me->loadout_index];
    const int same = vfx.me == me && vfx.me_uid == me->uid && vfx.me_loadout_index == me->loadout_index;

    if (same && reload > vfx.me_reload + 1e-4f) local_shot(client, me);

    vfx.me = me;
    vfx.me_uid = me->uid;
    vfx.me_loadout_index = me->loadout_index;
    vfx.me_reload = reload;
}

// 95.js shoot(): shotCntP++ for every projectile and knife throw, which the owner predicts
// (physObj). Krunker spawns it in players.update and moves it in the particle update of the
// same frame, so this runs before projectiles_render_update(): the knife leaves the eye
// in the frame of the throw.
static void detect_local_projectile(Client *client) {
    Player *me = client->me;

    if (!me || !me->active || !me->weapon) {
        vfx.proj_me = NULL;
        return;
    }

    if (vfx.proj_me == me && vfx.proj_me_uid == me->uid && me->shot_count != vfx.proj_shot_count) {
        projectiles_render_local_shot(client, me, me->weapon->melee);
    }

    vfx.proj_me = me;
    vfx.proj_me_uid = me->uid;
    vfx.proj_shot_count = me->shot_count;
}

void vfx_remote_shot(Client *client, const Player *shooter, const vec3 hit, const int has_normal, const float normal_yaw,
                     const float normal_pitch, const int trail) {
    if (!shooter) return;
    if (vfx.log) {
        fprintf(stderr, "[vfx] remote shot from %.1f %.1f %.1f to %.1f %.1f %.1f\n", shooter->position.x, shooter->position.y,
                shooter->position.z, hit.x, hit.y, hit.z);
    }

    if (vfx.settings.tracers) {
        const vec3 start = {shooter->position.x, shooter->position.y + shooter->height - game_constants.camera_height -
                            shooter->crouch_val * game_constants.crouch_distance, shooter->position.z};
        spawn_tracer(start, hit, v3_length(v3_sub(hit, start)), trail);
    }

    // index.js g7: impacts only within 160 units of the local player
    const Player *me = client->me;
    if (has_normal && me && v3_length(v3_sub(shooter->position, me->position)) <= 160.0f) {
        spawn_impact(hit, normal_yaw, normal_pitch);
    }
}

// ---------------------------------------------------------------- map particle areas

static float json_number(const cJSON *object, const char *key, const float fallback) {
    const cJSON *item = cJSON_GetObjectItem(object, key);
    if (cJSON_IsNumber(item)) return (float) cJSON_GetNumberValue(item);
    if (cJSON_IsString(item)) return (float) atof(cJSON_GetStringValue(item));
    return fallback;
}

// parseInt(x) || fallback, as 93.js reads the PARTICLES fields
static float json_parse_int(const cJSON *object, const char *key, const float fallback) {
    const float value = truncf(json_number(object, key, 0.0f));
    return value != 0.0f ? value : fallback;
}

// 93.js PARTICLES -> 88.js area(): `cnt` static sprites wrapping inside the box.
static void build_areas(const Map *map) {
    vfx.area_count = 0;
    if (!map || !map->raw_data) return;

    const cJSON *objects = cJSON_GetObjectItem(map->raw_data, "objects");
    const cJSON *raw;

    cJSON_ArrayForEach(raw, objects) {
        const cJSON *prefab = cJSON_GetObjectItem(raw, "i");
        if (!prefab) prefab = cJSON_GetObjectItem(raw, "id");
        if (!cJSON_IsNumber(prefab) || (int) cJSON_GetNumberValue(prefab) != PREFAB_PARTICLES) continue;

        const cJSON *p = cJSON_GetObjectItem(raw, "p");
        const cJSON *s = cJSON_GetObjectItem(raw, "s");
        if (cJSON_GetArraySize(p) < 3 || cJSON_GetArraySize(s) < 3 || vfx.area_count >= VFX_MAX_AREAS) continue;

        VfxArea *area = &vfx.areas[vfx.area_count];
        area->position = v3((float) cJSON_GetNumberValue(cJSON_GetArrayItem(p, 0)), (float) cJSON_GetNumberValue(cJSON_GetArrayItem(p, 1)),
                            (float) cJSON_GetNumberValue(cJSON_GetArrayItem(p, 2)));
        area->half = v3((float) cJSON_GetNumberValue(cJSON_GetArrayItem(s, 0)) / 2.0f, (float) cJSON_GetNumberValue(cJSON_GetArrayItem(s, 1)),
                        (float) cJSON_GetNumberValue(cJSON_GetArrayItem(s, 2)) / 2.0f);

        const int txt = (int) json_parse_int(raw, "pr", 0.0f);
        const float spd = json_parse_int(raw, "psp", 0.0f) * 0.005f;
        const int count = (int) json_parse_int(raw, "pc", 0.0f);
        const float grav = json_parse_int(raw, "pg", 0.0f);
        const float size = json_parse_int(raw, "ps", 1.0f);
        const float dir = json_number(raw, "d", 0.0f);
        const int blend = (int) json_number(raw, "pb", 0.0f);
        const float angle = (dir + 1.0f) * (float) M_PI / 2.0f - (float) M_PI / 2.0f;

        // 1.js particleIDS = default, grass, fog, ember ("_0" appended unless default)
        static const int textures[] = {VFX_TEX_DEFAULT, VFX_TEX_GRASS, VFX_TEX_FOG, VFX_TEX_EMBER};
        const int texture = txt >= 0 && txt < 4 ? textures[txt] : VFX_TEX_DEFAULT;

        vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
        const cJSON *c = cJSON_GetObjectItem(raw, "c");
        if (cJSON_IsNumber(c)) color = hex_to_vec((int) cJSON_GetNumberValue(c));
        else if (cJSON_IsString(c)) parse_hex_color(cJSON_GetStringValue(c), &color);
        color.w = 1.0f;

        for (int i = 0; i < count; i++) {
            const vec3 position = {
                area->position.x + rand_int(-area->half.x, area->half.x),
                area->position.y + rand_int(0.0f, area->half.y),
                area->position.z + rand_int(-area->half.z, area->half.z),
            };
            const vec3 velocity = {spd * cosf(angle), 0.0f, spd * sinf(angle)};
            const VfxBlend mode = blend == 2 ? VFX_BLEND_ADDITIVE : blend == 3 ? VFX_BLEND_SUBTRACTIVE : VFX_BLEND_NORMAL;

            VfxSprite *sprite = sprite_add(position, texture, mode, size, velocity, grav * rand_float(0.7f, 1.3f) * 0.00001f, 0.0f,
                                           texture != VFX_TEX_GRASS);
            if (!sprite) break;

            sprite->area = vfx.area_count;
            sprite->color = color;
        }

        vfx.area_count++;
    }
}

// ---------------------------------------------------------------- simulation

static void update_area_sprite(VfxSprite *sprite, const float dt, const vec3 viewer) {
    const VfxArea *area = &vfx.areas[sprite->area];

    sprite->vis_timer -= dt;
    if (sprite->vis_timer <= 0.0f) {
        const float distance = v3_length(v3_sub(sprite->position, viewer));
        sprite->vis_timer = distance >= 2000.0f ? rand_int(5000.0f, 7000.0f) : distance >= 1000.0f ? rand_int(1500.0f, 2500.0f) :
                            distance >= 400.0f ? rand_int(1000.0f, 1200.0f) : rand_int(500.0f, 600.0f);
        sprite->hidden = distance > vfx.settings.particle_distance;
    }

    sprite->position = v3_add(sprite->position, v3_scale(sprite->velocity, dt));
    sprite->velocity.y -= sprite->grav * dt;

    const float l = sprite->size / 2.0f;

    if (sprite->position.x - l >= area->position.x + area->half.x) sprite->position.x = area->position.x - area->half.x - l;
    else if (sprite->position.x + l <= area->position.x - area->half.x) sprite->position.x = area->position.x + area->half.x + l;

    if (sprite->position.z - l >= area->position.z + area->half.z) sprite->position.z = area->position.z - area->half.z - l;
    else if (sprite->position.z + l <= area->position.z - area->half.z) sprite->position.z = area->position.z + area->half.z + l;

    if (sprite->position.y - l > area->position.y + area->half.y) {
        sprite->position.y = area->position.y - l;
        sprite->velocity.y = sprite->base_vy;
        sprite->hidden = 1;
    } else if (sprite->position.y + l < area->position.y) {
        sprite->position.y = area->position.y + area->half.y + l;
        sprite->velocity.y = sprite->base_vy;
        sprite->hidden = 1;
    }
}

static void update_sprites(const float dt, const vec3 viewer) {
    for (int i = 0; i < VFX_MAX_SPRITES; i++) {
        VfxSprite *sprite = &vfx.sprites[i];
        if (!sprite->active) continue;

        if (sprite->area >= 0) {
            if (vfx.settings.particles) {
                update_area_sprite(sprite, dt, viewer);
            } else { // shown again by the next distance check once particles are back on
                sprite->hidden = 1;
                sprite->vis_timer = 0.0f;
            }
            continue;
        }

        // 88.js Sprite.update: particles off hides every live sprite
        if (!vfx.settings.particles) {
            sprite->active = 0;
            continue;
        }

        sprite->position = v3_add(sprite->position, v3_scale(sprite->velocity, dt));
        sprite->velocity.y -= sprite->grav * dt;

        if (sprite->life > 0.0f) sprite->life -= dt;
        if (sprite->life <= 0.0f && sprite->updates) sprite->active = 0;
        sprite->updates++;
    }
}

static void update_boxes(const float dt) {
    for (int i = 0; i < VFX_MAX_BOXES; i++) {
        VfxBox *box = &vfx.boxes[i];
        if (!box->active) continue;

        box->position = v3_add(box->position, v3_scale(box->velocity, dt));
        box->roll += box->spin * dt;

        if (box->grav) {
            box->velocity.y -= box->grav * dt;
            if (box->look_dir) box->forward = box->velocity;
        }

        box->dst -= box->spd * dt;

        if (box->dst <= 0.0f) box->active = 0;
    }
}

static void update_trails(const float dt) {
    for (int i = 0; i < VFX_MAX_TRAILS; i++) {
        VfxTrail *trail = &vfx.trails[i];
        if (!trail->active) continue;

        if (trail->len < trail->dst) trail->len = fminf(trail->dst, trail->len + trail->spd * dt);

        trail->scale -= dt * VFX_TRAIL_SHRINK;
        if (trail->scale <= 0.0f) trail->active = 0;
    }
}

static void update_debris(const float dt_ms) {
    const float dt = dt_ms / 1000.0f;

    for (int i = 0; i < VFX_MAX_DEBRIS; i++) {
        VfxDebris *debris = &vfx.debris[i];
        if (!debris->active) continue;

        debris->life += dt * debris->life_speed;
        debris->velocity = v3_scale(debris->velocity, 1.0f - VFX_EXPLOSION_DAMPING * dt);
        debris->position = v3_add(debris->position, v3_scale(debris->velocity, dt));

        const float ratio = debris->life / sqrtf(2.0f);
        if ((2.0f - ratio * ratio) * debris->size <= 0.0f) debris->active = 0;
    }
}

void vfx_reset(void) {
    memset(vfx.sprites, 0, sizeof(vfx.sprites));
    memset(vfx.boxes, 0, sizeof(vfx.boxes));
    memset(vfx.trails, 0, sizeof(vfx.trails));
    memset(vfx.debris, 0, sizeof(vfx.debris));
    memset(vfx.muzzles, 0, sizeof(vfx.muzzles));
    memset(vfx.speed_lines, 0, sizeof(vfx.speed_lines));
    vfx.area_count = 0;
    vfx.screen_flash_active = 0;
    vfx.me = NULL;
    vfx.proj_me = NULL;
    projectiles_render_reset();
}

void vfx_update(Client *client, const float delta) {
    if (!vfx.initialized) {
        const char *log = getenv("KRUNKNATIVE_VFX_LOG");
        vfx.log = log && *log && *log != '0';

        const char *scale = getenv("KRUNKNATIVE_VFX_TIMESCALE");
        vfx.time_scale = scale && *scale ? (float) atof(scale) : 1.0f;
        if (vfx.time_scale <= 0.0f) vfx.time_scale = 1.0f;
    }

    vfx_read_settings();

    const Map *map = client->game.ready ? client->game.map : NULL;
    if (map != vfx.map || client->game.current_map_index != vfx.map_index) {
        if (vfx.log) fprintf(stderr, "[vfx] reset: map %p -> %p index %d -> %d\n", vfx.map, (const void *) map, vfx.map_index, client->game.current_map_index);
        vfx_reset();
        vfx.map = map;
        vfx.map_index = client->game.current_map_index;
        build_areas(map);
    }

    const float dt = fminf(fmaxf(delta, 0.0f) * 1000.0f, VFX_MAX_DELTA_MS) * vfx.time_scale;
    vfx.time_ms += dt;
    vfx.frame_ms = dt;

    // The one-frame muzzle sprites / overlay flash age before this frame's shot can re-arm them.
    for (int i = 0; i < 2; i++) {
        VfxMuzzle *muzzle = &vfx.muzzles[i];
        if (!muzzle->active) continue;

        muzzle->age += dt;
        if (muzzle->frames && muzzle->age >= VFX_FLASH_MIN_MS || muzzle->age >= VFX_FLASH_MAX_MS) muzzle->active = 0;
    }

    if (vfx.screen_flash_active) {
        vfx.screen_flash_age += dt;
        if (vfx.screen_flash_frames && vfx.screen_flash_age >= VFX_FLASH_MIN_MS || vfx.screen_flash_age >= VFX_FLASH_MAX_MS) {
            vfx.screen_flash_active = 0;
        }
    }

    // game.update: players.update (shoot() spawns the tracer, impacts and casings) runs before
    // particles.update of the same frame, so effects are advanced once before their first draw.
    // A new tracer is already spd * delta past the muzzle; drawn at the muzzle instead, its
    // centred 13-unit box would reach back past the camera. Remote shots ("9") arrive between
    // frames and are advanced the same way.
    if (map) detect_local_shot(client);

    update_sprites(dt, client->camera.position);
    update_boxes(dt);
    if (map) detect_local_projectile(client);
    projectiles_render_update(client, dt);
    update_trails(dt);
    update_debris(dt);
}

// ---------------------------------------------------------------- GL

static int vfx_gl_init(void) {
    if (vfx.initialized) return vfx.program != 0;
    vfx.initialized = 1;

    char *vertex_path = concat(assets_path(), "shaders/vfx.vert");
    char *fragment_path = concat(assets_path(), "shaders/vfx.frag");

    if (vertex_path && fragment_path) vfx.program = shader_compile(vertex_path, fragment_path);

    free(vertex_path);
    free(fragment_path);

    if (!vfx.program) {
        fprintf(stderr, "vfx: shader unavailable, effects disabled\n");
        return 0;
    }

    glGenVertexArrays(1, &vfx.vao);
    glGenBuffers(1, &vfx.vbo);

    glBindVertexArray(vfx.vao);
    glBindBuffer(GL_ARRAY_BUFFER, vfx.vbo);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VfxVertex), (void *) offsetof(VfxVertex, position));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VfxVertex), (void *) offsetof(VfxVertex, tex_coord));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(VfxVertex), (void *) offsetof(VfxVertex, color));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 1;
}

void vfx_fini(void) {
    projectiles_render_fini();
    if (vfx.vbo) glDeleteBuffers(1, &vfx.vbo);
    if (vfx.vao) glDeleteVertexArrays(1, &vfx.vao);
    if (vfx.program) glDeleteProgram(vfx.program);
    if (g_active_shader == vfx.program) g_active_shader = 0;

    free(vfx.vertices);
    free(vfx.draws);
    free(vfx.sort_items);

    // textures belong to load_texture()'s shared cache
    memset(&vfx, 0, sizeof(vfx));
    vfx.map_index = -1;
    vfx.me_loadout_index = -1;
}

static void batch_begin(void) {
    vfx.vertex_count = 0;
    vfx.draw_count = 0;
}

static int batch_state(const VfxDrawState state) {
    if (vfx.draw_count) {
        VfxDraw *last = &vfx.draws[vfx.draw_count - 1];
        if (!memcmp(&last->state, &state, sizeof(state))) return 1;
    }

    if (vfx.draw_count == vfx.draw_capacity) {
        const size_t capacity = vfx.draw_capacity ? vfx.draw_capacity * 2 : 64;
        VfxDraw *draws = realloc(vfx.draws, capacity * sizeof(VfxDraw));
        if (!draws) return 0;

        vfx.draws = draws;
        vfx.draw_capacity = capacity;
    }

    VfxDraw *draw = &vfx.draws[vfx.draw_count++];
    memset(draw, 0, sizeof(*draw));
    draw->state = state;
    draw->first = (int) vfx.vertex_count;
    return 1;
}

static void batch_vertex(const vec3 position, const float u, const float v, const vec4 color) {
    if (!vfx.draw_count) return;

    if (vfx.vertex_count == vfx.vertex_capacity) {
        const size_t capacity = vfx.vertex_capacity ? vfx.vertex_capacity * 2 : 1024;
        VfxVertex *vertices = realloc(vfx.vertices, capacity * sizeof(VfxVertex));
        if (!vertices) return;

        vfx.vertices = vertices;
        vfx.vertex_capacity = capacity;
    }

    VfxVertex *vertex = &vfx.vertices[vfx.vertex_count++];
    vertex->position[0] = position.x;
    vertex->position[1] = position.y;
    vertex->position[2] = position.z;
    vertex->tex_coord[0] = u;
    vertex->tex_coord[1] = v;
    vertex->color[0] = color.x;
    vertex->color[1] = color.y;
    vertex->color[2] = color.z;
    vertex->color[3] = color.w;
    vfx.draws[vfx.draw_count - 1].count++;
}

// Camera-facing quad (THREE.Sprite with sizeAttenuation): `right`/`up` are the
// camera axes in world space.
static void batch_sprite_quad(const vec3 center, const float size, const float rotation, const vec3 right, const vec3 up, const vec4 color) {
    const float h = size * 0.5f;
    const float c = cosf(rotation);
    const float s = sinf(rotation);
    const float corners[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    vec3 points[4];

    for (int i = 0; i < 4; i++) {
        const float x = corners[i][0] * c - corners[i][1] * s;
        const float y = corners[i][0] * s + corners[i][1] * c;
        points[i] = v3_add(center, v3_add(v3_scale(right, x), v3_scale(up, y)));
    }

    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) batch_vertex(points[order[i]], uvs[order[i]][0], uvs[order[i]][1], color);
}

// Lambert face colour like Krunker's default material: diffuse * (ambient +
// light * N.L) + emissive, using the map lighting basic.frag uses.
static vec4 lit_color(const vec4 color, const vec3 normal, const vec4 emissive) {
    if (!g_render_lighting.enabled) {
        return (vec4) {fminf(color.x + emissive.x, 1.0f), fminf(color.y + emissive.y, 1.0f), fminf(color.z + emissive.z, 1.0f), color.w};
    }

    const RenderLighting *l = &g_render_lighting;
    const float lambert = fmaxf(v3_dot(normal, v3_normalize(l->light_direction)), 0.0f);
    vec4 out = color;

    out.x = fminf(color.x * (l->ambient_color.x * l->ambient_intensity + l->light_color.x * l->light_intensity * lambert) + emissive.x, 1.0f);
    out.y = fminf(color.y * (l->ambient_color.y * l->ambient_intensity + l->light_color.y * l->light_intensity * lambert) + emissive.y, 1.0f);
    out.z = fminf(color.z * (l->ambient_color.z * l->ambient_intensity + l->light_color.z * l->light_intensity * lambert) + emissive.z, 1.0f);
    return out;
}

// Explosion debris: ambient-lit diffuse plus the equal emissive (see vfx_render_world).
static vec4 debris_color(const vec4 color) {
    const RenderLighting *l = &g_render_lighting;
    const float ai = l->enabled ? l->ambient_intensity : 1.0f;
    const vec4 ambient = l->enabled ? l->ambient_color : (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    return (vec4) {
        fminf(color.x * ambient.x * ai + color.x, 1.0f),
        fminf(color.y * ambient.y * ai + color.y, 1.0f),
        fminf(color.z * ambient.z * ai + color.z, 1.0f),
        color.w,
    };
}

// Box spanned by three edge vectors from `corner`; `lit` shades each face.
static void batch_box(const vec3 corner, const vec3 ex, const vec3 ey, const vec3 ez, const vec4 color, const int lit, const vec4 emissive) {
    const vec3 faces[6][4] = {
        {ex, v3_add(ex, ey), v3_add(v3_add(ex, ey), ez), v3_add(ex, ez)},  // +x
        {v3(0, 0, 0), ez, v3_add(ey, ez), ey},                             // -x
        {ey, v3_add(ey, ez), v3_add(v3_add(ex, ey), ez), v3_add(ex, ey)},  // +y
        {v3(0, 0, 0), ex, v3_add(ex, ez), ez},                             // -y
        {ez, v3_add(ex, ez), v3_add(v3_add(ex, ey), ez), v3_add(ey, ez)},  // +z
        {v3(0, 0, 0), ey, v3_add(ex, ey), ex},                             // -z
    };
    const vec3 normals[6] = {
        v3_normalize(ex), v3_scale(v3_normalize(ex), -1.0f),
        v3_normalize(ey), v3_scale(v3_normalize(ey), -1.0f),
        v3_normalize(ez), v3_scale(v3_normalize(ez), -1.0f),
    };
    static const int order[6] = {0, 1, 2, 0, 2, 3};

    for (int f = 0; f < 6; f++) {
        const vec4 face_color = lit ? lit_color(color, normals[f], emissive) : color;
        for (int i = 0; i < 6; i++) batch_vertex(v3_add(corner, faces[f][order[i]]), 0.0f, 0.0f, face_color);
    }
}

static void batch_phys_box(const VfxBox *box) {
    vec3 x, y, z;
    look_basis(box->forward, &x, &y, &z);

    if (box->roll) { // rotateX(roll): tumble around the local x axis
        const float c = cosf(box->roll);
        const float s = sinf(box->roll);
        const vec3 ny = v3_add(v3_scale(y, c), v3_scale(z, s));
        const vec3 nz = v3_sub(v3_scale(z, c), v3_scale(y, s));
        y = ny;
        z = nz;
    }

    const vec3 ex = v3_scale(x, box->size.x);
    const vec3 ey = v3_scale(y, box->size.y);
    const vec3 ez = v3_scale(z, box->size.z);
    const vec3 corner = v3_sub(box->position, v3_scale(v3_add(v3_add(ex, ey), ez), 0.5f));

    batch_box(corner, ex, ey, ez, box->color, box->lit, (vec4) {0});
}

// 88.js z: generateCube (base at y = 0) after lookAt + rotateX(PI / 2), so the
// box grows from the muzzle along the shot.
static void batch_trail(const VfxTrail *trail) {
    vec3 x, y, z;
    look_basis(trail->forward, &x, &y, &z);

    const vec3 ex = v3_scale(x, trail->scale);
    const vec3 ey = v3_scale(z, trail->len);
    const vec3 ez = v3_scale(y, -trail->scale);
    const vec3 corner = v3_sub(trail->start, v3_scale(v3_add(ex, ez), 0.5f));

    batch_box(corner, ex, ey, ez, color_from_hex(VFX_TRAIL_COLOR, VFX_TRAIL_OPACITY), 0, (vec4) {0});
}

static void camera_axes(const Camera *camera, vec3 *right, vec3 *up, vec3 *forward) {
    const float *m = camera->world_inverse_matrix; // row-major view matrix: rows are the camera axes
    *right = v3(m[0], m[1], m[2]);
    *up = v3(m[4], m[5], m[6]);
    *forward = v3(-m[8], -m[9], -m[10]);
}

static VfxDrawState sprite_state(const VfxBlend blend, const int texture, const int fog) {
    VfxDrawState state = {0};
    state.blend = (unsigned char) blend;
    state.fog = (unsigned char) fog;
    state.texture = vfx_texture(texture);
    state.use_texture = state.texture != 0;
    return state;
}

static void apply_blend(const unsigned char blend) {
    switch (blend) {
        case VFX_BLEND_OPAQUE:
            glDisable(GL_BLEND);
            break;
        case VFX_BLEND_ADDITIVE:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case VFX_BLEND_SUBTRACTIVE:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
            break;
        default:
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
    }
}

// Uploads the batch and draws it. GL state touched here is set explicitly and
// put back to the renderer defaults (main.c blend func, depth mask on, VAO 0).
static void batch_flush(Camera *camera, const int screen_space) {
    if (!vfx.vertex_count || !vfx.program) return;

    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    // Cull mode / front face are not queried: WebGL answers getParameter(CULL_FACE_MODE /
    // FRONT_FACE) with a synchronous GPU round trip (~3 ms per frame measured on the web),
    // and every pass leaves them at GL_BACK / GL_CCW (scene.c, shadow.c), which is what
    // this batch sets below and leaves behind.

    glUseProgram(vfx.program);
    g_active_shader = vfx.program;

    glUniform1i(glGetUniformLocation(vfx.program, "screen_space"), screen_space);
    glUniform1i(glGetUniformLocation(vfx.program, "tex"), VFX_TEXTURE_UNIT);

    if (!screen_space) {
        glUniformMatrix4fv(glGetUniformLocation(vfx.program, "camera_world_inverse"), 1, GL_TRUE, camera->world_inverse_matrix);
        glUniformMatrix4fv(glGetUniformLocation(vfx.program, "camera_projection"), 1, GL_TRUE, camera->projection_matrix);
    }

    const vec4 fog = g_render_lighting.fog_color;
    glUniform4f(glGetUniformLocation(vfx.program, "fog_color"), fog.x, fog.y, fog.z, fog.w);
    glUniform1f(glGetUniformLocation(vfx.program, "fog_near"), g_render_lighting.fog_near);
    glUniform1f(glGetUniformLocation(vfx.program, "fog_far"), g_render_lighting.fog_far);

    const int use_texture_location = glGetUniformLocation(vfx.program, "use_texture");
    const int fog_location = glGetUniformLocation(vfx.program, "fog_enabled");

    glBindVertexArray(vfx.vao);
    glBindBuffer(GL_ARRAY_BUFFER, vfx.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (vfx.vertex_count * sizeof(VfxVertex)), vfx.vertices, GL_STREAM_DRAW);

    glCullFace(GL_BACK);
    glFrontFace(GL_CCW); // batch_box/batch_sprite_quad emit counter-clockwise front faces
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
    if (screen_space) glDisable(GL_DEPTH_TEST);
    else glEnable(GL_DEPTH_TEST);

    glActiveTexture(GL_TEXTURE0 + VFX_TEXTURE_UNIT);
    // g_active_texture is only a hint: other passes (HTML engine, sky) bind unit 0 without
    // updating it, so the first textured draw of a batch always binds.
    unsigned int bound = 0;

    for (size_t i = 0; i < vfx.draw_count; i++) {
        const VfxDraw *draw = &vfx.draws[i];
        if (!draw->count) continue;

        apply_blend(draw->state.blend);
        if (draw->state.cull) glEnable(GL_CULL_FACE);
        else glDisable(GL_CULL_FACE);
        glDepthMask(draw->state.depth_write ? GL_TRUE : GL_FALSE);
        glUniform1i(use_texture_location, draw->state.use_texture);
        glUniform1i(fog_location, draw->state.fog && g_render_lighting.fog_enabled && !screen_space);

        if (draw->state.use_texture && bound != draw->state.texture) {
            glBindTexture(GL_TEXTURE_2D, draw->state.texture);
            g_active_texture = bound = draw->state.texture;
        }

        glDrawArrays(GL_TRIANGLES, draw->first, draw->count);
    }

    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (blend_was_enabled) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    if (depth_was_enabled) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (cull_was_enabled) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void prepare_camera(Camera *camera) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    camera_update_projection_matrix(camera, viewport[3] > 0 ? (float) viewport[2] / (float) viewport[3] : 1.0f);
    camera_update_world_inverse_matrix(camera);
}

static int compare_depth(const void *a, const void *b) {
    const float da = ((const VfxSortItem *) a)->depth;
    const float db = ((const VfxSortItem *) b)->depth;
    return da < db ? 1 : da > db ? -1 : 0; // far to near
}

static float view_distance(const vec3 position, const vec3 camera_position, const vec3 forward) {
    return v3_dot(v3_sub(position, camera_position), forward);
}

void vfx_render_world(Client *client) {
    if (!client->game.ready || !vfx_gl_init()) return;

    Camera *camera = &client->camera;
    prepare_camera(camera);

    // thrown knives: textured Lambert meshes through their own scene (projectiles_render.c)
    projectiles_render_draw_meshes(client, camera);

    vec3 right, up, forward;
    camera_axes(camera, &right, &up, &forward);

    batch_begin();

    // opaque: tracers, casings, projectiles, explosion debris
    VfxDrawState opaque = {0};
    opaque.blend = VFX_BLEND_OPAQUE;
    opaque.depth_write = 1;
    opaque.cull = 1;

    ProjectileBox projectiles[VFX_MAX_BOXES];
    const int projectile_count = projectiles_render_boxes(projectiles, VFX_MAX_BOXES);

    for (int pass = 0; pass < 2; pass++) { // unfogged basic boxes, then fogged lit ones
        opaque.fog = (unsigned char) pass;
        int started = 0;

        for (int i = 0; i < VFX_MAX_BOXES; i++) {
            const VfxBox *box = &vfx.boxes[i];
            if (!box->active || box->fog != pass) continue;
            if (!started) started = batch_state(opaque);
            batch_phys_box(box);
        }

        // projectile physObjs: MeshBasicMaterial boxes (fog: false) like the tracers
        for (int i = 0; i < projectile_count; i++) {
            const ProjectileBox *p = &projectiles[i];
            if (p->lit != pass) continue;

            VfxBox box = {0};
            box.position = p->position;
            box.forward = p->forward;
            box.roll = p->roll;
            box.size = p->size;
            box.color = p->color;
            box.lit = (unsigned char) p->lit;
            if (!started) started = batch_state(opaque);
            batch_phys_box(&box);
        }

        if (pass != 1) continue;

        const vec4 grey = color_from_hex(VFX_EXPLOSION_COLOR, 1.0f);
        const vec4 color = {grey.x * VFX_EXPLOSION_EMISSIVENESS, grey.y * VFX_EXPLOSION_EMISSIVENESS, grey.z * VFX_EXPLOSION_EMISSIVENESS, 1.0f};

        for (int i = 0; i < VFX_MAX_DEBRIS; i++) {
            const VfxDebris *debris = &vfx.debris[i];
            if (!debris->active) continue;

            const float ratio = debris->life / sqrtf(2.0f);
            const float edge = (2.0f - ratio * ratio) * debris->size;
            if (edge <= 0.0f) continue;

            if (!started) started = batch_state(opaque);
            const vec3 corner = v3_sub(debris->position, v3(edge * 0.5f, edge * 0.5f, edge * 0.5f));
            // 88.js: `emissive: H.multiplyScalar(0.55)` scales the shared colour too. The real
            // game shows these cubes as one flat grey (#989A9F) on every face: the debris
            // geometry gets no usable normals, so only ambient + emissive reach it.
            batch_box(corner, v3(edge, 0, 0), v3(0, edge, 0), v3(0, 0, edge), debris_color(color), 0, (vec4) {0});
        }
    }

    // transparent: sprites and trails, far to near
    size_t item_count = 0;
    const size_t needed = VFX_MAX_SPRITES + VFX_MAX_TRAILS;

    if (vfx.sort_capacity < needed) {
        VfxSortItem *items = realloc(vfx.sort_items, needed * sizeof(VfxSortItem));
        if (items) {
            vfx.sort_items = items;
            vfx.sort_capacity = needed;
        }
    }

    if (vfx.sort_items) {
        for (int i = 0; i < VFX_MAX_SPRITES; i++) {
            const VfxSprite *sprite = &vfx.sprites[i];
            if (!sprite->active || sprite->hidden) continue;
            vfx.sort_items[item_count++] = (VfxSortItem) {view_distance(sprite->position, camera->position, forward), 0, i};
        }

        for (int i = 0; i < VFX_MAX_TRAILS; i++) {
            const VfxTrail *trail = &vfx.trails[i];
            if (!trail->active) continue;
            const vec3 middle = v3_add(trail->start, v3_scale(trail->forward, trail->len * 0.5f));
            vfx.sort_items[item_count++] = (VfxSortItem) {view_distance(middle, camera->position, forward), 1, i};
        }

        qsort(vfx.sort_items, item_count, sizeof(VfxSortItem), compare_depth);
    }

    for (size_t i = 0; i < item_count; i++) {
        const VfxSortItem *item = &vfx.sort_items[i];

        if (item->kind == 0) {
            const VfxSprite *sprite = &vfx.sprites[item->index];
            if (!batch_state(sprite_state(sprite->blend, sprite->texture, 1))) break;
            batch_sprite_quad(sprite->position, sprite->size, sprite->rotation, right, up, sprite->color);
        } else {
            VfxDrawState state = {0};
            state.blend = VFX_BLEND_NORMAL;
            state.fog = 1;
            state.cull = 1;
            if (!batch_state(state)) break;
            batch_trail(&vfx.trails[item->index]);
        }
    }

    batch_flush(camera, 0);
}

void vfx_render_fps(Client *client, Camera *camera) {
    if (!client->game.ready || !vfx_gl_init()) return;

    Player *me = client->me;
    if (!me || !me->active || !vfx.settings.particles) return;

    const VfxWeaponFx *fx = weapon_fx(me->weapon);
    // 95.js hvJK5: a fully scoped scope weapon hides the weapon, muzzle sprites with it
    if (me->weapon && me->weapon->scope && me->aim_val >= 1.0f) return;

    prepare_camera(camera);

    vec3 right, up, forward;
    camera_axes(camera, &right, &up, &forward);

    batch_begin();

    for (int i = 0; i < 2; i++) {
        VfxMuzzle *muzzle = &vfx.muzzles[i];
        if (!muzzle->active) continue;

        vec3 world;
        if (!arms_point(me, muzzle->local, &world)) continue;

        muzzle->frames++;
        if (!batch_state(sprite_state(VFX_BLEND_NORMAL, muzzle->texture, 1))) break;
        batch_sprite_quad(world, muzzle->size, muzzle->rotation, right, up, (vec4) {1.0f, 1.0f, 1.0f, 1.0f});
    }

    batch_flush(camera, 0);
}

static void batch_screen_rect(const float x0, const float y0, const float x1, const float y1, const vec4 color) {
    // NDC rectangle; texture rows are stored bottom-first so v follows y
    const vec3 points[4] = {v3(x0, y0, 0.0f), v3(x1, y0, 0.0f), v3(x1, y1, 0.0f), v3(x0, y1, 0.0f)};
    const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) batch_vertex(points[order[i]], uvs[order[i]][0], uvs[order[i]][1], color);
}

// index.js scaleBase(): the overlay canvas draws in a 1700x900 * uiCustomScale design space
// (uiCustomScale = 2 - scaleUI) fitted to the window; c7 stays 1 with uiScaling off.
static float overlay_scale(const int width, const int height) {
    if (ui_setting_is_false("uiScaling")) return 1.0f;
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 2.0 - scale_ui;
    const double sx = width / (custom * 1700.0), sy = height / (custom * 900.0);
    return (float) (sy < sx ? sy : sx);
}

// 91.js ae.init(x, y, speed, canvasWidth)
static void speed_line_init(VfxSpeedLine *line, const vec2 center, const float speed, const float out_radius, const float strength) {
    line->center = center;
    line->speed = speed * (6.0f / 400.0f) + 2.0f;              // speed.map(0, 400, 2, 8)
    line->life = line->start_life = rand_int(500.0f, 1100.0f);
    line->alpha = rand_float(fmaxf(0.0f, strength - 0.4f), strength) * fminf(1.0f, (speed - 50.0f) / 50.0f);
    line->angle = (float) M_PI * rand_float(0.0f, 2.0f);
    line->size = speed * (399.0f / 350.0f) + 1.0f;             // speed.map(0, 350, 1, 400)
    line->in_radius = 550.0f - speed * 0.5f;                    // speed.map(0, 400, 550, 350)
    line->out_radius = out_radius;
}

// 91.js ae.update + render: a triangle from inRadius (apex) out to outRadius (base 2 * size
// wide), rotated by `angle` about the canvas centre. Canvas y points down.
static void speed_line_update(VfxSpeedLine *line, const float dt, const float scale, const int width, const int height, const vec4 color) {
    line->life -= line->speed * dt;
    line->in_radius += line->speed * dt;
    const float shrink = powf(fmaxf(line->life / line->start_life, 0.0f), dt / VFX_SPEED_LINE_FRAME_MS);
    line->alpha *= shrink;
    line->size *= shrink;
    if (line->alpha <= 0.0f) return;

    const float c = cosf(line->angle), s = sinf(line->angle);
    const float local[3][2] = {{0.0f, line->in_radius}, {line->size, line->out_radius}, {-line->size, line->out_radius}};
    const vec4 fill = {color.x, color.y, color.z, fminf(line->alpha, 1.0f)};

    for (int i = 0; i < 3; i++) {
        const float x = (line->center.x + local[i][0] * c - local[i][1] * s) * scale;
        const float y = (line->center.y + local[i][0] * s + local[i][1] * c) * scale;
        batch_vertex(v3(x / (float) width * 2.0f - 1.0f, 1.0f - y / (float) height * 2.0f, 0.0f), 0.0f, 0.0f, fill);
    }
}

// 91.js render: speed lines while the local player is active and moving fast enough.
static void batch_speed_lines(const Player *me, const int width, const int height) {
    const ClientRenderSettings *settings = client_render_settings();
    const float strength = settings ? settings->speed_lines : 0.0f;
    if (strength <= 0.0f || !me || !me->active) return;

    const float speed = sqrtf(me->velocity.x * me->velocity.x + me->velocity.z * me->velocity.z) * 500.0f;
    if (speed < VFX_SPEED_LINE_MIN) return; // below the threshold the pool neither moves nor draws

    vec4 color = hex_to_vec(0xe6e6e6);
    if (settings->speed_lines_color[0]) parse_hex_color(settings->speed_lines_color, &color);

    const float scale = overlay_scale(width, height);
    const float logical_width = (float) width / scale;
    const vec2 center = {logical_width * 0.5f, (float) height / scale * 0.5f};
    VfxDrawState state = {0};
    state.blend = VFX_BLEND_NORMAL;
    if (!batch_state(state)) return;

    for (int i = 0; i < VFX_SPEED_LINES; i++) {
        VfxSpeedLine *line = &vfx.speed_lines[i];
        if (line->life <= 0.0f) speed_line_init(line, center, speed, logical_width, strength);
        else speed_line_update(line, vfx.frame_ms, scale, width, height, color);
    }
}

void vfx_render_screen(Client *client) {
    if (!client->game.ready || !vfx_gl_init()) return;

    const Player *me = client->me;
    if (!me || !me->active) {
        vfx.screen_flash_active = 0;
        return;
    }

    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;

    const float aspect = (float) viewport[2] / (float) viewport[3];
    batch_begin();

    batch_speed_lines(me, viewport[2], viewport[3]); // first on the overlay canvas, under the flash

    // 91.js: only with the normal camera
    if (vfx.screen_flash_active && !camera_offset_active(client)) {
        const float cx = vfx.screen_flash_pos.x * 2.0f - 1.0f;
        const float cy = 1.0f - vfx.screen_flash_pos.y * 2.0f;
        const float hx = VFX_SCREEN_FLASH_SIZE;
        const float hy = VFX_SCREEN_FLASH_SIZE * aspect;

        if (batch_state(sprite_state(VFX_BLEND_NORMAL, VFX_TEX_SCREEN_FLASH, 0))) {
            batch_screen_rect(cx - hx, cy - hy, cx + hx, cy + hy, (vec4) {1.0f, 1.0f, 1.0f, 1.0f});
        }

        vfx.screen_flash_frames++;
    }

    // index.js gx: bloodOpacity = 1 - (health / maxHealth) / 0.8, hidden while a menu shows (91.js aA)
    if (vfx.settings.damage_overlay && me->max_health > 0 && !html_ui_is_menu_open()) {
        const float opacity = 1.0f - (me->health / (float) me->max_health) / 0.8f;

        if (opacity > 0.0f && batch_state(sprite_state(VFX_BLEND_NORMAL, VFX_TEX_BLOOD, 0))) {
            batch_screen_rect(-1.0f, -1.0f, 1.0f, 1.0f, (vec4) {1.0f, 1.0f, 1.0f, fminf(opacity, 1.0f)});
        }
    }

    batch_flush(&client->camera, 1);
}
