#pragma once
// Krunker v3.1.1 directional sun shadow (render.js initScene/updateLightMap, three.js r121
// BasicShadowMap): one static depth map of the map's shadow casters, seen from the
// DirectionalLight at lightD along g_render_lighting.light_direction, sampled by
// basic.frag for receivers. All tunables live at the top of shadow.c.
//
// GL resources (FBO, depth texture, depth program) belong to this module.
// Texture unit SHADOW_TEXTURE_UNIT is reserved for the shadow map in basic.frag.
#include <client.h>

#define SHADOW_TEXTURE_UNIT 1

// Reads shadowR/shadowD/lightD and the map extents, flags each map object's mesh as
// caster/receiver from its prefab and schedules a shadow map render.
// Call after client_apply_map_lighting() set the light direction.
void shadow_load_map(const Map *map);
// Drops the caster list (the meshes are freed with the map).
void shadow_unload_map(void);
// Renders the shadow map if it is out of date. Restores framebuffer, viewport,
// program, VAO, cull/depth/blend state.
void shadow_update(void);
// Brackets the world scene draw: only meshes drawn in between receive shadows
// (the first-person scene and the menu class preview do not).
void shadow_begin_world(void);
void shadow_end_world(void);
// Per-draw receiver uniforms for `mesh` (no-op for programs without them).
void shadow_apply_mesh_uniforms(const Mesh *mesh);
void shadow_fini(void);
