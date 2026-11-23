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

## Surface graphs (Shader Studio skins)

v10.0.7's Skin Manager saves a `kind: "surface"` graph as `itemConfig.customShader = {type: "graph", graph,
applyToAll, parts}`. testSkinSandbox and the preview's shaderStudioUpdate turn it into `customShaderInfo`
(`{"*" | part: {shaderDef: {type: "graph", graph}, shaderArgs: {params}}}`, :204476-204508), and getMat compiles
`shaderDef` before `shaders[shaderNum]` (:206493-206514 for an OBJ item, :208000-208016 per glTF part). The material
(iïììiíí, :95813-95847) is a Lambert material when the output's `lit` is set, else a basic one; `fog` is off; the
output's `color`, `alpha`, `emissive`, `positionOffset` and `normal` ports and `side` / `blend` / `depthWrite` /
`transparent` params apply.

- `mod_shader_graph_compile_surface()` emits a vertex and a fragment shader. The vertex stage compiles only
  `positionOffset` (derivatives read 0 there), and the fragment stage the other ports. Input nodes read the mesh
  (uv 0, local / world / view position and normal, the camera), `time` / `delta` / `seed`, and the mesh's own
  texture as the `map` slot (`kn_map`, unit 0). Both stages share one `u_p<i>` uniform per param node.
- `skin_fx.c` puts a `GraphMaterial` on the mesh: one linked program per graph text, the part's
  `shaderArgs.params`, the copy's seed when the graph has a `seed` node, and Lambert lighting from the scene's
  ambient and sun (`basic.frag`'s units). A graph that does not compile leaves the item's own material
  (v10's catch). It logs `[skin_fx] graph material: ...` when the material goes on.
- Differences: uv sets above 0 read uv 0; tangents are 0 and `vertexColor` is white; a texture slot other than
  `map` reads white, and `map` without a texture reads (0, 0, 0, 1); `textureBicubic` samples one mip level
  (`strength * maxMip`); the Lambert term has the ambient light and the sun only, without the map's point lights.

## The node canvas (sgEditor)

v10.0.7 edits a graph on a node canvas (sgEditor, deob-10.0.7.js:122896-124145). Shader Studio mounts it with
`kind: "surface"`, and its onChange is `itemConfig.customShader` (iÌiïíîi, :127145). A paint's Animated Shader
mounts it with `kind: "paint"`, and its onChange is `itemConfig.paintShaderGraph` (iÍííiïi, :126258). The
port is `client/src/ui/shader_graph_ui.c` (`shader_graph_ui.h`). Its DOM ids take the caller's prefix
(`knSmSg`, `knSmPg`).

- The node table and the templates are v10's own data. `tools/modshader/gen_editor.cjs <deob>` evaluates the deob's
  node table and editor region and writes `client/src/ui/shader_graph_defs.inc` (172 node defs, 9 templates).
- Layout (iïïiiìï, :122303): nodes are 210 px wide, with a 26 px header and 24 px rows (outputs, then params,
  then inputs). Wires are cubic beziers (iïïíîíî, :122453), drawn as one SVG image.
- The view: pan, wheel zoom (exp(-deltaY * 0.0015), 0.2 to 3) and Fit (80 px margin, 0.2 to 1.25). The pointer's
  screen px are divided by the canvas's on-screen scale, getBoundingClientRect().width / offsetWidth (iÌiíïïî,
  :123076), so a drag follows the pointer inside the window's `scale(--ui-scale)`.
- Editing works as in v10:
  - nodes are added from the palette (sorted by category, then title);
  - a wire dropped on empty canvas opens the palette filtered to that port's type;
  - pressing a connected input detaches its wire;
  - connections that would self-connect, mismatch types or make a cycle are refused with v10's messages.
- Keys: Delete removes, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y undo and redo, Ctrl+A selects all, F fits, Escape clears.
  History merges edits with the same key within 800 ms and keeps 200 entries. onChange is debounced by 100 ms.
- Also ported: Templates, Import / Export JSON, the footer's validation (`mod_shader_graph_errors`, each error
  clickable to its node), and the param and input value editors.
- HTML engine support the canvas needed: inline `onmousemove` (key = MouseEvent.buttons) and `onwheel` (key = deltaY
  px; `return false` keeps the page from scrolling). A text field's own `onkeydown` also sees Escape, so the
  palette's search closes the palette (:122695).

## Skin Manager's other Appearance sections

These sections are v10's, from `client/src/ui/v10_skin_manager.c` (:126048-126978):

- **Paintable** (`paintTargets` true, or a list of mesh names) and the paint mask (texture "mask", `texture_p.png`).
- **Paint**: a paint's Solid Colour (`paintColor`) or its Animated Shader (`paintShaderGraph`). It is previewed on
  `paintPreviewBase`, 7805 (Weapon) or 7798 (Cosmetic).
- **Wear**: four layers whose red channels are packed into R, G, B and A of a 1024 px `texture_w.png`, uploaded as
  texture "wear" (`customTextures.wear`).
- **Kounter**: `itemConfig.attach`, a type 1 stat counter. `viewer_skin_define` reads it, so Test and the preview
  draw it (skin_counter.c).
- **Charm**: `charmM{X,Y,Z}Off` / `charmMScl` (the weapon's spot to start from) and `noCharm`. player_mesh.c
  moves the mount to `skin || weapon` and scales it by `weapon.charmMScl * (skin.charmMScl || 1)` (:174210).
- **The preview's Wear slider and paint select** add `&wear=` and `&paint=` to the iframe's query. The query also
  carries `&<stat>=1284`, so the showcase's Kounter shows 1284.
- **Differences**:
  - The iframe takes no postMessage. attach and the charm keys stay in the showcase JSON (v10 posts attachEdit /
    charmEdit), and the wear level is the query's.
  - The preview's Snap to surface tool and its Move / Rotate / Scale gizmo are not ported. The Kounter and charm
    fields set the placement.
  - The paint preview draws a solid colour only; a paint's graph is saved but not drawn.

## Tests

- `mod_shader_test` (ctest): the validator's refusals, the sample graphs, and every node output translating.
- `mod_shader_glsl` (ctest, needs glslangValidator): every node output's GLSL is valid ES 3.00 and 4.50 core.
- `mod_shader_gl` (ctest, `tools/modshader/check_gl.py`, Mesa llvmpipe through surfaceless EGL): every post
  fragment compiles, and every surface node output's vertex / fragment pair compiles and links, as GLSL ES 3.00
  and 4.50 core. It skips (77) unless the renderer is llvmpipe.
- `v10_skin_manager_e2e`: Shader Studio's Refresh Preview and the table's Test both put the graph material on.
  - The node canvas: the default graph; Add node via the palette, search and arrow keys; a wire dragged
    port to port; a wire dropped on the canvas (the palette for vec3); a node drag and Undo; pan; wheel zoom; Fit;
    Delete and Undo; Export JSON; Templates.
  - Save checks the edited graph in `customShader.graph`.
  - The other editors: Paintable and its mask, the Wear layers packed into `texture_w.png` (its first pixel
    checked), the preview's wear and paint query, the Kounter (`[viewer] showcase counter`), the charm placement,
    and a paint's colour and Animated Shader canvas.
- `tools/modshader/sample_e2e.sh`: grey and invert mods (`client/tests/data/mod_shader/`) on a Sandstorm copy
  whose `modURL` points at them, headless from a pinned camera. Checks: grey frame R=G=B with the plain frame's
  Rec. 709 luminance, invert frame = 255 minus plain.
- uidiff `settings-advanced-render-shaders` (KN reference): the rows with two passes loaded (`state.modShaders`).
