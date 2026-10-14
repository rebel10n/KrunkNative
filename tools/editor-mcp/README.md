# kn_editor_mcp: Krunker map editor MCP server

`kn_editor_mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server, written in C11, that edits
Krunker maps. Its design follows the Unity MCP gateway:

- rich structured scene queries;
- batched, validated edits on one undo stack;
- multi-angle visual feedback;
- logs;
- validate / compile / playtest loops.

It works on two kinds of project:

- **local map files** (file mode), and
- **live editor projects** in a running Krunker Box Office editor, reached over the editor bus (live mode).

## Build, test, run

```sh
make editor-mcp          # cmake-build-debug/tools/editor-mcp/kn_editor_mcp (also built by plain `make`)
make editor-mcp-test     # MCP stdio test suite, every tool (part of `make check`)
make editor-mcp-install  # release build -> ~/.local/bin/kn_editor_mcp (EDITOR_MCP_PREFIX=...)
tools/kref/editor-mcp-smoke.sh   # live mode against a headless editor (Kanji + Burg)
```

`kn_editor_mcp [options]` speaks MCP over stdio by default. The options are:

| option | meaning |
|---|---|
| `--root DIR` | Krunker Box Office checkout (assets, `client_bin`). Auto-detected: `KRUNKNATIVE_ROOT`, the binary's location, cwd, `~/src/KrunkNative`. |
| `--workspace DIR` | Scratch dir for new maps, compiled output and the prefab library. Writes are allowed here. Default `$XDG_DATA_HOME/krunknative/editor-mcp`. |
| `--allow DIR` | Another directory maps may be read from and written to (repeatable). |
| `--open MAP` | Open a map at start: a path or a catalog name (`burg`, `Kanji`). |
| `--schema FILE` | Schema override. Default `<root>/tools/editor-mcp/data/schema.json`, else the embedded copy. |
| `--client PATH` | `client_bin` for renders. Default: the newest of `<root>/cmake-build-{release,debug}/client_bin`. |
| `--read-only` | Refuse every edit and save. |
| `--live` / `--no-live` | Require the editor bus / never use it. Without either, it attaches automatically when an editor hosts a bus. |
| `--bus-dir DIR` | Editor bus directory. Default `KRUNKNATIVE_EDITOR_BUS_DIR`, else `$XDG_RUNTIME_DIR/krunknative`. |
| `--scopes read,edit,project` | Permission scopes to request from the editor (default: all). |
| `--permission-timeout S` | How long to wait for the editor's permission dialog (default 300). |
| `--http HOST:PORT` `--announce FILE` | Serve MCP Streamable HTTP on `POST /mcp` instead of stdio. Port 0 picks a free port; the URL is written to FILE. |
| `--call TOOL [JSON]` | Run one tool and print the result (scripts, CI). Exits 1 on a tool error. |

## Registering with Claude Code

The project example is [`mcp.example.json`](mcp.example.json). Copy it to `<repo>/.mcp.json` to use it as a
project-scoped server. To register it for every project (user scope), install the binary and add it with the CLI:

```sh
make editor-mcp-install
claude mcp add --scope user krunker-editor -- ~/.local/bin/kn_editor_mcp --root /home/user/src/KrunkNative
claude mcp get krunker-editor
```

Without `--live` / `--no-live` one registration serves both modes:

- local maps through `map_open`;
- live editor projects whenever `client_bin --editor --mcp-bus` is running.

## Architecture

```
stdio / HTTP ─ mcp.c (JSON-RPC 2.0, lifecycle, tools, resources, prompts, logging)
                 │
     tools_project.c  tools_objects.c  tools_logic.c  tools_build.c  tools_check.c  tools_script.c  render.c  playtest.c
                 │            (every mutation: edit_begin -> mutate the document -> edit_commit)
             project.c ── file projects: Doc + snapshot undo/redo (doc.c)
                 │     └─ live projects: mirror of the editor's export {full:true}; commit diffs the
                 │        mirror by object uuid into one bus "batch" (one editor undo step)
   doc.c (map JSON, ids, properties)  schema.c (data/schema.json)  jsonw.c (JSON.stringify output)
   validate.c ── shared/src/map.c map_init, bot_nav.c (reachability), map_validate.c (v7 validateMap)
   ks_bridge.c ── shared/include/ks.h ks_validate (KrunkScript), weak-linked
   live.c ── shared/include/editor_bus.h (framing, token, hello / permission handshake)
   proc.c ── client_bin subprocesses only (renders), no shell
```

### Map format and round trips

- The document is the map's own cJSON tree. Key order, unknown keys, legacy keys (`id`, `col`, `hp`) and
  v7 fields are all kept.
- `jsonw.c` prints exactly what `JSON.stringify` prints: shortest round-trip numbers in JavaScript's layout,
  JavaScript string escapes, the document's key order. Every map under `assets/maps/` (36) and every v7.1.4
  map (53) saves byte-identically unless it was edited.
- `format`: `source` (like the loaded file), `compact`, `pretty` (2-space indent, short number arrays inline)
  or `indent2` (exactly `JSON.stringify(v, null, 2)`).
- Object ids are session ids stored in the cJSON node (never written). Live projects use the editor's objUUIDs.
- Spawns are the top-level `spawns` array in files and `SPAWN_POINT` objects in the editor. The spawn tools
  hide the difference, and `doc_map_form()` converts the editor form for the game code.
- Colours fold into the `colors` palette (`ci` / `ei`) when the map has one, else they are written inline as
  `c` / `e`. Pooled sizes (`si` + `xyz`) are read; editing a size writes `s`.

### Schema

The schema is data, in [`data/schema.json`](data/schema.json), generated by `gen/gen_schema.py`. It holds:

- the 97 prefabs of v7.1.4 (ids 0-59 are Krunker Box Office's native v3.1.1 set), with flags, default sizes and
  categories;
- 20 textures;
- 162 object properties (friendly name, map key, type, default, range, the prefab flags / names allowed to
  carry them, `since`);
- the top-level map keys;
- the trigger events (32), actions (72) and conditions (12);
- the server config, modes, classes and limits.

The schema is embedded at build time. An edited `<root>/tools/editor-mcp/data/schema.json` (or `--schema`)
overrides it, so v7 fields drop in without touching C.

Edits are checked against it:

- A property the prefab does not use fails unless `force:true`.
- An out-of-range value fails unless `clamp:true`.
- `raw` keys are written as given with `force:true`.

### Undo

- **File projects:** every mutating tool is one transaction. The document before the edit is kept as compact
  JSON plus the id list, up to 200 steps or 256 MB. Undo / redo restore it byte for byte.
- **Live projects:** the editor's own stack. A transaction becomes one `batch` request (add / replace /
  remove / map_set / map_unset), which the editor records as one labelled undo step.

### Live mode (editor bus)

- The protocol is `shared/include/editor_bus.h` (v1): a unix socket plus a token in the bus dir, and
  length-prefixed JSON frames.
- **Connecting:** `kn_editor_mcp` connects at the first tool call that needs an editor, or on
  `editor_connect`. It then sends `hello` and `permission_request` with its client id, name, version and
  scopes. Connecting is deliberately lazy: Claude Code starts servers at session start, and a dialog then
  would block `initialize`.
- **Permission:** the editor shows its permission dialog: Allow once / Always allow / Deny.
  `--mcp-auto-accept` or `--mcp-auto-deny` on the editor answers it in tests. A deny is cached: tools fail
  with "the editor denied … access", and only `editor_connect {retry:true}` asks again. A reduced grant fails
  the tools that need the missing scope.
- **Projects:** they appear in `projects_list` with ids like `1.2`. Every tool takes `project`: an id, path,
  basename or map name; the default is the focused project.
- **Operations:** reads use the mirror (`export {full:true}`, refreshed when the editor's revision changes).
  Edits are diffed into one `batch`. Undo / redo / history and saves are forwarded. Renders use the
  editor's own renderer (`render`, split multiview).

### Rendering

- **File projects:** `client_bin` runs headless as a subprocess (`KRUNKNATIVE_HEADLESS=1`: an EGL pbuffer,
  no window) on a temporary copy of the map. The camera is pinned with the `client/src/debug_capture.c`
  knobs:
  - `KRUNKNATIVE_CAMERA="x,y,z,pitch,yaw"`, `KRUNKNATIVE_FOV`;
  - `KRUNKNATIVE_ORTHO=<half height>`, `KRUNKNATIVE_NO_FOG=1`, `KRUNKNATIVE_CAMERA_FAR`;
  - `KRUNKNATIVE_HIDE_UI=1`, `KRUNKNATIVE_SCREENSHOT`.
- GL never enters the MCP binary.
- **Camera convention** (Krunker): radians; forward = (-sin yaw · cos pitch, sin pitch, -cos yaw · cos pitch).
- **Multiview:**
  - iso: perspective, yaw 45°, pitch -35.26°;
  - front: orthographic, looking -Z;
  - top: orthographic, looking down, screen up = -Z;
  - right: orthographic, looking -X.
- **Framing:** without `ids`, views frame the "core" of the map: objects whose centres lie in the 4th-96th
  percentile box, plus spawns. Backdrop and skyline pieces don't shrink the map. `framing:"all"` frames
  everything.

### Playtest

The map runs in Krunker Box Office's own game code: `shared/src/game.c` as the offline authority, the v7.1.4 bots of
`bots.c`, and the `bot_nav.c` grid. The simulation runs in a forked child process, so it is crash-isolated,
leak-free (the game code has no full teardown) and killed on timeout. It reports:

- spawn reachability (A* between all spawn pairs);
- falls (world deaths below the death height, with the last ground position);
- stuck spots (no movement for 3 s while not fighting);
- kills, coverage, and tick timings (avg / p95 / max against the 60 Hz budget).

An optional scripted walker follows `path` waypoints, and a screenshot is taken from spawn 0.

### Validation

`map_validate` combines:

- **Krunker Box Office's loader** (`map_init`): what the native game loads, and prefabs it does not implement yet;
- **schema checks**: types, NaN, ranges, palette and size-pool indices, property applicability, textures
  missing from `assets/textures`;
- **logic**: trigger events and actions (v7-only ones flagged as not simulated), invalid `tct` text,
  dangling interface targets, teleporter channels without receivers;
- **spawns**: inside solids, over the void, overlapping, team balance, and unreachable via the nav grid;
- **limits**: objects, collidable objects, spawns, signs, publish size (500 000 characters);
- **KrunkScript**: `ks_validate`;
- **v7.1.4's own `validateMap`** (`shared/src/map_validate.c`, `v7.*` codes): exactly what publishing to v7
  would reject.

Each diagnostic is `{severity, code, message, object_ids?, spawn?, line?, col?}`.

### Compile

`map_compile` produces the publishable v7.1.4 form, like the editor's `getMapExport`:

- editor-only prefabs are dropped;
- editor spawn and camera objects become `spawns` / `camPos`;
- `p` / `s` / `r` are rounded and defaults stripped;
- colours are folded into a palette and sizes pooled into `xyz` / `si`;
- KrunkScript is checked (`ks_validate`) and base64-encoded.

It blocks on validation errors unless `force:true`, and it verifies that the output loads in `map_init`.
Bake steps:

- `tris`: complex-collision (`cpx`) cubes, cylinders, spheres and planes are baked into `tris` with the editor's own
  bake (`client/src/editor/editor_model.c`, linked GL-free; it reproduces the triangles v7.1.4 exports); custom assets
  need their model and stay boxes (`bake_tris:false` skips it);
- v7 exports carry no lightmaps.

## Safety

- Maps are read and written only under the allowed directories: `<root>/assets/maps`, the workspace, and each
  `--allow DIR`. Writes must be `.json`. Overwriting a different existing file needs `overwrite:true`.
- Subprocesses are limited to Krunker Box Office's own `client_bin` (found by the server itself), exec'd directly with
  a minimal environment (no `DISPLAY`) and a timeout. There is no shell and no arbitrary command.
- HTTP mode binds 127.0.0.1 by default and rejects non-local `Origin` headers.

## Tools

R = read-only (annotations `readOnlyHint`). Every tool also accepts `project`. `*` = required. Full JSON
schemas come from `tools/list`.

| tool | | arguments |
|---|---|---|
| `projects_list` | R | — (projects, focus, editor connection / permission state, allowed dirs) |
| `map_open` | W | path*, editor, reload, focus — a file under the allowed dirs or a catalog name |
| `project_open` | W | alias of map_open |
| `map_new` | W | name, template (blank / empty / arena), path, overwrite, editor |
| `map_save` | W | path, as, format (source / compact / pretty / indent2), overwrite |
| `map_save_as` | W | path*, format, overwrite |
| `project_close` | W | force |
| `project_focus` | R | project* |
| `editor_connect` | W | retry, scopes — live bus handshake / retry after a deny |
| `map_info` | R | — counts by category / prefab, bounds, spawns per team, triggers, interfaces, modes, lighting, limits |
| `map_settings_get` | R | group, keys |
| `map_settings_set` | W | set {key: value}, unset [keys], config {modes, classes, settings}, force |
| `undo` / `redo` | W | steps |
| `history` | R | limit |
| `logs` | R | source (all / mcp / tool / live / render / playtest / editor), level, tail |
| `prefabs` | R | name (full property schema), category, search |
| `objects_query` | R | ids, prefab, category, text, bbox {min,max,mode}, near {center,radius}, where, has, color, texture, collidable, visible, team, interface_id, target_interface, links, fields (summary / full / props / ids), sort, limit, offset |
| `object_get` | R | id or ids, defaults |
| `objects_create` | W | objects* [{prefab, p, s, r / rotation_deg, props, raw}], snap, force, clamp, label |
| `objects_update` | W | updates* [{id, p, s, r, rotation_deg, prefab, props, raw, unset}], snap, force, clamp, return_objects |
| `objects_delete` | W | ids* |
| `objects_duplicate` | W | ids*, offset, count |
| `objects_transform` | W | ids*, op* (move / rotate / scale / snap / align / distribute / mirror / drop) + op arguments |
| `objects_copy_between_projects` | W | from*, to*, ids*, offset or at |
| `spawns_list` | R | — (index, p, team, dir, facing, inside_object, ground_y) |
| `spawns_add` | W | spawns* [{p, team, dir or yaw_deg, comp}], snap_to_ground |
| `spawns_update` | W | updates* [{index, ...}] |
| `spawns_delete` | W | indices* |
| `triggers_list` | R | — decoded events / actions / targets |
| `triggers_catalog` | R | — v7 events (32), actions (72), conditions (12), native flags, keys |
| `triggers_create` | W | triggers* [{p, s, event, event_value, action, value / text, chance, target, interface_id, single_use, team, sound...}] |
| `triggers_update` | W | updates* [{id, ...}] |
| `triggers_link` | W | trigger*, targets*, interface_id |
| `interfaces_list` | R | — members, targeted_by, dangling, next free id |
| `group_create` / `group_list` / `group_delete` | W/R/W | name, ids, delete_objects |
| `group_stamp` | W | name or ids, at / offset, step, count, rotate_y_deg, new_group |
| `prefab_save` / `prefab_list` / `prefab_stamp` | W/R/W | name, ids or group, description / at, step, count, rotate_y_deg |
| `generate_box_room` | W | center, size, wall, floor, ceiling, doors [{side, offset, width, height}], props, floor_props |
| `generate_stairs` | W | start, direction (+x / -x / +z / -z), height, width, step_height, step_depth, steps, style (steps / ramp), props |
| `generate_platform_path` | W | points or start + end + count, size, jitter, seed, max_gap, max_rise, props |
| `fill_area` | W | bbox*, prefab, count or spacing, jitter, scale_range, random_yaw, on_ground, avoid_overlap, avoid_spawns, seed, props |
| `map_validate` | R | featured, reachability, max_diagnostics, min_severity |
| `map_compile` | W | output, inline, round, pool_sizes, fold_colors, strip_defaults, drop_editor_only, encode_scripts, bake_tris, scripts {client, server}, force |
| `script_get` | R | side (client / server / both), source — decoded KrunkScript (the template when the map has none), lines, ksc's diagnostics |
| `script_set` | W | side*, source* ("" = the template), force — stored as the editor exports it (UTF-8 base64, the template omitted); a compile error is refused unless force; live projects reach the editor's script window over the bus |
| `playtest` | R | seconds, bots, mode, path [[x,y,z]...], screenshot, seed |
| `render_view` | R | position + target, or position + yaw / pitch, or ids; fov, ortho, framing, fog, width, height |
| `render_multiview` | R | ids, framing, fog, width, height → 4 images (iso, front, top, right) |
| `render_topdown` | R | x, z, width, depth, pixels_per_unit, y_min, y_max, framing, fog |

Coordinates are Krunker units with y up. `p` is an object's bottom centre and rotations are radians;
collision ignores rotation.

## Resources and prompts

- `krunker://map/current`, `krunker://map/{project}`, `krunker://map/current/summary`;
- `krunker://prefabs`, `krunker://prefabs/{name}` (the full property schema), `krunker://properties`,
  `krunker://textures`, `krunker://triggers`;
- `krunker://catalog` (official and community maps), `krunker://config` (map keys, game / server config,
  modes, classes), `krunker://limits`;
- `krunker://docs/editor-mcp` (this file), `krunker://docs/v7/{name}` (`doc/v7/*.md`).

Prompts: `build_arena`, `review_map`, `fix_validation`.

## Files

| file | role |
|---|---|
| `src/main.c` | options, root detection |
| `src/ctx.c` | allowed directories, path safety |
| `core.cmake` | the tool core compiled into `client_bin`: the map editor's AI panel runs these tools in-process (`client/src/editor/editor_ai_tools.c`; live mode with `Live.loopback` instead of the bus socket) |
| `src/mcp.c` `src/http.c` | protocol core (stdio), Streamable HTTP transport |
| `src/project.c` `src/live.c` | projects, edit transactions, live editor bus client |
| `src/doc.c` `src/schema.c` `src/jsonw.c` | document model, schema, JSON writer |
| `src/tools_*.c` `src/render.c` `src/playtest.c` | tools |
| `src/validate.c` `src/ks_bridge.c` | validation, KrunkScript bridge |
| `src/proc.c` | `client_bin` subprocesses |
| `data/schema.json` `gen/` | schema data and its generator (`python3 -I tools/editor-mcp/gen/gen_schema.py`) |
| `tests/mcp_test.py` | stdio test suite (`make editor-mcp-test`) |
