#pragma once
// Map render work that runs once every object mesh exists (map_post.c): AO planes
// (ambient.c), noise-plane bump geometry, the v7 light objects (basic.frag light list), GLTF
// user assets, spinning meshes and the canvas render distance (map_post_frame).
#include <client.h>
#include <canvas2d.h>

void map_post_load(Scene *scene, const Map *map);
void map_post_unload(Scene *scene);
// Every frame before the world render: spin ("spn"), sign render distance, GLTF asset clips,
// player builds (their meshes), the battle-royale zone.
void map_post_frame(Scene *scene, const Game *game, const Camera *camera, float delta_ms);
Mesh *prefab_build_dynamic(const Object *object); // prefab.c: the mesh of a player build (caller owns it)
Mesh *prefab_pickup_mesh(int weapon_id, int no_texture, float *scale); // prefab.c: a weapon pickup's model (caller owns it)
Mesh *prefab_weapon_mesh(const Weapon *weapon, int no_texture);   // prefab.c: a weapon's model, unposed (caller owns it)
Mesh *prefab_asset_mesh(const char *model, const char *texture);   // prefab.c: models/<model>.obj + textures/<texture>.png (caller owns it)
unsigned int prefab_asset_texture(const char *src);               // prefab.c: textures/<src>.png (cached; the missing-texture checker)

// prefab.c canvas helpers (utils.js createCanvasText / ctxText on canvas2d.h), for the
// leaderboard canvases: begin a width x height canvas, draw, end -> a cached RGBA texture.
Canvas2D *prefab_canvas_begin(int width, int height);
unsigned int prefab_canvas_end(Canvas2D *canvas, const char *key);
void prefab_canvas_text(Canvas2D *canvas, const char *text, const char *color, double x, double y,
                        const char *align, double font_px, const char *outline, double outline_width);
Mesh *prefab_canvas_mesh(Object *object, const cJSON *raw_obj, unsigned int texture); // the leaderboard plane / sprite

// MapManager.updatePendingLeader(index, rows): the server's rows (name, value, name, value ...)
// for the index-th LEADERBOARD object (file order, game.addPendingLeaderboards / WP-C/D
// packets). Draws utils.js createLeaderText and shows the board (nothing before the first
// non-empty rows, as in v7). Returns 1 when drawn.
int map_post_leaderboard_rows(Scene *scene, const Map *map, int index, const char *const *cells, int cell_count);
