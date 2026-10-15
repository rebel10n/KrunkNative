// Projectile visuals: a port of Krunker v3.1.1's physObj projectiles (see
// client/include/projectiles_render.h). Krunker.Unpacked-v3.1.1 references:
//   88.js  D (physObj): init / update / disablePhys, lookAt + rotateX(spin), linger
//   129.js projectiles.stored[0]: the thrown knife (spd 0.38, grav 0.0002, spn 0.035,
//          scale 1, length 1, linger 5000)
//   27.js  weapon projectile blocks (mat BASIC, colour, scale, length, lookDir, dontPred)
//   55.js  loadMesh(meshConf, 0, 0, 0, PI / 2, 1, container): the knife model inside the
//          physObj container, rotation.y = PI / 2, scale 1
//   2.js   getMeleeConfig: melee/melee_<skin> with a Lambert material
// Units follow Krunker: world units, velocities per ms, gravity per ms^2, times in ms.
#include <flavor.h>
#include <strike_rules.h> // Krunker Strike projectile tints
#include <gl.h>
#include <projectiles_render.h>
#include <vfx.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

#define PROJ_MAX 64
// 95.js: the owner's predicted physObj flies 3000 units unless "pre" ends it first.
#define PROJ_LOCAL_DST 3000.0f
// A prediction waits this long for its server PROJECTILE before it stops linking.
#define PROJ_LINK_WINDOW_MS 1000.0f

// 129.js stored[0]
#define KNIFE_SPIN 0.035f
#define KNIFE_LINGER_MS 5000.0f
#define KNIFE_MODEL "models/melee/melee_0.obj"     // skins are not ported: melee_0 like player_mesh.c
#define KNIFE_TEXTURE "textures/melee/melee_0.png"

// 27.js projectile visuals (the flight values come from shared projectile_config_for()).
typedef struct {
    const char *src;
    int color;
    float scale;
    float length;
    unsigned char look_dir;
    unsigned char dont_pred;
} ProjVisualConfig;

static const ProjVisualConfig proj_visual_configs[] = {
    {"weapon_9", 0xffffdb, 1.4f, 10.0f, 1, 0},  // Rocket Launcher
    {"weapon_14", 0xffffdb, 0.7f, 10.0f, 1, 1}, // Crossbow (dontPred)
    {"weapon_19", 0x8b00fb, 0.7f, 10.0f, 1, 0}, // Blaster
    // v7.1.4 (WP-B) pj_color / pj_scale / pj_length
    {"weapon_23", 0xfbc02d, 1.4f, 15.0f, 1, 0}, // Noob Tube
    {"weapon_25", 0xfbc02d, 0.8f, 10.0f, 1, 0}, // Zapper
    {"weapon_27", 0xfbc02d, 1.4f, 15.0f, 1, 0}, // War Machine
};
// bullet-drop projectiles of the hitscan guns (projectile.disable unless config.bDrop)
static const ProjVisualConfig proj_visual_bullet = {NULL, 0xffffdb, 1.4f, 10.0f, 1, 0};

typedef struct {
    unsigned char active;
    unsigned char knife;
    unsigned char look_dir;
    unsigned char predicted;     // the owner's own prediction (physObj owid)
    unsigned char linked;        // server id known
    unsigned char own_unclaimed; // server visual of our own shot that arrived before the prediction
    int server_id;
    int weapon_index;
    float age;                   // ms since spawn
    vec3 position;
    vec3 velocity;               // xS, yS, zS per ms
    float spd;                   // scalar speed the distance counts down with
    float grav;
    float dst;
    float spin;
    float roll;
    vec3 forward;
    float linger;                // ms left stuck at the end point, 0 = flying
    vec3 size;
    vec4 color;
} ProjVisual;

static struct {
    ProjVisual visuals[PROJ_MAX];
    Scene *scene;
    MeshTransform containers[PROJ_MAX];
    Mesh *knives[PROJ_MAX];
    int knife_failed;
} proj;

// KRUNKNATIVE_VFX_LOG=1 (shared with vfx.c): print spawns, links and ends to stderr
static int proj_log(void) {
    static int log = -1;
    if (log < 0) {
        const char *value = getenv("KRUNKNATIVE_VFX_LOG");
        log = value && *value && *value != '0';
    }
    return log;
}

static vec3 pv3(const float x, const float y, const float z) {
    return (vec3) {x, y, z};
}

static float pv3_length(const vec3 a) {
    return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

static vec4 proj_color(const int hex) {
    return (vec4) {(float) (hex >> 16 & 0xff) / 255.0f, (float) (hex >> 8 & 0xff) / 255.0f, (float) (hex & 0xff) / 255.0f, 1.0f};
}

static const Weapon *proj_weapon(const Client *client, const int index) {
    const Game *game = &client->game;
    if (index < 0) return NULL;
    if (game->weapons && (size_t) index < game->weapon_count) return game->weapons[index];
    if (index < rules_krunker_weapon_count()) return rules_weapon(index);
    return NULL;
}

static const ProjVisualConfig *proj_visual_config(const Weapon *weapon) {
    if (!weapon || !weapon->src) return &proj_visual_bullet;

    for (size_t i = 0; i < sizeof(proj_visual_configs) / sizeof(proj_visual_configs[0]); i++) {
        if (!strcmp(proj_visual_configs[i].src, weapon->src)) return &proj_visual_configs[i];
    }

    return &proj_visual_bullet;
}

static ProjVisual *proj_alloc(void) {
    for (int i = 0; i < PROJ_MAX; i++) {
        if (!proj.visuals[i].active) return &proj.visuals[i];
    }

    return NULL;
}

// 88.js D.init (the parts after the velocity): configuration, lookAt the flight direction.
static ProjVisual *proj_spawn(const Client *client, const int weapon_index, const vec3 position, const vec3 velocity,
                              const float grav, const float dst) {
    const Weapon *weapon = proj_weapon(client, weapon_index);
    ProjVisual *visual = proj_alloc();
    if (!visual) return NULL;

    memset(visual, 0, sizeof(*visual));
    visual->active = 1;
    visual->server_id = -1;
    visual->weapon_index = weapon_index;
    visual->position = position;
    visual->velocity = velocity;
    visual->spd = pv3_length(velocity);
    visual->grav = grav;
    visual->dst = dst;
    visual->forward = velocity;
    visual->knife = (unsigned char) (weapon && weapon->melee);

    if (visual->knife) {
        visual->spin = KNIFE_SPIN; // stored[0] has no lookDir: the knife keeps its launch heading
        visual->size = pv3(1.0f, 1.0f, 1.0f);
    } else {
        const ProjVisualConfig *config = proj_visual_config(weapon);
        visual->look_dir = config->look_dir;
        visual->size = pv3(config->scale, config->scale, config->length);
        // Krunker Strike's own tints (pj_color, strike_rules.h projectile_colors: the Rocket Launcher's 0xfdebd0)
        const int strike_color = flavor_current()->rules ? strike_projectile_color(flavor_current()->rules, weapon_index) : -1;
        visual->color = proj_color(strike_color >= 0 ? strike_color : config->color);
    }

    if (proj_log()) {
        fprintf(stderr, "[proj] spawn w%d %s at %.1f %.1f %.1f v %.3f %.3f %.3f\n", weapon_index, visual->knife ? "knife" : "box",
                position.x, position.y, position.z, velocity.x, velocity.y, velocity.z);
    }
    return visual;
}

static int proj_dont_pred(const Client *client, const int weapon_index) {
    const Weapon *weapon = proj_weapon(client, weapon_index);
    return weapon && !weapon->melee && proj_visual_config(weapon)->dont_pred;
}

void projectiles_render_server_spawn(Client *client, const NetProjectilePacket *packet) {
    if (!packet || packet->id < 0) return;

    const Player *me = client->me;
    const int own = me && packet->owner_uid == me->uid;

    if (own && !proj_dont_pred(client, packet->weapon_index)) {
        // 129.js init: the owner gets no "pr" for a predicted projectile. Link the oldest
        // prediction of this weapon that is still waiting for its server id. A knife stuck in
        // its wall (linger) already had its end: linked back to the next throw, the knife in
        // the air lost its stick / removal and flew on through the map for 3000 units, while
        // the stuck one jumped to the next throw's end (v7 links by owid = shotCntP instead).
        ProjVisual *oldest = NULL;

        for (int i = 0; i < PROJ_MAX; i++) {
            ProjVisual *visual = &proj.visuals[i];
            if (!visual->active || !visual->predicted || visual->linked || visual->linger > 0.0f ||
                visual->weapon_index != packet->weapon_index) continue;
            if (visual->age > PROJ_LINK_WINDOW_MS) continue;
            if (!oldest || visual->age > oldest->age) oldest = visual;
        }

        if (oldest) {
            oldest->linked = 1;
            oldest->server_id = packet->id;
            if (proj_log()) fprintf(stderr, "[proj] linked prediction to server id %d\n", packet->id);
            return;
        }
    }

    // g9: physObj(x, y, z, yaw, pitch, projectile.range, false, null, null, projectile, sid, null, meleeIndex)
    const vec3 position = {packet->position[0], packet->position[1], packet->position[2]};
    const vec3 velocity = {packet->velocity[0], packet->velocity[1], packet->velocity[2]};
    ProjVisual *visual = proj_spawn(client, packet->weapon_index, position, velocity, packet->gravity, packet->range);
    if (!visual) return;

    visual->linked = 1;
    visual->server_id = packet->id;
    visual->own_unclaimed = (unsigned char) (own && !proj_dont_pred(client, packet->weapon_index));
}

// g8 -> disablePhys(id, own, impact, x, y, z) plus effect(x, y, z, 0, 0, 0) on an impact.
void projectiles_render_server_end(Client *client, const NetProjectileEndPacket *packet) {
    if (!packet) return;

    const vec3 end = {packet->position[0], packet->position[1], packet->position[2]};
    if (proj_log()) fprintf(stderr, "[proj] end id %d at %.1f %.1f %.1f impact %d\n", packet->id, end.x, end.y, end.z, packet->impact);

    for (int i = 0; i < PROJ_MAX; i++) {
        ProjVisual *visual = &proj.visuals[i];
        if (!visual->active || !visual->linked || visual->server_id != packet->id) continue;

        if (visual->knife && packet->impact) {
            // lingerTime && impact: stuck at the end point, lookAt(end - velocity)
            visual->position = end;
            visual->forward = pv3(-visual->velocity.x, -visual->velocity.y, -visual->velocity.z);
            visual->roll = 0.0f;
            visual->linger = KNIFE_LINGER_MS;
        } else {
            visual->active = 0;
        }

        visual->linked = 0;
        visual->server_id = -1;
    }

    if (packet->impact) vfx_impact(client, end, 0.0f, 0.0f);
}

// 95.js shoot(): z.physObj(x, eyeY, z, yaw, pitch, 3000, ..., projectile, -1, shotCntP[, meleeIndex])
// for the local player's knife throw (stored[0]) and its weapon projectiles (!dontPred).
void projectiles_render_local_shot(Client *client, const Player *me, const int thrown) {
    if (!me || !me->weapon) return;

    const int weapon_index = me->loadout && me->loadout_index >= 0 && me->loadout_index < me->loadout_size ? me->loadout[me->loadout_index] : -1;
    const ProjectileConfig *config = projectile_config_for(me->weapon, thrown);
    if (!config || weapon_index < 0) return;
    if (!thrown && proj_dont_pred(client, weapon_index)) return;

    // The server's visual of this shot may already be here (local server, no latency).
    for (int i = 0; i < PROJ_MAX; i++) {
        ProjVisual *visual = &proj.visuals[i];
        if (visual->active && visual->own_unclaimed && visual->weapon_index == weapon_index && visual->age <= PROJ_LINK_WINDOW_MS) {
            visual->own_unclaimed = 0;
            if (proj_log()) fprintf(stderr, "[proj] prediction claimed server id %d\n", visual->server_id);
            return;
        }
    }

    float yaw = me->direction.y;
    float pitch = me->direction.x;

    if (!thrown) {
        // 95.js: config.projSpreads[shotCntP % 12] scales the spread (same as player.c)
        static const float proj_spreads[] = {0.1f, -0.8f, 0.3f, 1.0f, 0.1f, -0.4f, -0.1f, 0.3f, 0.5f, -0.45f, 0.2f, -0.2f};
        const float spread = (me->spread + me->weapon->inaccuracy) * game_constants.spread_adjustment *
                             proj_spreads[me->shot_count % (sizeof(proj_spreads) / sizeof(proj_spreads[0]))];
        yaw += spread;
        pitch += me->recoil_anim_y * game_constants.recoil_mlt + spread;
    }

    // D.init: L -= PI; xS = spd * sin(L) * cos(M), zS = spd * cos(L) * cos(M), yS = spd * sin(M).
    // A knife throw passes its charge {d, v: max(0.3, d)} (players.js): spd * v, like the server's
    // knife. At full speed a quick throw's knife outran its server twin threefold, through walls.
    const float speed = config->speed * (thrown && me->throw_speed_mlt > 0.0f ? me->throw_speed_mlt : 1.0f);
    const float l = yaw - (float) M_PI;
    const vec3 velocity = {speed * sinf(l) * cosf(pitch), speed * sinf(pitch), speed * cosf(l) * cosf(pitch)};
    const vec3 origin = {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z};

    ProjVisual *visual = proj_spawn(client, weapon_index, origin, velocity, config->gravity, PROJ_LOCAL_DST);
    if (visual) visual->predicted = 1;
}

// 88.js D.update
void projectiles_render_update(Client *client, const float dt) {
    (void) client;
    if (dt <= 0.0f) return;

    for (int i = 0; i < PROJ_MAX; i++) {
        ProjVisual *visual = &proj.visuals[i];
        if (!visual->active) continue;

        visual->age += dt;

        if (visual->linger > 0.0f) {
            visual->linger -= dt;
            if (visual->linger <= 0.0f) visual->active = 0;
            continue;
        }

        visual->position.x += visual->velocity.x * dt;
        visual->position.y += visual->velocity.y * dt;
        visual->position.z += visual->velocity.z * dt;
        if (visual->spin) visual->roll += visual->spin * dt;

        if (visual->grav) {
            visual->velocity.y -= visual->grav * dt;
            if (visual->look_dir) visual->forward = visual->velocity;
        }

        visual->dst -= visual->spd * dt;
        if (visual->dst <= 0.0f) visual->active = 0;
    }
}

int projectiles_render_boxes(ProjectileBox *out, const int max) {
    int count = 0;

    for (int i = 0; i < PROJ_MAX && count < max; i++) {
        const ProjVisual *visual = &proj.visuals[i];
        if (!visual->active || visual->knife) continue;

        out[count++] = (ProjectileBox) {visual->position, visual->forward, visual->roll, visual->size, visual->color, 0};
    }

    return count;
}

static Mesh *proj_knife_mesh(const int index) {
    if (proj.knives[index]) return proj.knives[index];
    if (proj.knife_failed) return NULL;

    char *model_path = concat(assets_path(), KNIFE_MODEL);
    char *texture_path = concat(assets_path(), KNIFE_TEXTURE);
    Geometry *geometry = model_path ? load_obj_model(model_path, 0) : NULL; // cached, shared with player_mesh.c
    const unsigned int texture = texture_path ? load_texture(texture_path) : 0;
    free(model_path);
    free(texture_path);

    if (!geometry || !texture) {
        fprintf(stderr, "projectiles: knife model unavailable, thrown knives are not drawn\n");
        proj.knife_failed = 1;
        return NULL;
    }

    if (!proj.scene) proj.scene = scene_init();
    if (!proj.scene) return NULL;

    BasicMaterial *material = basic_material_init(); // getMeleeConfig: MeshLambertMaterial, lit and fogged
    if (!material) return NULL;
    material->texture = texture;

    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return NULL;
    }

    MeshTransform *container = &proj.containers[index];
    memset(container, 0, sizeof(*container));
    container->rotation_order = ROTATION_ORDER_YXZ;
    container->scale = pv3(1.0f, 1.0f, 1.0f); // scaleMesh(scale 1, scale 1, length 1)

    mesh->transform.parent = container;
    mesh->transform.rotation.y = (float) M_PI / 2.0f; // loadMesh(..., PI / 2, 1, container)
    mesh->visible = 0;

    proj.knives[index] = mesh;
    scene_add_mesh(proj.scene, mesh);
    return mesh;
}

// lookAt(position + forward) then rotateX(roll) as a three.js "YXZ" Euler:
// Ry(atan2(fx, fz)) * Rx(-asin(fy) + roll).
static void proj_container_pose(MeshTransform *container, const ProjVisual *visual) {
    const float length = pv3_length(visual->forward);
    const vec3 f = length > 1e-6f ? pv3(visual->forward.x / length, visual->forward.y / length, visual->forward.z / length) : pv3(0.0f, 0.0f, 1.0f);

    container->position = visual->position;
    container->rotation.y = atan2f(f.x, f.z);
    container->rotation.x = -asinf(fmaxf(-1.0f, fminf(1.0f, f.y))) + visual->roll;
    container->rotation.z = 0.0f;
}

void projectiles_render_draw_meshes(Client *client, Camera *camera) {
    (void) client;
    int any = 0;

    for (int i = 0; i < PROJ_MAX; i++) {
        const ProjVisual *visual = &proj.visuals[i];
        Mesh *mesh = visual->active && visual->knife ? proj_knife_mesh(i) : proj.knives[i];
        if (!mesh) continue;

        mesh->visible = visual->active && visual->knife;
        if (!mesh->visible) continue;

        proj_container_pose(&proj.containers[i], visual);
        any = 1;
    }

    if (any && proj.scene) scene_render(proj.scene, camera);
}

void projectiles_render_reset(void) {
    memset(proj.visuals, 0, sizeof(proj.visuals));
}

void projectiles_render_fini(void) {
    for (int i = 0; i < PROJ_MAX; i++) {
        if (!proj.knives[i]) continue;
        if (proj.scene) scene_remove_mesh(proj.scene, proj.knives[i]);
        mesh_fini(proj.knives[i]); // frees the material; geometry and texture stay in the loader caches
        proj.knives[i] = NULL;
    }

    if (proj.scene) {
        free(proj.scene->meshes);
        scene_fini(proj.scene);
    }

    memset(&proj, 0, sizeof(proj));
}
