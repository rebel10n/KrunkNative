#pragma once
// Krunker v10.0.3 environment reflections for GLB materials (client/src/env_map.c, doc/v10/render.md
// "GLB environment reflections"). Opt-in per material: a GLB material whose glTF extras carry "env"
// (GLTFLoader puts them in material.userData) gets
//   envMap = render.getEnvMap(), envMapIntensity = limitMM(parseFloat(userData.env), 0, 4)
// when it has no envMap yet (deob-10.0.3.js:233665-233669; gltf.c reads the extras). getEnvMap
// (deob:231119-231157) paints a 256 x 128 equirectangular canvas: a vertical gradient from the sky
// dome colours (skyDomeCols, deob:231159) and the scene fog colour, sRGB, EquirectangularReflection.
// MeshStandardMaterial turns it into a PMREM (three r18x PMREMGenerator: a 64 px cube, 9 GGX-filtered
// roughness levels, deob:70739-71011) and lights the material with it (EnvironmentNode +
// PhysicalLightingModel.indirectSpecular, deob:71245-71275 / 70268-70318; basic.frag krunker_scene_env).
//
// The gradient depends on the elevation only, so the PMREM is too: env_map.c filters it on the CPU
// into a 9 x 128 table (one column per PMREM level, one row per elevation) and hands it, with
// three's 16 x 16 DFG lookup table (deob:69405-69420), to basic.c (basic_material_set_scene_env).
//
// Documented differences from v10.0.3:
//   - v10 builds the map when a material asks for it, so a model keeps the colours of the moment it
//     loaded. KrunkNative builds it when a map loads (env_map_update from map_environment.c) and every
//     opted-in material reflects the current one: a model loaded inside an ENVIRONMENT_ZONE reflects
//     the map's colours, not the zone's.
//   - The PMREM cube's own texel blur (16 px faces from level 3 on) is not modelled; the GGX cascade is.
// AI.spawn's "reflections" option (refl: metallic materials of an AI's model, deob:211721, 205080) sets the same
// scene_env_intensity on the AI's model (client/src/ai_render.c air_apply_refl).
#include <common.h>

// skyDomeInit (deob:231159): skyDomeCols = [col0, col1, col2].filter(Boolean) of the data the dome
// was built from. `colors` holds the colours left after the filter (count 0..3). Like v10's, the
// list stays until the next dome is built (clearSkyDome never resets it), across maps and zones.
void env_map_note_sky_dome(const char *const *colors, int count);

// getEnvMap for the scene now: the fog colour (NULL: no scene fog, "#8899aa") and the noted dome
// colours. Rebuilds the filtered table when they changed and hands it to basic.c.
void env_map_update(const vec4 *fog_color);

// Frees the GL textures (client shutdown).
void env_map_fini(void);
