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
//   video             v10.0.3: videos/video_<n>.mp4 in the map (client/include/skin_video.h), unless the
//                     noVideoSkins setting (Disable Video Skins) strips video, projection and texSrc
// Animations only run while the textureAnim setting is on (v7 RENDER.textureAnim), like
// client_tick_textures for map objects (a video plays whatever it says, as v10's VideoTexture). Not
// ported: the per-material three.js texture cache sharing (each mesh animates on its own, in step
// with the others of the same item; a video is one texture per id and filter).
#include <client.h>

struct ViewerSkin;
struct GltfInstance;

// getMat for a store item on `mesh` (a BasicMaterial mesh): the static options (colour, opacity,
// side, alphaTest, wireframe, tileS, frames repeat, glow, glowInt, env, projection, video) and the
// animations above. texture_key is texSrc (NULL: none), model_key the model src; the emissive map
// is (glow_text && texSrc || src) + "_e" (sameGlow: the texture). emissive = (glow || sameGlow) &&
// !noEmis for most items. shaderNum replaces mesh->material with a ShaderToy material.
void skin_fx_apply(Mesh *mesh, const struct ViewerSkin *skin, const char *texture_key, const char *model_key, int glow_text, int emissive);

// f1117 for a hasAnim (GLTF) item: colour / opacity, pulsT, rgb and env on every material of the
// model, and rAnim on its root.
void skin_fx_apply_gltf(struct GltfInstance *model, const struct ViewerSkin *skin, const char *model_key);

// Body-part materials of a dye (playerMesh.js genHead / genBody / genLeg / genArms getMat): its tex,
// colour, opacity, glow and rgb (registered like an item; no frames / pulse), its video and projection.
void skin_fx_apply_dye(Mesh *mesh, const struct ViewerSkin *dye);

// A video in the map of `mesh` (a BasicMaterial) without a store item: the upgraded weapons' altSkin
// (skin_video_alt_skin). projection: screen-space UVs, DoubleSide, LinearFilter. 0 when noVideoSkins
// is on or the video cannot play: the mesh keeps its texture (v10's strip, deob-10.0.3.js:231704-231710).
int skin_fx_apply_video(Mesh *mesh, int video, int projection, int linear);

// The per-copy paint of a worn skin on its mesh (a BasicMaterial): v10.0.3's paint node (basic.frag paint_apply): the
// colour paint overlays the albedo, a shader paint (paintShader 0-5) draws its graph. `skin` gives the paintMask
// (its texture scales the intensity); NULL = none. A paint of colour -1 and shader -1 leaves the mesh as it is.
void skin_fx_apply_paint(Mesh *mesh, WornSkinPaint paint, const struct ViewerSkin *skin);
// The same on every mesh of a hasAnim (GLTF) item: v10 paints the meshes paintTargets names, or all of them when it
// is true (deob-10.0.3.js:233833-233837). The GLTF meshes carry no names here, so every mesh is painted.
void skin_fx_apply_paint_gltf(struct GltfInstance *model, WornSkinPaint paint, const struct ViewerSkin *skin);

// Drop a mesh's / model's registrations: call before freeing it (mesh_fini / gltf_instance_fini).
void skin_fx_forget(const Mesh *mesh);
void skin_fx_forget_gltf(const struct GltfInstance *model);

// Advance every registered animation by dt_ms when texture_anim (the textureAnim setting) is on.
// basic_lighting: the lighting setting is 0 (every material a MeshBasicMaterial: rgb colours the
// colour, not the emissive). The viewer and the game share it.
void skin_fx_tick(float dt_ms, int texture_anim, int basic_lighting);

// UTILS.hexFromHue(hue): the 0xRRGGBB of a hue in degrees (0-360)
int skin_fx_hex_from_hue(int hue);

// pulseShotShaders (deob-10.0.4.js:174911-174919): a shot starts the pulse of the shotPulse custom shaders (14, "Shock")
// on the weapon's model / mesh (either may be NULL). Shader 14's parts show only while their pulse plays.
void skin_fx_shot_pulse(const struct GltfInstance *model, const Mesh *mesh);

// The seeded custom shaders (15, "Sketch") vary by the owned copy (deob-10.0.4.js:24344-24353, getMat :203448 / :203463).
// skin_fx_copy_seed: a copy's seed (iîîíïiï): its meta seed (floor(seed) mod 1e6) when it has one, else its instance id
// mixed (lowbias32 mod 1e6), 0 = no copy (shop previews). skin_fx_seed_mix: that seed into [0, 1) (iîíîìíi, 0 stays 0).
// skin_fx_set_seed: the seed (skin_fx_copy_seed's) of the seeded custom materials on the model / mesh (either may be NULL).
int skin_fx_copy_seed(int copy_id, int has_meta_seed, double meta_seed);
float skin_fx_seed_mix(int seed);
void skin_fx_set_seed(struct GltfInstance *model, Mesh *mesh, int seed);
