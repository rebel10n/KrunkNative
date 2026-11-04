// The local player's own shadow in first person (client/include/rt.h rt_set_self_caster / rt_self_frame).
//
// Krunker's first-person model is arms and a weapon only (player_generate_meshes with render_you: no body,
// head or legs are built; v10 genArms), drawn by the viewmodel pass from the camera. So nothing of the local
// player exists in the world scene for the tracer: other players cast shadows, you do not. This module keeps
// a "shadow puppet": a third-person model of the local player (player_generate_meshes(copy, 0), the same
// build another client makes of us: body, head, legs, cosmetics, the held weapon in third-person arms), posed
// every frame by player_update_meshes on a copy of the live player state, exactly as a remote player's model
// is posed from its state (running legs, crouch, the look's pitch on the upper body, the weapon in hand).
// Its meshes never join a Scene: rt_scene_build traces them as RT_INST_SHADOW_ONLY casters (shadow / AO /
// local-light rays hit them; the prepass never draws them; closest-hit rays, i.e. reflections and GI, pass
// through, so you never meet your own body in a mirror, which a first-person view has no pose data to make
// right anyway).
//
// The puppet is a calloc'd Player overwritten from `me` each frame (every pointer it holds, loadout, game,
// weapon, is the live player's, read only here); only `mesh` and render_you are its own. Rebuilt when the
// live model was rebuilt (me->mesh changed: respawn, class / skin change, a third-person toggle), freed with
// the map (its geometry and weapons go with it) and at shutdown. No pet (its look drops petIndex) and no
// scene registration, so nothing of it is drawn or ticked anywhere else.
#include <gl.h>
#include <rt.h>
#include <pets.h>
#include <render_settings.h>
#include <stdlib.h>
#include <string.h>

static struct {
    Player *me;              // this frame's local player (rt_set_self_caster), NULL = none
    int enabled;             // rt_set_self_caster's flag for this frame
    Player *puppet;          // the shadow puppet (owns only puppet->mesh)
    const void *built_for;   // me->mesh the puppet was built alongside (a rebuild of the live model rebuilds it)
    int built_loadout_size;
    int swapped_index;       // the loadout slot the puppet's meshes show (player_swap_weapon), -1 none
    Scene probe;             // the puppet's meshes for rt_scene_build (a lasting-0 probe: no pet, no class-model memory)
} g_self;

void rt_set_self_caster(Player *me, const int enabled) {
    g_self.me = me;
    g_self.enabled = enabled && me != NULL;
}

static void puppet_free(void) {
    if (g_self.puppet) {
        if (g_self.puppet->mesh) player_meshes_fini(g_self.puppet);
        free(g_self.puppet);
        g_self.puppet = NULL;
    }
    g_self.built_for = NULL;
    g_self.swapped_index = -1;
    free(g_self.probe.meshes);
    g_self.probe.meshes = NULL;
    g_self.probe.mesh_count = 0;
}

// The live state onto the puppet, keeping what is the puppet's own.
static void puppet_sync(Player *puppet, const Player *me) {
    PlayerMesh *mesh = puppet->mesh;
    *puppet = *me;
    puppet->mesh = mesh;
    puppet->render_you = 0;
    puppet->cosmetics_v7.pet = -1; // no pet: the live model's pet already follows the player in the world scene
    puppet->uid = -1;              // Krunker's preview sid: no world particles of its cosmetics (the live model spawns them)
}

const Scene *rt_self_frame(void) {
    Player *me = g_self.me;
    g_self.me = NULL; // one frame's hand-over
    static int no_self = -1; // dev: KRUNKNATIVE_RT_NO_SELF=1 drops the proxy (A/B captures)
    if (no_self < 0) {
        const char *env = getenv("KRUNKNATIVE_RT_NO_SELF");
        no_self = env && *env && *env != '0';
    }
    if (!g_self.enabled || !me || no_self || client_render_settings()->rt_mode == RT_MODE_OFF) {
        g_self.probe.mesh_count = 0;
        return NULL;
    }
    if (!me->mesh || !me->loadout || me->loadout_size <= 0 || !me->game || !me->game->map) {
        g_self.probe.mesh_count = 0;
        return NULL;
    }

    // (re)build alongside the live model
    if (g_self.puppet && (g_self.built_for != me->mesh || g_self.built_loadout_size != me->loadout_size)) puppet_free();
    if (!g_self.puppet) {
        Player *puppet = calloc(1, sizeof(Player));
        if (!puppet) return NULL;
        puppet->mesh = NULL;
        puppet_sync(puppet, me);
        player_generate_meshes(puppet, 0); // third person: the model another client sees of us
        if (!puppet->mesh) {
            free(puppet);
            return NULL;
        }
        PlayerMesh *pm = puppet->mesh;
        if (pm->pet) { // generate_meshes made one if the look still had a pet (it should not): never shown, drop it
            pet_free(pm->pet);
            pm->pet = NULL;
        }
        g_self.puppet = puppet;
        g_self.built_for = me->mesh;
        g_self.built_loadout_size = me->loadout_size;
        g_self.swapped_index = -1;
    }

    Player *puppet = g_self.puppet;
    puppet_sync(puppet, me);
    if (g_self.swapped_index != me->loadout_index) { // the slot's meshes shown / hidden like a remote swap
        player_swap_weapon(puppet, me->loadout_index, 1, 1, 1);
        g_self.swapped_index = me->loadout_index;
        puppet_sync(puppet, me); // swap_weapon's own timers / weapon pointer back to the live ones
    }
    player_update_meshes(puppet, 0);

    // the meshes scene_render would draw of this model (scene.c's probe: lasting 0 adds no pet, remembers no scene)
    g_self.probe.mesh_count = 0;
    g_self.probe.lasting = 0;
    scene_add_player_mesh(&g_self.probe, puppet->mesh, (size_t) puppet->loadout_size);
    return g_self.probe.mesh_count ? &g_self.probe : NULL;
}

void rt_self_unload_map(void) {
    puppet_free();
    g_self.enabled = 0;
}

void rt_self_fini(void) {
    puppet_free();
    memset(&g_self, 0, sizeof(g_self));
}
