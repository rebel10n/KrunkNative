#pragma once
// The developer console's `ent_bbox` geometry (Krunker v7.1.4 mod/devConsole.ts:1392, the entity bounding-box
// overlay; client/src/ent_bbox_geom.c): the movement boxes of the players and the map objects as GL_LINES vertex
// lists, one layer per colour. No GL here: client/src/ent_bbox.c uploads the layers every frame (players and
// dynamic objects move) and client/tests/ent_bbox_test.c checks them.
//
// The boxes are the ones the movement code tests (player_object_movement_bounds / player_movement_bounds,
// shared/src/player.c player_do_map_collisions), each as the 12 edges of its axis-aligned box:
//   self:    the local player's box (third person only: in first person the camera is inside it)
//   players: every other live player and bot
//   solid:   the map objects that stop a player (bot_object_blocks_player, not noPCol)
//   volume:  the walk-through ones the movement loop still tests (zones, jump pads, triggers, ladders...)
// Objects without a collider (noCol "l" / "col", COLLISION_TYPE_NONE), inactive or destroyed ones and complex
// collision (cpx: they collide through their triangles) are not drawn.
#include <shared.h>
#include <stddef.h>

enum { ENT_BBOX_SELF, ENT_BBOX_PLAYERS, ENT_BBOX_SOLID, ENT_BBOX_VOLUME, ENT_BBOX_LAYERS };

extern const int ent_bbox_layer_hex[ENT_BBOX_LAYERS]; // 0xrrggbb per layer

#define ENT_BBOX_BOX_VERTS 24 // 12 edges, GL_LINES pairs

typedef struct {
    float *v; // x, y, z per vertex
    size_t n, cap;
} EntBboxVerts;

typedef struct {
    EntBboxVerts layers[ENT_BBOX_LAYERS];
    int players;  // boxes in self + players
    int objects;  // boxes in solid + volume
} EntBboxGeom;

// Builds every layer from the game's live players and current map (layers cleared first, buffers kept).
// self: the local player (NULL: none); draw_self 0 leaves its box out. Returns the vertex count over all layers.
size_t ent_bbox_geom_build(const Game*, const Player *self, int draw_self, EntBboxGeom*);
void ent_bbox_geom_free(EntBboxGeom*);
