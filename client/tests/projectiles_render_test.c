// Thrown-knife visuals (client/src/projectiles_render.c) over the "pr" / "pre" packet sequences of
// the owner's own throws: a knife stuck in a wall must not take the next throw's server id (its
// knife then never got its stick and flew on through the map), the owner's knife flies at the
// throw's charge speed, and an end without impact removes the knife. GL-free: the file is
// included and its mesh / scene / loader calls are stubbed below.
// Run: ./<build>/projectiles_render_test
#include "../src/projectiles_render.c"

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ---- stubs: the knife meshes are never drawn here
Geometry *load_obj_model(char *path, int flags) { (void) path; (void) flags; return NULL; }
unsigned int load_texture(char *path) { (void) path; return 0; }
Scene *scene_init() { return NULL; }
void scene_add_mesh(Scene *scene, Mesh *mesh) { (void) scene; (void) mesh; }
void scene_remove_mesh(Scene *scene, Mesh *mesh) { (void) scene; (void) mesh; }
void scene_render(const Scene *scene, Camera *camera) { (void) scene; (void) camera; }
void scene_fini(Scene *scene) { (void) scene; }
BasicMaterial *basic_material_init() { return NULL; }
void material_fini(Material *material) { (void) material; }
Mesh *mesh_init(Geometry *geometry, Material *material) { (void) geometry; (void) material; return NULL; }
void mesh_fini(Mesh *mesh) { (void) mesh; }
void vfx_impact(Client *client, const vec3 position, const float yaw, const float pitch) { (void) client; (void) position; (void) yaw; (void) pitch; }

static Client g_client;
static Player g_me;
static int g_loadout[3] = {0, 1, WEAPON_ID_KNIFE};

static int knives(int *flying, int *stuck) {
    *flying = *stuck = 0;
    for (int i = 0; i < PROJ_MAX; i++) {
        const ProjVisual *v = &proj.visuals[i];
        if (!v->active || !v->knife) continue;
        if (v->linger > 0.0f) (*stuck)++;
        else (*flying)++;
    }
    return *flying + *stuck;
}

static const ProjVisual *knife_at(const float x, const float y, const float z) {
    for (int i = 0; i < PROJ_MAX; i++) {
        const ProjVisual *v = &proj.visuals[i];
        if (v->active && v->knife && v->linger > 0.0f && fabsf(v->position.x - x) < 1e-3f && fabsf(v->position.y - y) < 1e-3f &&
            fabsf(v->position.z - z) < 1e-3f) return v;
    }
    return NULL;
}

// The local player throws (detect_local_projectile -> local shot), the server's "pr" follows.
static void throw_knife(const int server_id) {
    projectiles_render_local_shot(&g_client, &g_me, 1);
    const NetProjectilePacket spawn = {.id = server_id, .owner_uid = g_me.uid, .weapon_index = WEAPON_ID_KNIFE,
                                       .position = {0.0f, 10.0f, 0.0f}, .velocity = {0.0f, 0.0f, -0.38f}, .gravity = 0.0002f, .range = 1500.0f};
    projectiles_render_server_spawn(&g_client, &spawn);
}

static void end_knife(const int server_id, const float x, const float y, const float z, const int impact) {
    const NetProjectileEndPacket end = {.id = server_id, .position = {x, y, z}, .impact = (uint8_t) impact};
    projectiles_render_server_end(&g_client, &end);
}

static void test_relink(void) {
    printf("a stuck knife keeps its end, the next throw links its own knife...\n");
    projectiles_render_reset();
    int flying, stuck;

    throw_knife(0);
    projectiles_render_update(&g_client, 50.0f);
    end_knife(0, 0.0f, 10.0f, -20.0f, 1); // the first knife sticks in a wall 20 units away
    knives(&flying, &stuck);
    CHECK(flying == 0 && stuck == 1, "first knife stuck (flying %d, stuck %d)", flying, stuck);

    projectiles_render_update(&g_client, 200.0f); // well inside the 1 s link window
    throw_knife(0);                                // the server reuses slot 0
    projectiles_render_update(&g_client, 100.0f);
    end_knife(0, 30.0f, 12.0f, -40.0f, 1);
    knives(&flying, &stuck);
    CHECK(flying == 0, "no knife left flying after both ends (flying %d, stuck %d)", flying, stuck);
    CHECK(knife_at(0.0f, 10.0f, -20.0f) != NULL, "the first knife is still in its wall");
    CHECK(knife_at(30.0f, 12.0f, -40.0f) != NULL, "the second knife stuck at its own end");

    // a third throw that hits a player: no impact, its knife goes away, the stuck ones stay
    projectiles_render_update(&g_client, 100.0f);
    throw_knife(0);
    projectiles_render_update(&g_client, 50.0f);
    end_knife(0, 0.0f, 10.0f, -15.0f, 0);
    knives(&flying, &stuck);
    CHECK(flying == 0 && stuck == 2, "a player hit removes only its own knife (flying %d, stuck %d)", flying, stuck);

    // the stuck knives go after linger (stored[0].linger 5000 ms)
    projectiles_render_update(&g_client, 5000.0f);
    CHECK(knives(&flying, &stuck) == 0, "linger over: no knife left (flying %d, stuck %d)", flying, stuck);
}

static void test_charge_speed(void) {
    printf("the owner's knife flies at the throw's charge speed...\n");
    projectiles_render_reset();
    g_me.throw_speed_mlt = 0.3f; // a quick throw: v = max(0.3, d)
    projectiles_render_local_shot(&g_client, &g_me, 1);
    const ProjVisual *knife = NULL;
    for (int i = 0; i < PROJ_MAX; i++) {
        if (proj.visuals[i].active && proj.visuals[i].knife) knife = &proj.visuals[i];
    }
    CHECK(knife && fabsf(knife->spd - 0.38f * 0.3f) < 1e-5f, "spd 0.38 * 0.3 (got %.4f)", knife ? knife->spd : -1.0f);
    g_me.throw_speed_mlt = 1.0f;
}

int main(void) {
    g_me.uid = 7;
    g_me.active = 1;
    g_me.loadout = g_loadout;
    g_me.loadout_size = 3;
    g_me.loadout_index = 2;
    g_me.weapon = g_weapons[WEAPON_ID_KNIFE];
    g_me.height = 11.0f;
    g_me.throw_speed_mlt = 1.0f;
    g_client.me = &g_me;

    if (!g_me.weapon || !g_me.weapon->melee) {
        printf("projectiles_render_test: weapon %d is not the knife\n", WEAPON_ID_KNIFE);
        return 1;
    }

    test_relink();
    test_charge_speed();

    if (g_failures) {
        printf("projectiles_render_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("projectiles_render_test: %d checks OK\n", g_checks);
    return 0;
}
