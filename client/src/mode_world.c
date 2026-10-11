// In-world pieces of the game modes, driven by the mode state the client mirrors
// (hud_modes.c / Game.mode). Krunker v3.1.1:
//   106.js setObjective: only the active objective's zone mesh is visible
//     (the map builder adds every OBJECTIVE zone hidden, client/src/prefab.c);
//   objects.js addFlag + 106.js updateFlags / updateFlagCol: CTF flags are a crystal_0
//     model (crystal_1 / emissive 4360861 for your team, crystal_0 / 8729652 for the
//     enemy) over a zone_r / zone_b cube without top and bottom; the crystal bobs and
//     spins, rides above its carrier (flagHOff) and hides with the carrier's model;
//   106.js createSpawnable: Kill Confirmed tags are krunkie_0 models (noFog, emissive by
//     team) that bob and spin the same way.
// Everything is created lazily for the current map + mode and removed on change.
#include <gl.h>
#include <mode_world.h>
#include <hud_modes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

Player *client_find_player(Client*, int uid); // client/src/main.c

#define MODE_WORLD_EMISSIVE_ENEMY 8729652 // getFlagDat "0" / tag emissive (your team's tag)
#define MODE_WORLD_EMISSIVE_ALLY 4360861  // getFlagDat "1" / tag emissive (enemy tag)

typedef struct {
    Mesh *crystal;
    Mesh *zone;
    float bob;
    float spin;
    int team_shown;          // the local team the textures were picked for, -1 none
} ModeWorldFlag;

typedef struct {
    Mesh *mesh;
    int sid;
    float bob;
    float spin;
} ModeWorldTag;

static struct {
    const Map *map;
    int mode_id;
    Scene *scene;
    int flag_count;
    ModeWorldFlag flags[MODE_MAX_FLAGS];
    ModeWorldTag tags[MODE_MAX_SPAWNABLES];
    Geometry *zone_geometry;     // 4 side faces (addCube faces [1, 1, 0, 0, 1, 1])
    unsigned int zone_vbo;
    Geometry *crystal_geometry;
    Geometry *tag_geometry;
    double last_time;
    int objective_shown;
} g_world = {.mode_id = -1, .objective_shown = -2};

static unsigned int mode_world_texture(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "textures/%s.png", name);
    char *full = concat(assets_path(), path);
    if (!full) return 0;
    const unsigned int texture = load_texture(full); // cached by path
    free(full);
    return texture;
}

static Geometry *mode_world_model(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "models/%s.obj", name);
    char *full = concat(assets_path(), path);
    if (!full) return NULL;
    Geometry *geometry = load_obj_model(full, 0);
    free(full);
    return geometry;
}

// A unit cube's four side faces, base at y = 0 (prefab.c's cube layout).
static Geometry *mode_world_zone_geometry(void) {
    if (g_world.zone_geometry) return g_world.zone_geometry;

    static const float vertices[] = {
        -0.5f, 0.0f, 0.5f, 0.0f, 0.0f,   0.5f, 1.0f, 0.5f, 1.0f, 1.0f,   -0.5f, 1.0f, 0.5f, 0.0f, 1.0f,   0.5f, 0.0f, 0.5f, 1.0f, 0.0f,
        0.5f, 0.0f, -0.5f, 0.0f, 0.0f,   -0.5f, 1.0f, -0.5f, 1.0f, 1.0f, 0.5f, 1.0f, -0.5f, 0.0f, 1.0f,   -0.5f, 0.0f, -0.5f, 1.0f, 0.0f,
        -0.5f, 0.0f, -0.5f, 0.0f, 0.0f,  -0.5f, 1.0f, 0.5f, 1.0f, 1.0f,  -0.5f, 1.0f, -0.5f, 0.0f, 1.0f,  -0.5f, 0.0f, 0.5f, 1.0f, 0.0f,
        0.5f, 0.0f, 0.5f, 0.0f, 0.0f,    0.5f, 1.0f, -0.5f, 1.0f, 1.0f,  0.5f, 1.0f, 0.5f, 0.0f, 1.0f,    0.5f, 0.0f, -0.5f, 1.0f, 0.0f,
    };
    static const unsigned int indices[] = {
        0, 1, 2, 0, 3, 1,   4, 5, 6, 4, 7, 5,   8, 9, 10, 8, 11, 9,   12, 13, 14, 12, 15, 13,
    };

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &g_world.zone_vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_world.zone_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    geometry->index_count = (int) (sizeof(indices) / sizeof(indices[0]));
    g_world.zone_geometry = geometry;
    return geometry;
}

static Mesh *mode_world_mesh(Geometry *geometry, const unsigned int texture) {
    if (!geometry) return NULL;
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    material->texture = texture;
    material->fog_enabled = 0;    // noFog
    material->ambient_enabled = 0;
    return mesh_init(geometry, (Material *) material);
}

static void mode_world_drop_mesh(Mesh **mesh) {
    if (!*mesh) return;
    if (g_world.scene) scene_remove_mesh(g_world.scene, *mesh);
    free((*mesh)->material);
    mesh_fini(*mesh); // geometry is shared (kept)
    *mesh = NULL;
}

static void mode_world_clear(void) {
    for (int f = 0; f < MODE_MAX_FLAGS; f++) {
        mode_world_drop_mesh(&g_world.flags[f].crystal);
        mode_world_drop_mesh(&g_world.flags[f].zone);
    }
    for (int t = 0; t < MODE_MAX_SPAWNABLES; t++) mode_world_drop_mesh(&g_world.tags[t].mesh);
    g_world.flag_count = 0;
    g_world.map = NULL;
    g_world.mode_id = -1;
    g_world.objective_shown = -2;
}

static void mode_world_build(Client *client) {
    mode_world_clear();
    g_world.scene = client->scene;
    g_world.map = client->game.map;
    g_world.mode_id = client->game.mode ? client->game.mode->config.id : -1;

    const GameMode *mode = client->game.mode;
    if (!mode || !mode->config.flags || !client->scene) return;

    if (!g_world.crystal_geometry) g_world.crystal_geometry = mode_world_model("crystal_0");

    for (int f = 0; f < mode->flag_count && f < MODE_MAX_FLAGS; f++) {
        ModeWorldFlag *flag = &g_world.flags[f];
        const vec3 origin = mode->flags[f].origin;
        const float ground = origin.y - MODE_FLAG_SCALE * 0.5f - MODE_FLAG_OFF;

        flag->crystal = mode_world_mesh(g_world.crystal_geometry, 0);
        flag->zone = mode_world_mesh(mode_world_zone_geometry(), 0);
        flag->team_shown = -1;
        flag->bob = 0.0f;
        flag->spin = 0.0f;

        if (flag->zone) {
            BasicMaterial *material = (BasicMaterial *) flag->zone->material;
            material->unlit = 1;               // MeshBasicMaterial
            material->base.transparent = 1;
            material->no_depth_write = 1;
            material->double_sided = 1;
            flag->zone->no_shadow_receive = 1;
            flag->zone->transform.position = (vec3) {origin.x, ground, origin.z};
            flag->zone->transform.scale = (vec3) {MODE_FLAG_ZONE_SIZE, MODE_FLAG_ZONE_HEIGHT, MODE_FLAG_ZONE_SIZE};
            scene_add_mesh(client->scene, flag->zone);
        }
        if (flag->crystal) {
            flag->crystal->no_shadow_receive = 1;
            flag->crystal->transform.scale = (vec3) {MODE_FLAG_SCALE, MODE_FLAG_SCALE, MODE_FLAG_SCALE};
            flag->crystal->transform.position = origin;
            scene_add_mesh(client->scene, flag->crystal);
        }
    }

    g_world.flag_count = mode->flag_count;
}

// updateFlagCol: your team's crystal / zone are blue, the enemy's red.
static void mode_world_flag_colors(ModeWorldFlag *flag, const int flag_team, const int local_team) {
    if (flag->team_shown == local_team) return;
    flag->team_shown = local_team;
    const int own = local_team && flag_team == local_team;

    if (flag->crystal) {
        BasicMaterial *material = (BasicMaterial *) flag->crystal->material;
        material->texture = mode_world_texture(own ? "crystal_1" : "crystal_0");
        material->emissive = hex_to_vec(own ? MODE_WORLD_EMISSIVE_ALLY : MODE_WORLD_EMISSIVE_ENEMY);
    }
    if (flag->zone) {
        BasicMaterial *material = (BasicMaterial *) flag->zone->material;
        material->texture = mode_world_texture(own ? "zone_b" : "zone_r");
    }
}

static void mode_world_tick_flags(Client *client, const float delta_ms) {
    const int local_team = client->me ? client->me->team : 0;

    for (int f = 0; f < g_world.flag_count; f++) {
        ModeWorldFlag *flag = &g_world.flags[f];
        vec3 position;
        int team = 0, carrier_uid = -1;
        if (!hud_modes_flag(f, &position, &team, &carrier_uid)) {
            const ModeFlag *base = &client->game.mode->flags[f];
            position = base->origin;
            team = base->team;
        }

        mode_world_flag_colors(flag, team, local_team);
        if (!flag->crystal) continue;

        flag->crystal->visible = 1;
        if (carrier_uid >= 0) {
            const Player *carrier = client_find_player(client, carrier_uid);
            if (carrier) {
                position = (vec3) {carrier->position.x, carrier->position.y + MODE_FLAG_HOFF, carrier->position.z};
                flag->crystal->visible = carrier != client->me && carrier->active; // follows the carrier's model visibility
            }
        }

        flag->bob += delta_ms * 0.003f;
        flag->spin += delta_ms * 0.002f;
        flag->crystal->transform.position = (vec3) {position.x, position.y + cosf(flag->bob), position.z};
        flag->crystal->transform.rotation = (vec3) {0.0f, flag->spin, 0.0f};
    }
}

static void mode_world_tick_tags(Client *client, const float delta_ms) {
    if (!client->scene) return;
    const int local_team = client->me ? client->me->team : 0;

    for (int t = 0; t < MODE_MAX_SPAWNABLES; t++) {
        ModeWorldTag *tag = &g_world.tags[t];
        vec3 position;
        int team = 0;

        if (!hud_modes_spawnable(t, &position, &team)) {
            mode_world_drop_mesh(&tag->mesh);
            continue;
        }

        if (!tag->mesh) {
            if (!g_world.tag_geometry) g_world.tag_geometry = mode_world_model("krunkie_0");
            tag->mesh = mode_world_mesh(g_world.tag_geometry, mode_world_texture("krunkie_0"));
            if (!tag->mesh) continue;
            tag->mesh->no_shadow_receive = 1;
            tag->mesh->transform.scale = (vec3) {MODE_TAG_SCALE, MODE_TAG_SCALE, MODE_TAG_SCALE};
            tag->bob = 0.0f;
            tag->spin = 0.0f;
            scene_add_mesh(client->scene, tag->mesh);
        }

        // spawnables[0].emissive(localPlayer, team)
        ((BasicMaterial *) tag->mesh->material)->emissive = hex_to_vec(local_team && team == local_team ? MODE_WORLD_EMISSIVE_ENEMY : MODE_WORLD_EMISSIVE_ALLY);
        tag->bob += delta_ms * 0.003f;
        tag->spin += delta_ms * 0.002f;
        tag->mesh->transform.position = (vec3) {position.x, position.y + cosf(tag->bob), position.z};
        tag->mesh->transform.rotation = (vec3) {0.0f, tag->spin, 0.0f};
    }
}

// setObjective: the active hardpoint's zone is the only one shown.
static void mode_world_tick_objectives(Client *client) {
    const GameMode *mode = client->game.mode;
    const Map *map = client->game.map;
    const int active = mode && mode->config.objective ? mode->active_objective : -1;
    if (active == g_world.objective_shown || !map) return;
    g_world.objective_shown = active;

    int index = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        Object *object = map->objects[i];
        if (!object->objective) continue;
        if (object->mesh) ((Mesh *) object->mesh)->visible = index == active;
        index++;
    }
}

void mode_world_tick(Client *client) {
    if (!client || !client->game.ready || !client->game.map) {
        if (g_world.map) mode_world_clear();
        return;
    }

    const int mode_id = client->game.mode ? client->game.mode->config.id : -1;
    if (g_world.map != client->game.map || g_world.mode_id != mode_id || g_world.scene != client->scene) mode_world_build(client);

    const double now = glfwGetTime();
    float delta_ms = g_world.last_time > 0.0 ? (float) ((now - g_world.last_time) * 1000.0) : 0.0f;
    if (delta_ms < 0.0f || delta_ms > 250.0f) delta_ms = 0.0f;
    g_world.last_time = now;

    mode_world_tick_objectives(client);
    if (g_world.flag_count) mode_world_tick_flags(client, delta_ms);
    mode_world_tick_tags(client, delta_ms);
}

void mode_world_unload(Client *client) {
    (void) client;
    mode_world_clear();
}
