// Krunker v7.1.4 zombies machines and airdrops in the world (map-logic WP-H, client side). The
// state is the Game.zombies mirror (shared/src/zombies.c, airdrop.c); this file only draws it.
//
//   MapManager.addMysteryBox: the box model (map-G's prefab mesh, Object.mesh) is shown while the
//     box is active, with outlineMesh 2 (a BackSide MeshBasicMaterial #AC69B9 copy at 1.025) and
//     the "mysterybox_beam" model (MeshBasicMaterial #AC69B9, additive, no depth write, alphaTest
//     0.05, no fog, both sides) at y + s[1] / 2, rotation r, scale (10, 1000, 10).
//   game.changeMysteryWeapon / updateMysteryBoxes: while rolling (state 1) the box shows a random
//     onlyWeapons weapon that changes every 250 ms (floor(timer / 250)) and the real pick from 80%
//     on (a teddy roll shows the teddy from the start, as in v7), rising from the box top by
//     (mysteryBoxOffset 10 - height) * progress; it stays where it stopped while it waits to be
//     taken (state 2). Weapons: "weapons/<src>" at scale * 1.5, rotation (xR, yR + PI/2, zR); the
//     teddy: "teddy_0" at scale 9, rotation r.
//   game.changeKrumWeapon / updateKrumMachines: while upgrading the weapon slides out along the
//     machine's facing (krumMachineOffset 10 * (p <= 0.5 ? 1 - p : p), p = timer / anim) at
//     y - height / 4, hidden while 0.3 < p < 0.7, the upgradeWeapons entry from p <= 0.5 on.
//   data/airDrop.js: a group at the drop's position holding "adrp_0" (0, -1.5, 0), rotation y
//     PI/2, scale 4, transparent and double-sided, coloured by the package (trapped packages
//     #e51b23; the server sends non-owners the package a trap imitates, so a trap looks like a
//     real package to them), rgb (hue cycle, rgbS 0.5) for animate packages. Falling, the group
//     bobs (x / z sin(bob) * 0.1) and turns (y += delta * 0.001); landed (readyClaim) it stops
//     tilting and switches to the "adrp_1" texture.
//
// KN notes: weapons have no altSkin in KN (no mid models, video projection or glow skins), so the
// upgraded weapon shows its base model. The airdrop overlay icon (game-overlay.js,
// /img/streaks/package/<icon>.png, ui/img from tools/ui-import/fetch_live_assets.py) is drawn by
// client/src/ui/hud_map.c with the death markers. The claim progress is the generic #interactProg bar (ui_interact.c).
#include <zombies_render.h>
#include <map_post.h>
#include <zombies.h>
#include <weapons.h>
#include <render_settings.h>
#include <audio.h>
#include <math.h>
#include <string.h>

#define ZR_MYSTERY_BOX_OFFSET 10.0f   // config.mysteryBoxOffset
#define ZR_KRUM_OFFSET 10.0f          // config.krumMachineOffset
#define ZR_BOX_COLOR 0xAC69B9         // the beam colour and outline material 2 (11299257)
#define ZR_OUTLINE_SCALE 1.025f       // loadMesh outlineScale default
#define ZR_UPGRADED_OUTLINE 65535     // outline material 0 (cyan) of every upgradeWeapons altSkin
#define ZR_UPGRADED_OUTLINE_SCALE 1.05f
#define ZR_TRAP_COLOR 0xe51b23        // airDropPackages trapped colour
#define ZR_AIRDROP_SCALE 4.0f
#define ZR_AIRDROP_RGB_SPEED 0.5f     // rgbS: hue degrees per ms

typedef struct {
    int uid;              // map object index (MapManager uid)
    const Object *object;
    Mesh *outline;        // mystery box outlineMesh
    Mesh *beam;           // mystery box beamMesh
    Mesh *weapon;         // wepMesh
    Mesh *weapon_outline; // the upgraded weapon's altSkin outline (0, outlineS 1.05)
    int weapon_id;        // wepMesh.weaponId (-1 the teddy), ZOMBIES_NONE without a mesh
    int stage;            // wepMesh.stage
} ZrMachine;

typedef struct {
    MeshTransform group;  // genObj3D: position and bob rotation
    Mesh *mesh;           // adrp_0, child of group
    int look;             // the package index the material was set up for, -1 none
    int landed;           // the adrp_1 texture is on
    float bob;            // bobAnim
    float last_fall;      // a fall that grows again is a new drop in the slot
    int was_active;
} ZrDrop;

static struct {
    Scene *scene;
    ZrMachine boxes[ZOMBIES_MAX_MACHINES];
    int box_count;
    ZrMachine krums[ZOMBIES_MAX_MACHINES];
    int krum_count;
    ZrDrop drops[AIRDROP_MAX];
    float hue;            // rgbMats dlt (degrees)
    int fire_sale;        // the fire sale ran last frame (its firesale_1 loop, zombie_events.c)
    uint32_t rng;         // the client's own picks (never the game's seeded RNG)
} g_zr;

static uint32_t zr_rand(void) {
    g_zr.rng = g_zr.rng * 1664525u + 1013904223u;
    return g_zr.rng >> 8;
}

static const Weapon *zr_weapon(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : g_weapons;
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

// weapons.onlyWeapons: not melee, not equipment, not customOnly
static int zr_random_weapon(const Game *game) {
    int options[64], count = 0;
    const size_t total = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    for (size_t id = 0; id < total && count < 64; id++) {
        const Weapon *w = zr_weapon(game, (int) id);
        if (w && w->src && !w->melee && !w->equipment && !w->custom_only) options[count++] = (int) id;
    }
    return count ? options[zr_rand() % (uint32_t) count] : -1;
}

// UTILS.hexFromHue
static vec4 zr_hue_color(const float hue) {
    const float h = hue / 60.0f;
    const float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    const int i = (int) floorf(h);
    const float r = i < 1 || i > 4 ? 1.0f : i == 1 || i == 4 ? x : 0.0f;
    const float g = i == 0 || i == 3 ? x : i == 1 || i == 2 ? 1.0f : 0.0f;
    const float b = i == 0 || i == 1 ? 0.0f : i == 3 || i == 4 ? 1.0f : x;
    return (vec4) {r, g, b, 1.0f};
}

static void zr_drop_mesh(Mesh **mesh) {
    if (!*mesh) return;
    if (g_zr.scene) scene_remove_mesh(g_zr.scene, *mesh);
    mesh_fini(*mesh); // the material too; geometry and textures are cached (shared)
    *mesh = NULL;
}

static void zr_add(Mesh *mesh) {
    if (mesh && g_zr.scene) scene_add_mesh(g_zr.scene, mesh);
}

// loadMesh outlineMesh: the same geometry, BackSide MeshBasicMaterial, scaled about the model
static Mesh *zr_outline(Mesh *base, const int color, const float scale) {
    if (!base) return NULL;
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    material->unlit = 1;
    material->double_sided = 0;
    material->cull_front = 1;
    material->color = hex_to_vec(color);
    Mesh *mesh = mesh_init(base->geometry, (Material *) material);
    mesh->transform.parent = &base->transform;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    mesh->no_shadow_receive = 1;
    mesh->visible = 0;
    return mesh;
}

static Mesh *zr_beam(const Object *object) {
    Mesh *mesh = prefab_asset_mesh("mysterybox_beam", "mysterybox_beam");
    if (!mesh) return NULL;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    material->unlit = 1;            // MeshBasicMaterial
    material->fog_enabled = 0;      // noFog
    material->base.transparent = 1;
    material->no_depth_write = 1;
    material->alpha_test = 0.05f;
    material->additive_blend = 1;   // blending 2: AdditiveBlending
    material->double_sided = 1;     // side 2
    material->color = hex_to_vec(ZR_BOX_COLOR);
    mesh->transform.position = (vec3) {object->position.x, object->position.y + object->scale.y * 0.5f, object->position.z};
    mesh->transform.rotation = object->rotation;
    mesh->transform.scale = (vec3) {10.0f, 1000.0f, 10.0f}; // loadMesh scale 10, then scaleMesh(10, 1000, 10)
    mesh->no_shadow_receive = 1;
    mesh->visible = 0;
    return mesh;
}

void zombies_render_unload(void) {
    for (int k = 0; k < 2; k++) {
        ZrMachine *list = k ? g_zr.krums : g_zr.boxes;
        const int count = k ? g_zr.krum_count : g_zr.box_count;
        for (int i = 0; i < count; i++) {
            zr_drop_mesh(&list[i].weapon_outline);
            zr_drop_mesh(&list[i].weapon);
            zr_drop_mesh(&list[i].beam);
            zr_drop_mesh(&list[i].outline);
        }
    }
    for (int i = 0; i < AIRDROP_MAX; i++) zr_drop_mesh(&g_zr.drops[i].mesh);
    if (g_zr.fire_sale) audio_stop("firesale_1"); // a map change ends the sale's loop with it
    const uint32_t rng = g_zr.rng;
    memset(&g_zr, 0, sizeof(g_zr));
    g_zr.rng = rng;
}

void zombies_render_load(Scene *scene, const Map *map) {
    zombies_render_unload();
    g_zr.scene = scene;
    if (!g_zr.rng) g_zr.rng = 0x9e3779b9u;
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *object = map->objects[i];
        const int box = object->prefab == PREFAB_MYSTERY_BOX;
        if (!box && object->prefab != PREFAB_KRUM_MACHINE) continue;
        if (box ? g_zr.box_count >= ZOMBIES_MAX_MACHINES : g_zr.krum_count >= ZOMBIES_MAX_MACHINES) continue;
        ZrMachine *m = box ? &g_zr.boxes[g_zr.box_count++] : &g_zr.krums[g_zr.krum_count++];
        memset(m, 0, sizeof(*m));
        m->uid = (int) i;
        m->object = object;
        m->weapon_id = ZOMBIES_NONE;
        m->stage = -1;
        if (box) {
            m->outline = zr_outline(object->mesh, ZR_BOX_COLOR, ZR_OUTLINE_SCALE);
            zr_add(m->outline);
            m->beam = zr_beam(object);
            zr_add(m->beam);
        }
    }
    for (int i = 0; i < AIRDROP_MAX; i++) g_zr.drops[i].look = -1;
}

// ---------------------------------------------------------------- machines

// changeMysteryWeapon / changeKrumWeapon: a new wepMesh for (weapon, stage)
static void zr_machine_weapon(const Game *game, ZrMachine *m, const int id, const int stage, const int krum) {
    zr_drop_mesh(&m->weapon_outline);
    zr_drop_mesh(&m->weapon);
    m->weapon_id = id;
    m->stage = stage;
    const vec3 r = m->object->rotation;
    if (id == -1) { // the teddy
        m->weapon = prefab_asset_mesh("teddy_0", "teddy_0");
        if (!m->weapon) return;
        m->weapon->transform.rotation = r;
        m->weapon->transform.scale = (vec3) {9.0f, 9.0f, 9.0f};
        m->weapon->no_shadow_receive = 1; // shadowsR false
    } else {
        const Weapon *weapon = krum && stage ? weapon_upgraded(id) : zr_weapon(game, id);
        if (!weapon || !weapon->src) weapon = zr_weapon(game, id);
        m->weapon = prefab_weapon_mesh(weapon, 0);
        if (!m->weapon) return;
        const float scale = (weapon->scale != 0.0f ? weapon->scale : 1.0f) * 1.5f; // (mScl || scale) * 1.5
        m->weapon->transform.rotation = (vec3) {r.x, r.y + (float) M_PI / 2.0f, r.z};
        m->weapon->transform.scale = (vec3) {scale, scale, scale};
    }
    zr_add(m->weapon);
    if (krum && stage && m->weapon) { // the upgraded altSkin's outline (its video projection texture is not ported)
        m->weapon_outline = zr_outline(m->weapon, ZR_UPGRADED_OUTLINE, ZR_UPGRADED_OUTLINE_SCALE);
        zr_add(m->weapon_outline);
    }
}

// The box mesh sits at y + s[1] / 2 - 1; its GameObject height is s[1] / 2.
static void zr_box_frame(const Game *game, ZrMachine *m) {
    const Object *o = m->object;
    const ZombieMachine *state = zombies_machine(game, m->uid);
    const int active = o->active;
    if (o->mesh) ((Mesh *) o->mesh)->visible = active; // meshRef.visible = active
    if (m->outline) m->outline->visible = active;
    if (m->beam) m->beam->visible = active;

    const int s = state ? state->state : 0;
    if (s == 1) {
        const float height = o->scale.y * 0.5f;
        const float progress = 1.0f - state->timer / ZOMBIES_MYSTERY_BOX_ANIM;
        const int force = progress >= 0.8f;
        const int stage = (int) floorf(state->timer / 250.0f);
        const int pending = state->pending_weapon;
        if (!m->weapon || m->stage != stage || force) {
            const int id = force || pending == -1 ? pending : zr_random_weapon(game);
            // v7 builds a new mesh only for another weapon: the same pick keeps the old stage and
            // draws again next frame
            if (id != ZOMBIES_NONE && (!m->weapon || m->weapon_id != id)) zr_machine_weapon(game, m, id, stage, 0);
        }
        if (m->weapon) {
            const float y = o->position.y + o->scale.y * 0.5f - 1.0f;
            m->weapon->transform.position = (vec3) {o->position.x, y + height + (ZR_MYSTERY_BOX_OFFSET - height) * progress, o->position.z};
        }
    }
    if (m->weapon) m->weapon->visible = active && s >= 1;
}

// The krum machine mesh sits at y + s[1] / 2; its GameObject height is s[1] / 2.
static void zr_krum_frame(const Game *game, ZrMachine *m) {
    const Object *o = m->object;
    const ZombieMachine *state = zombies_machine(game, m->uid);
    const int s = state ? state->state : 0;
    int visible = o->active && s >= 1;
    if (s == 1 && state->pending_weapon >= 0) {
        const float height = o->scale.y * 0.5f;
        const float progress = state->timer / ZOMBIES_KRUM_ANIM;
        const float offset = ZR_KRUM_OFFSET * (progress <= 0.5f ? 1.0f - progress : progress);
        const float angle = -o->rotation.y + (float) M_PI / 2.0f;
        const int stage = progress <= 0.5f ? 1 : 0;
        if (!m->weapon || m->weapon_id != state->pending_weapon || m->stage != stage) zr_machine_weapon(game, m, state->pending_weapon, stage, 1);
        if (m->weapon) {
            m->weapon->transform.position = (vec3) {o->position.x - offset * sinf(angle), o->position.y + height - height / 4.0f,
                                                    o->position.z - offset * cosf(angle)};
        }
        visible = visible && (progress >= 0.7f || progress <= 0.3f);
    }
    if (m->weapon) m->weapon->visible = visible;
    if (m->weapon_outline) m->weapon_outline->visible = visible;
}

// ---------------------------------------------------------------- airdrops

static void zr_drop_look(ZrDrop *v, const int look) {
    BasicMaterial *material = (BasicMaterial *) v->mesh->material;
    const AirDropPackage *package = &g_airdrop_packages[look];
    material->color = package->imitate ? hex_to_vec(ZR_TRAP_COLOR) : (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    material->emissive = (vec4) {0.0f, 0.0f, 0.0f, 0.0f};
    v->look = look;
}

static void zr_drops_frame(const Client *client, const float delta_ms) {
    const GameZombies *z = &client->game.zombies;
    const int me = client->me ? client->me->uid : -1;
    for (int i = 0; i < AIRDROP_MAX; i++) {
        ZrDrop *v = &g_zr.drops[i];
        const AirDrop *d = i < z->drop_count ? &z->drops[i] : NULL;
        if (!d || !d->active) {
            if (v->mesh) v->mesh->visible = 0; // remove: the mesh leaves the scene
            v->was_active = 0;
            continue;
        }
        const int fresh = !v->was_active || d->fall > v->last_fall;
        v->was_active = 1;
        v->last_fall = d->fall;
        if (!v->mesh) {
            v->mesh = prefab_asset_mesh("adrp_0", "adrp_0");
            if (!v->mesh) continue;
            BasicMaterial *material = (BasicMaterial *) v->mesh->material;
            material->base.transparent = 1;
            material->double_sided = 1;
            v->mesh->transform.parent = &v->group;
            v->mesh->transform.position = (vec3) {0.0f, -1.5f, 0.0f};
            v->mesh->transform.rotation = (vec3) {0.0f, (float) M_PI / 2.0f, 0.0f};
            v->mesh->transform.scale = (vec3) {ZR_AIRDROP_SCALE, ZR_AIRDROP_SCALE, ZR_AIRDROP_SCALE};
            zr_add(v->mesh);
            v->look = -1;
        }
        if (fresh) { // AirDrop.init: a new drop in this slot
            v->bob = 0.0f;
            v->group.rotation = (vec3) {0.0f, 0.0f, 0.0f};
            v->group.scale = (vec3) {1.0f, 1.0f, 1.0f};
            v->landed = -1;
        }
        // the owner sees the real package, everyone else what it imitates (the server already
        // sends mirrors the imitated index; an authority game still holds both)
        const int look = d->imitate >= 0 && me != d->owner_uid ? d->imitate : d->index;
        if (look >= 0 && look < AIRDROP_PACKAGE_COUNT && look != v->look) zr_drop_look(v, look);
        const int landed = !!d->can_claim;
        if (landed != v->landed) { // readyClaim: meshRef.material = secMat ("adrp_1")
            BasicMaterial *material = (BasicMaterial *) v->mesh->material;
            if (!client_render_settings()->no_textures) material->texture = prefab_asset_texture(landed ? "adrp_1" : "adrp_0");
            v->landed = landed;
        }
        if (v->look >= 0 && g_airdrop_packages[v->look].animate) ((BasicMaterial *) v->mesh->material)->emissive = zr_hue_color(g_zr.hue);

        v->group.position = d->position;
        if (landed) {
            v->group.rotation.x = 0.0f;
            v->group.rotation.z = 0.0f;
        } else {
            v->bob += delta_ms * 0.001f;
            v->group.rotation.x = sinf(v->bob) * 0.1f;
            v->group.rotation.z = sinf(v->bob) * -0.1f;
            v->group.rotation.y += delta_ms * 0.001f;
        }
        v->mesh->visible = 1;
    }
}

void zombies_render_frame(Client *client, const float delta_ms) {
    if (!client || !g_zr.scene || !client->game.ready) return;
    const Game *game = &client->game;
    // io_updatePowerUp stops the fire sale loop on "pwup 1, 0"; a sale cut short by the round end
    // (finishInit zeroes the timers without a packet) stops it here
    const int fire_sale = game->zombies.powerups[ZOMBIES_POWERUP_FIRE_SALE] > 0.0f;
    if (g_zr.fire_sale && !fire_sale) audio_stop("firesale_1");
    g_zr.fire_sale = fire_sale;
    g_zr.hue += delta_ms * ZR_AIRDROP_RGB_SPEED;
    if (g_zr.hue >= 360.0f) g_zr.hue = 0.0f;
    for (int i = 0; i < g_zr.box_count; i++) zr_box_frame(game, &g_zr.boxes[i]);
    for (int i = 0; i < g_zr.krum_count; i++) zr_krum_frame(game, &g_zr.krums[i]);
    zr_drops_frame(client, delta_ms);
}
