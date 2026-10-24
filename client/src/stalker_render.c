// Krunker v10.0.3 Stalker, the model (client/include/stalker_client.h, doc/v10/stalker.md).
//
// The look: modelOverrides.Stalker (deob-10.0.3.js:118195-118205) is applied when the model is built
// (player_mesh.c player_generate_meshes): every colour 0, no cosmetics, the melee_0_5 knife, and here
// opacity 0.1 on the parts genHead / genBody / genLeg / genArms make (`transparent: opacity != null,
// opacity`, deob-10.0.3.js:194337-194566); the knife keeps its own material (opaque).
//
// Hiding a still stalker needs nothing here: the server leaves a hidden player out of its player sync
// (server/src/net.c, `!isHidden` deob-10.0.3.js:222346) and the client's players.hideAll for that sync
// (NET_EXT_PLAYER_SYNC, main.c client_players_hide_all) keeps its model out of the world pass.
#include <stalker_client.h>
#include <stalker.h>

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

// ---------------------------------------------------------------- a stale look

// a model built for the other side of isStalker (a team change the outfit check did not rebuild for)
void stalker_render_frame(Client *client) {
    if (!client || !client->game.ready || !client->game.mode || !client->game.mode->config.stalker_v10) return;
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *player = client->game.players[i];
        const PlayerMesh *mesh = player ? player->mesh : NULL;
        if (!mesh || !player->active || mesh->stalker == (unsigned char) player_is_stalker(&client->game, player)) continue;
        const int render_you = mesh->render_you;
        client_drop_player_mesh(client, player);
        client_add_player_mesh(client, player, render_you);
    }
}
