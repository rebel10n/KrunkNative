# Mod post-processing shaders (v10.0.7)

Krunker v10 mods can carry full-screen post passes: a mod zip's `shaders/<name>.json` entries are
shader graphs that the game compiles into three.js TSL and runs over the rendered frame, with their own
rows in Settings. Box Office runs the same files as GLSL passes on its forward renderer.

## What v10.0.7 does (cite `deob-10.0.7.js`)

| Step | Lines | Behaviour |
| --- | --- | --- |
| zip entry | :225662-225676, :225918-225921 | every entry whose first path segment is `shaders` is read as text, `JSON.parse`d and handed to `insertShaderPass`; parse errors are swallowed |
| insertShaderPass | :205848-205931 | accepts `{graph, shaderArgs}` or a bare graph whose `kind` is `"post"`; validates (`kind: "post"`), dry-compiles with a dummy context, and on any failure adds nothing; otherwise adds the pass to `postfx` and a row list to `shaderInputs` |
| rows | :205904-205919 | row 0 `{name: "Shader Mod # N", toggle, val: true}`; per graph param: type `bool` a switch, `float` / `number` / unset a slider `{min: parseFloat(min) \|\| 0, max: parseFloat(max) \|\| 1, step: 0.01, val: parseFloat(default) \|\| 0}`, other types no row |
| applyShaderInput | :205932-205941 | row 0 sets `pass.enabled`; other rows write `uniforms[name].value` (a switch as 1 / 0); every row is applied once at insert, so the rows' values win over `shaderArgs.params` |
| resetShaderPasses | :205943-205949, :218823 | Reset Mods clears the passes and rows; passes otherwise accumulate across loaded mods |
| postfx | :195864-196010 | two RGBA ping-pong targets at the drawing-buffer size; each pass samples the previous result (`sceneColor(uv)`), gets `time`, `resolution`, `mouse`, `frame`; a build that throws disables the pass and copies its input; the last pass applies the renderer's tone mapping |
| Settings | :218016, :218092-218120 | on the advanced view's Render tab (categories include `shaders`) or when a search matches `settings.shaders.header`: a header, then per row a switch (`setShaderInput(i, j, this.checked)`) or a number box `shader_input_i_j` (`delayExecute2(i, j, this)`) + range `shader_i_j` (`setShaderInput(i, j, this.value)`) |
| setShaderInput | :216660-216673 | `"true"` / `"false"` become booleans, a row with a min or max clamps `Number(v)`, a switch takes `~~!!v`; both elements show the value; `delayExecute2` applies the box's value 600 ms after the last key |

The graph format (:93193-95540): `{v: 1, kind, nodes: [{id, type, inputs, params, x, y}], output}`, 172
node types (`client/src/mod_shader_nodes.inc`, generated from the bundle by `tools/modshader/gen_nodes.cjs`),
inputs wired as `{node, port}` or literal `{value}`. The validator (:94463-94792) caps 256 nodes, 32 param
nodes and 64 KiB, checks ids, ports, params and kinds, and refuses cycles. The compiler (:94824-95560)
converts each input to the width its node asks for (broadcast a float, truncate, or pad `vec2 -> vec3(v, 0)`,
`-> vec4(v, 0, 1)`, `vec3 -> vec4(v, 1)`); arithmetic takes the widest operand.

## Box Office

- `client/src/mod_shader_graph.c` holds the validator (same rules and messages) and a translator that emits
  one GLSL fragment shader per graph. Each node becomes statements over temporaries, with v10's width
  rules. The TSL helpers the bundle imports from three.js (MaterialX noise, Worley, cell and fractal noise,
  hsv, blend modes, hue / saturation / vibrance, oscillators, UV helpers) are written out in GLSL there.
  The noise library is only emitted when a node uses it. Param nodes become uniforms `u_p<index>`.
- `client/src/mod_shader.c` holds the pass list, `shaderInputs` and the GL side: the shared `k_header` version
  line (`300 es` / `410 core` on macOS desktop GL / `450 core`), a gl_VertexID full-screen triangle, and
  RGBA8 ping-pong targets at the scene target's size. A pass whose GLSL fails to compile or link is skipped
  (v10's copy fallback).
- `render_target.c` routes the scene through its target while a pass is enabled (`mod_shader_active()` in
  `render_target_begin`'s `need`) and runs the passes in `render_target_end` after TAA / bloom, before the
  upscale / sharpen and the saturation / hue composite. **With no enabled pass nothing changes**: rtMode-0
  frames stay identical (`tools/replay/rt0_identity.sh`, Sandstorm and Kanji, with and without the page).
- `mod_pack.c` keeps the `shaders/` entries in zip order and hands them to `hooks.shader` once the pack is
  active; a pack with only shaders still loads. On the web the entries come out of the zip (MEMFS), so no new
  asset files are fetched; the GLSL library is compiled into the client.
- Settings: `ui_settings_shader_html()` builds the block, which `ui_windows.c` puts at the top of the
  advanced Render tab and the search shows when the header matches. `setShaderInput` / `delayExecute2` are
  handled in `ui_settings_handle`.

### Documented differences

- The passes run at the scene target's size (Settings resolution / aspect), not the drawing buffer's.
- Krunker's last pass tone maps; Box Office tone maps in `basic.frag`, so passes see tone-mapped colours.
- The active pack owns its passes: replacing the pack (a map's `modURL`, another mod) replaces them, where
  v10 keeps appending until Reset Mods.
- `viewportSharedTexture` samples the pass input; `viewportDepthTexture` / `viewportLinearDepth` read 1
  (a pass target's depth is never written). Mesh inputs describe the full-screen quad; texture nodes read
  white (post graphs have no texture slots in v10 either). `screenDPR` is 1.
- `mouse` and `frame` are not exposed: no v10.0.7 node reads them.

## Tests

- `mod_shader_test` (ctest): the validator's refusals, the sample graphs, and every node output translating.
- `mod_shader_glsl` (ctest, needs glslangValidator): every node output's GLSL is valid ES 3.00 and 4.50 core.
- `tools/modshader/sample_e2e.sh`: grey and invert mods (`client/tests/data/mod_shader/`) on a Sandstorm copy
  whose `modURL` points at them, headless from a pinned camera. Checks: grey frame R=G=B with the plain frame's
  Rec. 709 luminance, invert frame = 255 minus plain.
- uidiff `settings-advanced-render-shaders` (KN reference): the rows with two passes loaded (`state.modShaders`).
