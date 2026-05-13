#include <client.h>
#include <math.h>

typedef struct {
    int color;
    float height;
    float scale;
} ColorCubeSegment;

ColorCube *generate_color_cube(const float width, const float height, const float length, const ColorCubeSegment *segments, const size_t segment_count) {
    ColorCube *color_cube = calloc(1, sizeof(ColorCube));
    if (!color_cube) return NULL;

    color_cube->meshes = calloc(segment_count, sizeof(Mesh*));
    color_cube->mesh_count = segment_count;

    if (!color_cube->meshes) {
        free(color_cube);
        return NULL;
    }

    color_cube->transform.scale.x = width;
    color_cube->transform.scale.y = height;
    color_cube->transform.scale.z = length;

    float offset = 0.5f;

    for (size_t i = 0; i < segment_count; i++) {
        const ColorCubeSegment *segment = &segments[i];

        Geometry *geo = create_cube_geo();
        BasicMaterial *material = basic_material_init();

        Mesh *mesh = mesh_init(geo, (Material *) material);

        material->color = hex_to_vec(segments[i].color);

        mesh->scale.x = segment->scale;
        mesh->scale.z = segment->scale;
        mesh->scale.y = segment->height;

        mesh->position.y = offset - segment->height;
        mesh->parent = &color_cube->transform;

        offset -= segment->height;
        color_cube->meshes[i] = mesh;
    }

    return color_cube;
}

void generate_arm() {

}

void generate_arms(const Weapon *weapon, const int third_person) {

}

void generate_body() {

}

void generate_leg() {

}

void player_generate_meshes(Player *player, const int render_you) {
    int colors[6];
    memcpy(colors, player->game->classes[player->class_index].colors, sizeof(colors));

    // TODO: dye colors
    // TODO: mode specific skins

    const int third_person = !render_you || player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;

    if (player->game->map->config.model == MODEL_TYPE_SPRITE) {

    } else if (0 /* TODO: prop hunt */) {

    } else {
        if (third_person) {
            // TODO: body
        }

        int no_legs = 0;

        // TODO: back skin
        // TODO: waist skin
        // TODO: hat skin
        // TODO: face skin

        if (third_person && !no_legs) {
            for (int i = 0; i < 4; i++) {
                // TODO: legs
            }
        }

        for (int i = 0; i < player->loadout_size; i++) {
            const Weapon *weapon = player->game->weapons[player->loadout[i]];

            // ARM GENERATION
            if (weapon->src || !render_you) {

            } else {
                for (int j = 0; j < 2; j++) {
                    // TODO

                    if (!weapon->akimbo) break;
                }
            }

            // MELEE MESH
            if (weapon->melee) {
                // TODO
            }

            // WEAPON MESH
            if (weapon->src) {
                for (int j = 0; j < 2; j++) {
                    // TODO

                    if (!weapon->akimbo) break;
                }
            }
        }
    }
}