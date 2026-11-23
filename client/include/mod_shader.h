#pragma once
// Mod post-processing shaders (client/src/mod_shader.c, client/src/mod_shader_graph.c): Krunker v10.0.7's
// insertShaderPass. A mod zip's shaders/<name>.json entries (deob-10.0.7.js:225662-225676, :225918-225921) are
// shader graphs of kind "post", either bare or as {graph, shaderArgs: {params}}. Each valid one becomes a
// full-screen pass over the rendered scene (postfx, deob:195864-196010) with a "Shader Mod # N" switch and one
// slider per float parameter in Settings > Render (shaderInputs, deob:205848-205942, the rows :218099-218120,
// setShaderInput :216668-216673). Reset Mods clears them (resetShaderPasses, deob:205943-205949, :218823).
//
// A graph (deob:93193-95540) is a JSON node list {v: 1, kind, nodes: [{id, type, inputs, params, x, y}],
// output}: 172 node types (mod_shader_nodes.inc) wired by {node, port} references or {value} literals.
// Krunker compiles it to three.js TSL; here mod_shader_graph.c validates it with the same rules (iïiíííí,
// deob:94463-94792) and translates it to one GLSL ES 3.00 / GLSL 4.50 fragment shader.
//
// Documented differences from Krunker (see mod_shader.c / mod_shader_graph.c):
//   - The passes run on the scene target (render_target.c) after its TAA / bloom and before the upscale and
//     the in-game saturation / hue filter, at the target's size. Krunker's last pass applies the renderer's tone
//     mapping; Box Office tone maps in basic.frag, so a pass sees tone-mapped colours on maps with toneMapping.
//   - The active pack owns its passes: a pack replacing another (a map's modURL, a new mod) brings its own,
//     where Krunker appends every loaded pack's passes until Reset Mods.
#include <stddef.h>

struct cJSON;

#define MOD_SHADER_MAX_PASSES 16
#define MOD_SHADER_MAX_INPUTS 33 // the switch + up to maxParamNodes (32) parameters

// One row of a pass's shaderInputs: index 0 is the pass switch ("Shader Mod # N", toggle), the rest its float
// (slider) and bool (switch) parameters in node order.
typedef struct {
    char name[48];
    int toggle;
    float min, max, step;
    float val;
} ModShaderInput;

// insertShaderPass(JSON.parse(text)): 1 when a pass was added (an invalid / non-post graph adds nothing).
int mod_shader_insert_json(const char *text, size_t len);
// The same from parsed JSON (the scenario's state.modShaders).
int mod_shader_insert(const struct cJSON *json);
// resetShaderPasses: every pass and its inputs go.
void mod_shader_reset(void);

int mod_shader_count(void);                 // shaderInputs.length
int mod_shader_input_count(int pass);
const ModShaderInput *mod_shader_input(int pass, int index);
// setShaderInput(pass, index, value): "true" / "false" / a number, clamped to the row's range.
void mod_shader_set_input(int pass, int index, const char *value);

// 1 while an enabled pass exists (render_target_begin then routes the scene through its target).
int mod_shader_active(void);
// Runs the enabled passes over `scene_tex` (width x height): returns the texture holding the result, or
// `scene_tex` when no pass ran. Leaves the draw framebuffer to the caller (render_target_end rebinds it),
// blending / depth test off, unit 0 empty.
unsigned int mod_shader_render(unsigned int scene_tex, int width, int height);
// GL objects go (programs, targets); the pass list stays (the context is going away).
void mod_shader_fini(void);

// ---- the graph layer (no GL; mod_shader_graph.c, tested by client/tests/mod_shader_test.c)

#define MOD_SHADER_MAX_UNIFORMS 32

typedef struct {
    char name[33];  // the param node's params.name (the GLSL uniform is "u_p<index>")
    char node[33];  // the param node's id (a surface graph's two stages share one uniform per node)
    int width;      // 1..4 (a "color" param is a vec3)
    float value[4]; // its value: default, else shaderArgs.params[name], float ones clamped to min / max
} ModShaderUniform;

typedef struct {
    char name[33];
    char type[8]; // "float", "vec2", "vec3", "vec4", "color" (and "bool": a switch row)
    int has_default, has_min, has_max;
    double def, min, max;
} ModShaderParam;

typedef struct {
    char *fragment; // the fragment shader after its #version line (mod_shader.c prepends the platform's), malloc'd
    char *vertex;   // a surface graph's vertex shader, likewise (NULL for a post graph)
    // a surface graph's output settings (the output node's params, deob-10.0.7.js:94205-94228, as the material takes
    // them, :95840-95845): lit (MeshLambertNodeMaterial, else MeshBasicNodeMaterial), side double, additive blending,
    // depthWrite, transparent
    int lit, double_sided, additive, depth_write, transparent;
    ModShaderUniform uniforms[MOD_SHADER_MAX_UNIFORMS];
    int uniform_count;
    ModShaderParam params[MOD_SHADER_MAX_UNIFORMS]; // the graph's param nodes (iìiîííî, deob:95848-95870)
    int param_count;
} ModShaderProgram;

// The graph validator (deob:94463-94792) for `kind` ("surface", "paint", "post"; NULL any); 1 when valid, else 0 with the first error in `err`.
int mod_shader_graph_validate(const struct cJSON *graph, const char *kind, char *err, size_t err_size);
// Every error of the validator (v10's validate returns them all; the node canvas editor's footer lists them): the
// count (0 = valid), the first `max` stored in `out`. `node` is the node's id, "" for a graph-level error.
typedef struct {
    char node[64];
    char message[192];
} ModShaderGraphError;
int mod_shader_graph_errors(const struct cJSON *graph, const char *kind, ModShaderGraphError *out, int max);
// Validates and translates a post graph with the instance `params` (shaderArgs.params, may be NULL). 1 on success.
int mod_shader_graph_compile(const struct cJSON *graph, const struct cJSON *params, ModShaderProgram *out, char *err,
                             size_t err_size);
// A "surface" graph (a skin's customShaderInfo shaderDef, deob-10.0.7.js:95813-95847 and :208005-208016) as a vertex
// and a fragment shader for a mesh. The shaders' interface (filled by client/src/skin_fx.c):
//   attributes  location 0 position, 1 uv, 3 normal ((0, 0, 0) without normals: the face normal is derived)
//   uniforms    transform, camera_world_inverse, camera_projection (the renderer's); kn_map (unit 0) and kn_has_map
//               (the mesh's own texture: the graph's "map" slot and baseColor), kn_time, kn_delta, kn_seed,
//               kn_resolution, kn_dpr; for a lit graph kn_ambient (the ambient irradiance) and kn_sun / kn_sun_dir (the
//               sun's irradiance and its world direction); u_p<i> per param uniform
//   output      kn_frag_color (rgb, alpha = the alpha port)
int mod_shader_graph_compile_surface(const struct cJSON *graph, const struct cJSON *params, ModShaderProgram *out, char *err,
                                     size_t err_size);
// A "paint" graph (a paint's paintShaderGraph; the paint shader v10.0.7 builds from it, deob-10.0.7.js:95900-95925,
// whose ctx is {uv, time, baseColor, params}) as a GLSL chunk in out->fragment for a host fragment shader to include
// after its own declarations: the param uniforms u_p<i>, the graph library and
//   vec3 kn_paint_graph(vec2 uv, float t, vec3 base, vec3 plocal, vec3 pworld, vec3 cam, vec3 nworld)
// (the graph's colour; the paint material blends it over the base, :95732-95775). The host defines kn_map (its
// sampler2D), kn_has_map and KN_TEX(uv) as macros, and the mat4 `transform`. The chunk has no preprocessor lines and
// no // comments, so it can be one macro. The inputs the paint ctx has no source for take neutral values.
int mod_shader_graph_compile_paint(const struct cJSON *graph, const struct cJSON *params, ModShaderProgram *out, char *err,
                                   size_t err_size);
void mod_shader_program_free(ModShaderProgram *program);
