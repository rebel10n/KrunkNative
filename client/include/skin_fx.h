#pragma once
// Krunker v7.1.4 store-item material effects (src/libs/render.js getMat / loadMesh / f1115 /
// f1117 and the textureAnim part of RENDER.render): what a weapon skin, melee, cosmetic, pet,
// charm or dye does to the material of its meshes, and the per-frame animations:
//   frames / frameT   sprite-sheet frames (up to 10 per row), the map offset steps every frameT ms
//   movT / movD       the map offset scrolls along x (movD 0) or y by movT per ms
//   rotT              the map rotation turns by rotT per ms
//   pulsT / minGlo    emissiveIntensity = (sin(pulsVal) / 2 + 0.5) * 1.5 + minGlo, pulsVal += dt * 0.003
//   rgb / rgbS / rgbT the emissive (rgbT 1 or a MeshBasicMaterial: the colour) cycles through the hues
//   rAnim / rAnimA / rAnimT / rAnimM  the mesh spins about its axis by rAnim per ms, or (rAnimT)
//                     bobs: position[axis] = start + sin(bob) * (rAnimM || 0.55)
//   glow / sameGlow / emiss / noEmis / glowInt  emissive colour, map "<tex>_e" and intensity
//   env / sameEnv     environment map "<tex>_env" (EquirectangularReflection), basic.frag
//   projection        screen-space texture coordinates (basic.frag)
//   shaderNum         a ShaderToyMaterial (v7 src/133.js C240, assets/shaders/shadertoy/<n>.glsl)
// Animations only run while the textureAnim setting is on (v7 RENDER.textureAnim), like
// client_tick_textures for map objects. Not ported: `video` textures (no video decoder; the item
// keeps its model texture) and the per-material three.js texture cache sharing (each mesh
// animates on its own, in step with the others of the same item).
#include <client.h>

struct ViewerSkin;
struct GltfInstance;

// getMat for a store item on `mesh` (a BasicMaterial mesh): the static options (colour, opacity,
// side, alphaTest, wireframe, tileS, frames repeat, glow, glowInt, env, projection) and the
// animations above. texture_key is texSrc (NULL: none), model_key the model src; the emissive map
// is (glow_text && texSrc || src) + "_e" (sameGlow: the texture). emissive = (glow || sameGlow) &&
// !noEmis for most items. shaderNum replaces mesh->material with a ShaderToy material.
void skin_fx_apply(Mesh *mesh, const struct ViewerSkin *skin, const char *texture_key, const char *model_key, int glow_text, int emissive);

// f1117 for a hasAnim (GLTF) item: colour / opacity, pulsT, rgb and env on every material of the
// model, and rAnim on its root.
void skin_fx_apply_gltf(struct GltfInstance *model, const struct ViewerSkin *skin, const char *model_key);

// Body-part materials of a dye (playerMesh.js genHead / genBody / genLeg / genArms getMat): its tex,
// colour, opacity, glow and rgb (registered like an item; no frames / pulse).
void skin_fx_apply_dye(Mesh *mesh, const struct ViewerSkin *dye);

// Drop a mesh's / model's registrations: call before freeing it (mesh_fini / gltf_instance_fini).
void skin_fx_forget(const Mesh *mesh);
void skin_fx_forget_gltf(const struct GltfInstance *model);

// Advance every registered animation by dt_ms when texture_anim (the textureAnim setting) is on.
// basic_lighting: the lighting setting is 0 (every material a MeshBasicMaterial: rgb colours the
// colour, not the emissive). The viewer and the game share it.
void skin_fx_tick(float dt_ms, int texture_anim, int basic_lighting);

// UTILS.hexFromHue(hue): the 0xRRGGBB of a hue in degrees (0-360)
int skin_fx_hex_from_hue(int hue);
