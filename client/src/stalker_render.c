// Krunker v10.0.3 Stalker, the model (client/include/stalker_client.h, doc/v10/stalker.md).
//
// The look: modelOverrides.Stalker (deob-10.0.3.js:118195-118205) is applied when the model is built
// (player_mesh.c player_generate_meshes): every colour 0, no cosmetics, the melee_0_5 knife, and here
// opacity 0.1 on the parts genHead / genBody / genLeg / genArms make (`transparent: opacity != null,
// opacity`, deob-10.0.3.js:194337-194566); the knife keeps its own material (opaque).
//
// Hiding: a still stalker is isHidden after 300 ms (procInputs, deob-10.0.3.js:191772) and the server
// leaves it out of syncState (:222346), whose handler on the client hides every player first and shows
// those in the list (players.hideAll, UPDATE_PLAYERS :258580-258588). KrunkNative streams one STATE
// per player, so a client cannot see a player go missing from a list: it keeps v10's rule itself, from
// what it receives. A remote stalker whose received velocity is 0 and whose received position (the
// latest STATE's) has stopped changing for STALKER_HIDE_TIME is hidden (velocity alone would hide every stalker of an
// official lobby: their "k" carries no velocity, krunker_link.c); a STATE that moves shows it again.
// A hidden stalker's meshes leave the world scene for the draw (as spectate.c does for the followed
// player) and come back after it.
#include <stalker_client.h>
#include <stalker.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- the look

static void opacity_mesh(Mesh *mesh) {
    if (!mesh || !mesh->material) return;
    BasicMaterial *material = (BasicMaterial *) mesh->material;
    material->color.w = STALKER_OPACITY;
    material->base.transparent = 1;
}

static void opacity_cube(const ColorCube *cube) {
    for (size_t i = 0; cube && i < cube->mesh_count; i++) opacity_mesh(cube->meshes[i]);
}

void stalker_render_body_opacity(PlayerMesh *player_mesh, const size_t loadout_size) {
    if (!player_mesh) return;
    opacity_cube(player_mesh->head);
    opacity_cube(player_mesh->body);
    for (int i = 0; i < 2; i++) {
        opacity_cube(player_mesh->legs[i]);
        const PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
        if (!leg) continue;
        opacity_mesh(leg->upper);
        opacity_mesh(leg->joint);
        opacity_cube(leg->lower);
    }
    for (size_t w = 0; player_mesh->arms && w < loadout_size; w++) {
        const PlayerArms *arms = player_mesh->arms[w];
        if (!arms) continue;
        for (int j = 0; j < 2; j++) {
            const PlayerArmMesh *arm = j ? arms->left : arms->right;
            if (!arm) continue;
            opacity_mesh(arm->extender);
            opacity_mesh(arm->upper);
            opacity_mesh(arm->joint);
            opacity_cube(arm->lower);
        }
    }
}

// ---------------------------------------------------------------- hiding

#define HIDDEN_MAX 64

static struct {
    double last;               // glfwGetTime() of the last begin (the still clock's step)
    int count;                 // meshes taken out by the last begin
    const Player *player[HIDDEN_MAX];
    const PlayerMesh *mesh[HIDDEN_MAX];
    size_t loadout_size[HIDDEN_MAX];
} g_sr = {.last = -1.0};

int stalker_render_hidden(const Client *client, const Player *player) {
    if (!client || !player || !player->active) return 0;
    return player->is_hidden && player_is_stalker(&client->game, player);
}

static int vec3_equal(const vec3 a, const vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// the last received position (interp_pos_end) of each remote stalker, to tell a STATE that moved it
#define TRACK_MAX 128
typedef struct {
    const Player *player;
    int uid;
    vec3 end;
} StillTrack;
static StillTrack g_track[TRACK_MAX];
static int g_track_count;

// noMovT of a remote player from its states (the local player runs procInputs: shared/src/stalker.c):
// still while its received velocity is 0 and no STATE moved it (an unchanged interp_pos_end)
static void remote_still(Client *client, const float delta) {
    StillTrack next[TRACK_MAX];
    int count = 0;
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        if (!player || player == client->me) continue;
        if (!player->active || !player_is_stalker(&client->game, player)) {
            player->no_move_time = 0.0f;
            player->is_hidden = 0;
            continue;
        }
        const StillTrack *seen = NULL;
        for (int k = 0; k < g_track_count && !seen; k++) {
            if (g_track[k].player == player && g_track[k].uid == player->uid) seen = &g_track[k];
        }
        const int still = seen && vec3_equal(seen->end, player->interp_pos_end) && player->velocity.x == 0.0f &&
                          player->velocity.y == 0.0f && player->velocity.z == 0.0f;
        if (still) player->no_move_time += delta;
        else player->no_move_time = 0.0f;
        player->is_hidden = (unsigned char) (player->no_move_time >= STALKER_HIDE_TIME);
        if (count < TRACK_MAX) next[count++] = (StillTrack) {player, player->uid, player->interp_pos_end};
    }
    memcpy(g_track, next, (size_t) count * sizeof(StillTrack));
    g_track_count = count;
}

// a model built for the other side of isStalker (a team change the outfit check did not rebuild for)
static void refresh_looks(Client *client) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        const PlayerMesh *mesh = player ? player->mesh : NULL;
        if (!mesh || !player->active || mesh->stalker == (unsigned char) player_is_stalker(&client->game, player)) continue;
        const int render_you = mesh->render_you;
        client_drop_player_mesh(client, player);
        client_add_player_mesh(client, player, render_you);
    }
}

static int scene_holds(const Scene *scene, const PlayerMesh *player_mesh, const size_t loadout_size) {
    Scene parts = {0, NULL};
    scene_add_player_mesh(&parts, player_mesh, loadout_size);
    const Mesh *probe = parts.mesh_count ? parts.meshes[0] : NULL;
    free(parts.meshes);
    for (size_t i = 0; probe && i < scene->mesh_count; i++) {
        if (scene->meshes[i] == probe) return 1;
    }
    return 0;
}

void stalker_render_begin(Client *client) {
    g_sr.count = 0;
    if (!client || !client->scene) return;
    const double now = glfwGetTime();
    const float delta = g_sr.last >= 0.0 && now > g_sr.last ? (float) (now - g_sr.last) : 0.0f;
    g_sr.last = now;
    if (!client->game.ready || !client->game.mode || !client->game.mode->config.stalker_v10) return;
    remote_still(client, delta > 0.25f ? 0.25f : delta);
    refresh_looks(client);
    for (size_t i = 0; i < client->game.player_count && g_sr.count < HIDDEN_MAX; i++) {
        const Player *player = client->game.players[i];
        if (!player || player == client->me || !player->mesh || player->render_you || !stalker_render_hidden(client, player)) continue;
        const PlayerMesh *mesh = player->mesh;
        if (!scene_holds(client->scene, mesh, (size_t) player->loadout_size)) continue;
        scene_remove_player_mesh(client->scene, mesh, (size_t) player->loadout_size);
        g_sr.player[g_sr.count] = player;
        g_sr.mesh[g_sr.count] = mesh;
        g_sr.loadout_size[g_sr.count++] = (size_t) player->loadout_size;
    }
}

void stalker_render_end(Client *client) {
    const int count = g_sr.count;
    g_sr.count = 0;
    if (!client || !client->scene) return;
    for (int k = 0; k < count; k++) {
        // only while it is still the same player with the same meshes, and nothing put them back already
        for (size_t i = 0; i < client->game.player_count; i++) {
            const Player *player = client->game.players[i];
            if (player != g_sr.player[k] || player->mesh != g_sr.mesh[k]) continue;
            if (!scene_holds(client->scene, g_sr.mesh[k], g_sr.loadout_size[k]))
                scene_add_player_mesh(client->scene, (PlayerMesh *) g_sr.mesh[k], g_sr.loadout_size[k]);
            break;
        }
    }
}
