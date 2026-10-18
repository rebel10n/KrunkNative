# Map editor: Krunker v7.1.4 parity spec

Status: survey and plan, no code changes yet (written 2026-10-12).
Policy: Krunker Box Office is upgraded **in place** to Krunker v7.1.4. There is no
v3.1.1 compatibility mode; v3.1.1 lives on branch `v3.1.1` / tag
`v3.1.1-parity`. Wherever the v7.1.4 editor differs from v3.1.1, the native
editor takes the v7.1.4 behaviour. Old (v3.1.1) map files must still import,
because v7.1.4 itself imports them: legacy keys `col`, `hp`, `wid`, `tr`,
top-level `game`, and single-action triggers.

Related specs, being written in parallel:

- `doc/v7/krunkscript.md`: the KrunkScript language, compiler/validator and
  runtime. Section 1.8 below covers only the **editor UI** for scripts.
- `doc/v7/map-logic.md`: runtime semantics of the v7 prefabs, triggers
  (events, conditions, actions), custom GUI elements, the map `config` block
  and environment zones. This spec covers only how the editor **shows, edits
  and serializes** these.

## 0. Sources and abbreviations

All v7 references are under `/home/user/jails/kru/src/Krunker_v7.1.4/` (`R/`).
The trees are read-only.

| Abbr | File | Notes |
|---|---|---|
| E | `R/src/editor.js` (7043 lines) | `Editor` class, global `KE = new Editor("2.0.0")` (E:7007; `config.editorVersion`) |
| UI | `R/src/ui/editor.js` (7776 lines) | GUI blueprint builder: toolbar, panels, windows, Quick GUI |
| OI | `R/src/libs/ObjectInstance.js` | per-object state, `serialize()` (OI:1006-1963), defaults (OI:372-800) |
| PF | `R/src/data/prefabs.js` | 97 prefabs (PF:329-2570), 20 textures (PF:13-292) |
| TR | `R/src/data/triggers.js` | 32 events, 12 conditions, 72 actions |
| KSS | `R/src/data/krunkScriptSyntax.js` | client/server templates, 211 autocomplete entries, alias table |
| ST | `R/src/data/scriptTemplates.js` | 18 KrunkScript template maps |
| CGC | `R/src/data/customGameConfig.js` | map/config validation (`validateMap`, `validateConfigs`) |
| CFG | `R/src/config.js` | serverConfig, classConfig, weaponConfig, effectConfig, limits |
| EO | `R/src/libs/EditorOverlay.js` | 2D overlay: FPS counter and custom GUI preview |
| TC | `R/src/libs/TransformControls.js` | gizmo (with Krunker's fakeScale, modelScale and directionalScale patches) |
| H | `R/public/editor.html` | page skeleton; CSS is `R/public/css/editor.css`, `editor_dark.css`, `editor_custom.css` |
| SRV | `R/server/index.ts:3742-3830` | local `/editor/ws` socket stubs |

The v3.1.1 editor is at `/home/user/jails/kru/src/Krunker_v3.1.1/src/editor.js`
(4816 lines) and `src/ui/editor.js` (5456 lines). Line refs in section 2 marked
`v3E:` and `v3U:` point at those files.

**Missing from the reference.** `R/public/scripting.html` (the KrunkScript code
window that E:2275 opens) is not in any tree on disk. It was recovered from the
Wayback Machine's krunker.io snapshots (2021-10 .. 2025-02 are the same page apart
from build ids; WP5 uses the 2025-02-03 one, from the era whose editor page loads
monokai like H): `tools/editor/fetch_scripting_ref.sh` fetches it, its
`css/scripting.css` and the CodeMirror 5.61.1 files it loads into `~/.cache`
(pinned hashes; Krunker's script is never committed). Section 1.8 describes it; the
reference capture (`v7ref.js scripting*`) opens it with v7's own `KE.scriptPopup()`.

---

## 1. v7.1.4 editor feature inventory

### 1.1 UI layout, panels and menus

**Page skeleton (H:36-125).** The page has these regions:
- `#vignette`: drag-and-drop highlight.
- `#uiBase`: holds the popups, `#menuHolder > #windowHolder` (window frame), `#hotbar`, `#view` and `#editorUI` (`#progress`, `#instructionHolder`).
- `oncontextmenu` is disabled on the body.

UI scaling (`scaleUI`, E:6110-6120): `elemScale = min(h/937, w/1920) * settings.uiScale`, applied to `#uiBase` as a CSS `transform:scale`.

**Hotbar** (30 px, grid `45% 5% 50%`; editor.css:1118-1130) contains:
- `#_toolbar`: the menus.
- `#playbar`: three icon buttons.
- `#rightMenu > #versionTxt`: "Krunker Editor v2.0.0 - Yendis Entertainment".

**Toolbar menus** (UI:543-688, `_toolbar.show()`; rebuilt on login and account change):

| Menu | Items (behaviour) |
|---|---|
| File | **Import Project**: paste popup; Shift+click opens a file picker. **Import Old Project**: only with `?dev`; runs `importOldMap`, E:3828. **New Project**: confirm, then `clearMap`. **Save Project**: downloads `<name>_save.txt`. **Load Backup**: window 15. **Export**: downloads `<name>_export`; Shift+click also copies it. **Copy Export**. **Export GLTF**: E:3608. **Export MTL**: OBJ+MTL+PNG zip, E:3656. |
| Edit | Undo (Ctrl+Z), Redo (Ctrl+Y), Delete (Del), Duplicate (Shift+R), Settings... (window 3). |
| Object | **Scene Graph**: right panel `objectList`. **Basic / Models / Tools / Lights / Zones**: prefab submenus. A prefab is skipped if it is `custom`, `noExport`, nameless, or `localOnly` in prod. Category: `edCat=="lights"` → Lights, `zone` → Zones, `tool\|\|tool2` → Tools, `gen` → Models, otherwise Basic. Models and Tools use 12-row column chunks. Shift+click a prefab runs `reuseSelected(i)` (E:5954): the new prefab takes the selected object's pos and size. **Placeholder**. **Group ▸** Create / Stop / Stop All / Copy / Cut / Paste / Prefab ▸ (Import ▸ JSON, File; Export; Tools). **Custom...**: asset browser for models and prefabs. |
| Components | Environment, Terrain, Zone, Ocean, GUI (each opens its right panel). |
| Scripting | `KE.scriptPopup()` (section 1.8). |
| Game | Test Game, Host Game, Quick Start (`loadQuick(0)`: the first game template, E:6206), Templates (window 10), Tools (right panel `tools`). |
| Publish | Game (`publishMap`, E:3712), Asset (`publishAssetPopup`, E:6530), Publishing Guide (window 11). |
| Account | "Logging in...", or "Username: X" with My Games (window 14) and My Assets, or Login (`loginPopup`). |
| Help | Controls (window 1), Guide (window 8). |
| Documentation | Opens `//docs.krunker.io`. |

**Playbar** (H:63-67):
1. ▶ calls `KE.testMap()`. Right-click calls `KE.testMap(false, true)`, which spawns the player at the editor camera.
2. `video_settings` calls `KE.testMap(true)` (Host Game).
3. `explore` calls `KE.teleportPopup()` (section 1.10).

**Side panels** (H:73-118, `src/libs/Resizable.js`, UI:690-719):
- Both panels default to 300 px. Drag-resize is clamped to 220..600 px.
- The arrow button hides a panel and the restore tab brings it back. When a panel is hidden, `#canvasUI` or `#canvasInfo` shifts by 43 px.
- The lock button (`GUI.panel.state.lock`) pins the current view: `show()` for any other view returns early.
- Scrolling a panel hides the colour picker.
- Default views: left `essential`, right `blank`. On select, deselect and gizmo mouse-up, `updateObjConfigGUI()` (E:4445) shows the right `object` panel.

| Side | Views |
|---|---|
| Left | `essential` (map and game config, section 1.9) |
| Right | `blank`, `object` (UI:1267, header is the object name or "Group"), `environment` (5832), `terrain` (5970), `zone` (6016), `ocean` (6045), `tools` (6087), `prefab` (6202), `objectList` (6258, flat list with a Focus button per object), `gui` (6288) |

**`#canvasUI`** overlay (H:81-93). Each toggle gets background `rgba(0,0,0,.7)` when ON:

| Button | Action |
|---|---|
| translate / rotate / scale | `setTransformType(0/1/2)` (E:4346) |
| `#spaceOption` | toggles World/Local (E:4367) |
| `#faceOption` | "Face Selection: ON/OFF" |
| `#groupOption` | "Group Selection: ON/OFF"; turning it OFF commits the group |
| `#wireframeOption` | "Wireframe: ON/OFF"; sets `ObjectInstance.forceWire` (E:2900) |
| `#snappingOption` | "Snapping: ON/OFF"; `setSnapping` (E:2890) |
| `#chatHolder > #chatList` | warning and notice feed (`doWarning`, E:3097). This is not a chat. |

Quirk: snapping is ON at boot (E:1302), but the HTML label reads OFF until saved settings exist or the user toggles it.

**`#canvasInfo`** stats (H:95-105, `updateObjectCount` E:6140-6158):
- Spawn Points `n / 20`.
- Collidable Objects.
- Non-Collidable Objects.
- Event Objects `n / 10` (non-prod only).
- Server Nodes.
- Total Objects (spawns excluded).

Classification is done by `cntCheck` (E:3111).

**Other canvas controls.** `#canvasAdd` "+" opens window 4 (Quick Add). `#canvasObjEdit` has Delete and Duplicate buttons. Both are group-aware.

**Windows** (`window.windows[]`, E:373-985; `showWindow(n)` uses index n-1 and disables the editor until `closeWindow()`/Esc):

| n | Window | Contents |
|---|---|---|
| 1 | Controls | 600 px. Tabs Default and Advanced; key rebinding and speed (UI:6562-6990). |
| 2 | Assets | 1292 px. Sort New/Alphabetical; tabs New, My Assets, Search (with category). Cards show model, prefab (`d`), texture (`x`), sound (`u`, play button) or video (`v`). Right-click copies the id; the owner sees a delete button. Clicking calls `selectAsset`. |
| 3 | Settings | Tabs General, Display, Object, Quick GUI, plus Reset All (section 1.11). |
| 4 | Quick Add | Search box; Enter adds the first match (shortest name first). 8 customizable slots; Customize mode. |
| 5 | Mods | Tabs from `config.modTabs`, search; selecting one sets `mapConfig.modURL`. |
| 6 | Quick Add Customize | Lists General / Basic / Models / Tools entries to assign to a slot (UI:7540-7632). |
| 7 | Textures | Asset browser, category 16. |
| 8 | Welcome | 80% width. Shown at boot unless a template or edit is loaded or `?openPub` is set. Quick Start, My Games, Templates, Publishing and Documentation buttons; animated counters (160M users, 12 regions, $750k); edu and Discord links. |
| 9 | Sounds | Asset browser, category 17 ("My Sounds" tab). |
| 10 | Templates | Horizontal scroll. Script templates (tag "KrunkScript", `/img/templates/scripts/<i>.png`), game templates, default maps (tag "Default Map", `/img/maps/map_N.png`), then Import / Load / New / Guide tiles. |
| 11 | Publishing | Static guide with good and bad thumbnail examples. |
| 12 | Tutorials | YouTube iframes (`config.tutorials`). |
| 13 | Tips & Tricks | iframes (`config.tipsAndTricks`). |
| 14 | Maps (My Games) | `listMaps`; each card has Edit (`openEdit`) and Delete (`deleteCreationConf`). |
| 15 | Load Backup | 400 px. IndexedDB backups newest first ("date - name"); click to import; Clear All. |

**Popups** (`genericPopup`, E:1119):
- `importPop`: paste textarea.
- `teleportPop`.
- `listPop`: texture, spray, skin or model-variant chooser.
- `loginPop`.
- `editPop`: Load in Editor; leaderboard settings; map funds deposit/withdraw with a 10% fee.
- `confSubmitPop`: delete confirmation.
- `publishGamePop`, `publishPop`.
- `mapRespPop`: share link `/?play=<name>`.
- `mapTestPop`: iframe, when `testInEditor` is on.
- `#twoFAPop`: 6-digit code.

**Quick GUI** (context menu, UI:7633-7760; E:2286, 4448):
- A QuickSettings panel, 150 px wide, at the cursor; flips up near the bottom of the screen.
- Opens on middle click by default (`quickGUIOpen` 0) or right click (1).
- Items: Duplicate, Delete, Group ▸ Create/Stop/Copy/Paste, Fix Hitbox, Replace ▸ JSON/File, JSON (textarea with the serialized object, or the group's members).

**Theme.** Dark mode is on by default: `editor_dark.css` is toggled via `disabled = !settings.darkMode` (E:1884). `editor_custom.css` is an empty hook for user overrides.

**Boot** (E:1205-1311, 7007-7029):
- Every prefab gets a `createObjects[i]` entry; `CUSTOM_ASSET` opens the asset browser instead.
- Init order: generateLists, initSettings, initRenderer, sceneInit, initGUI, initQuickGUI, initControls, initHistory, initAutoBackup, registerEvents, setSnapping(true), checkEdit (`mapToEdit`), checkTemplate (`?scriptT=` / `?gameT=`), render.
- `onbeforeunload` saves scripts, closes the script window, takes a backup if enabled, and returns "You have unsaved changes!".
- Editor events dispatched through `KE.on/off/dispatch`: `objectAdded`, `objectRemoved`, `objectSelected`, `objectDeSelected`, `groupCreated`, `groupRemoved`, `mapImportStart`, `mapImportEnded`.

### 1.2 Object placement, transform gizmo, snapping

**Placement**:
- Menu items, Quick Add and the "+" button call `ObjectInstance.defaultFromType(id, pos)` → `addObject`. `pos` is `getNearPosition()` when `settings.createObjectNear` is on (default false), otherwise `[0,0,0]`.
- `getNearPosition()` (E:1513) is the camera position plus `(0,-5,-30)` rotated by the camera quaternion, with each axis rounded.
- **Hotkeys C / G / R always place near the camera** (E:2762-2770). They create CUBE 0, GATE 24 and TRIGGER 29.
- Placeholder (P, `createPlaceholder` E:5128): a 10³ PLACEHOLDER at the rounded camera position, y−10.
- `addObject` (E:3120) adds the mesh, `boundingMesh` (the pick target), `boxShape`, `realHitbox`, `alteredHitbox`, `arrowHelper`, camera helper and light helper. It then updates counts and warnings, records history, dispatches the event, and auto-selects the new object unless told not to.
- **Asset placement** (`selectAsset`, E:1522-1694):
  - Model → `{i:44, aid}`.
  - Texture → `{at}` cube.
  - Prefab asset → fetch `user-assets/<id>/data.json` and place it as a group via `replaceObject`.
  - Other targets route the id into the selected object's field: `assetTexture`, `assetID`, sky textures, sounds, projectile model, class model or crosshair, weapon icon, GUI image, action sound.
- **Drag & drop** a file onto the canvas: imports a whole map (E:2849).

**Gizmo** (THREE.TransformControls, E:2399; TC):
- Modes translate / rotate / scale (keys 1/2/3); world/local space (backtick).
- Gizmo size starts at 1. `+`/`=` and numpad `+` add 0.1; `-` and numpad `-` subtract 0.1, minimum 0.1 (E:2726).
- Per-prefab scale modes (`attachTransform`, E:4312):
  - `fakeScale` 1: X/Z are linked, Y is free.
  - `fakeScale` 2: Y/Z are linked, X is free.
  - **modelScale** (`gen` prefabs without `genGeo`, or with an `assetID`): any axis drag scales `mSize` uniformly. Rounded to 2 decimals, clamped to 0.01..100 (TC:550-559).
  - **directionalScale** (`canDirectionalScale && !modelScale`): hold Shift or Ctrl while dragging an X/Z scale handle. The opposite face stays fixed and the position moves by half the size change. Ctrl flips the side (TC:547-549, 610-619).
- F (`flipXZ`, E:1773) swaps the selection's X and Z scale.

**Snapping** (`updateSnapping`, E:4382-4387):

| Snap | ON | OFF |
|---|---|---|
| Translation | `translationSnapping` (default 1) | 0.01 |
| Rotation | `rotationSnapping` in degrees (default 1) | 0.01 rad |
| Scale | `scaleSnapping` (default 1) | 0.01 |

- Toggled by the canvas button only.
- The Controls window lists "Toggle snapping: Control", but this is **not implemented** in v7.1.4 either: Ctrl only does directional scale.
- Settings sliders: translation and scale 0.001..25, rotation 0.001..45, step 0.001. "Reset Snapping" sets all three back to 1.

**Face selection** (M, or the button). Clicking an `editFaces` prefab toggles `faces[floor(faceIndex/2)]` (0..5 = Right, Left, Top, Bottom, Back, Front), then regenerates the mesh. It does not select the object (E:2805).

**Alignment helpers.** These are the only alignment tools; there is no drop-to-ground or surface snap:
- Paste and prefab rotation `advancedOptions.rotation` (0..359; 90/180/270 handled specially, otherwise `rotateAroundPoint`, E:4476-4519).
- Map and group Reflect X/Y/Z (E:4654-4694), which also remaps `direction` and swaps face pairs.
- Scale map XYZ (E:5151).
- Frame (E:5892).

**Pickers on the T key** (E:2755, 6902, 6938):
- Prefab with `gen && variants`: model-variant list (thumbnails at `/img/editor-models/<src>[_<id>].png`).
- `sprayTexture` prefab: spray list.
- SHOWCASE: skin list.
- Otherwise: textures × variants plus "None".
- The list has search. With no texturable object selected, it alerts "Select a texturable object first".

### 1.3 Selection, groups, copy/paste, undo/redo

**Selection** (E:2796-2834):
- Single selection only: the gizmo-attached object is the selection. There is no shift/ctrl multi-select and no box select.
- Left click raycasts the bounding meshes. A miss deselects.
- With Alt held and `selectThroughInvis` on (default true), invisible objects and placeholders are skipped (E:2887).
- Double-click (not in face or group mode) moves the camera to the bbox centre plus the back vector × 2 × bounding radius. The camera orientation is kept (E:2780).
- Optional hover highlight (`objectHighlight`): sets a random emissive on the hovered object (E:2835, 4296).
- Shift+V toggles visible and Shift+C toggles collidable; both apply to the whole group if one is selected (E:4802-4825).
- There is no per-object lock or freeze.
- Settings ▸ Object has two buttons: "Lightcone Boundingbox Toggle" and "Model & Asset Boundingbox Toggle" (E:5100, 5114). Each also posts a persistent notice.
- **Scene Graph** (`objectList`): a flat list with a Focus button per object (`focusObject`, E:2872).

**Groups** (placeholder-volume model, E:4755-5022):
- Record: `objGroups[ownerUuid] = {owner, pos, scale, rotY, objects:[uuids]}`.
- **Create from a volume** (Shift+G, or Group ▸ Create): every object whose world AABB intersects the selected box is grouped.
  - The selector is removed and a fitted PLACEHOLDER (pos `[cx, minY-1, cz]`, size `[w+1, h+2, d+1]`) becomes the owner.
  - Errors: "Stretch a cube over your objects then try again", "You cant combine groups".
- **Create by clicking** (N, Group Selection): each click toggles membership and highlight. Turning the mode off builds the group. Errors: "You cant combine groups", "Nothing to Group".
- **Following the owner**: `updateGroups()` runs every frame and applies the owner's translation and **Y rotation only** to the members. The scale branch exists but is disabled (E:4965).
- **Stop**: Shift+G on a group removes the owner and keeps the members. **Stop all**: Shift+Alt+G.
- **Delete on a group** removes the members and the owner.
- **Duplicate on a group**: `duplicateGroup`. Quirk: Edit ▸ Duplicate throws after doing the work (E:4949).
- **Bulk edit** of a group from the object panel:
  - Texture select.
  - Colour prompt.
  - Reflect.
  - Key select (every ObjectInstance setter) plus Execute (prompt; the value is `eval`ed) via `editGroup`, E:5022.
- **Persistence**: groups are saved only in save files (`groups:{uuid:{ids}}`), never in exports (PLACEHOLDER is `noExport`).

**Clipboard** (internal `KE.copy`, a JSON string of serialized objects; it does not use the OS clipboard):
- **Ctrl+C**: the group's members, or the objects intersecting the selected volume. Quirk: the C key also creates a cube (E:2762).
- **Cut**: menu only.
- **Ctrl+V** (`pasteObjects`, E:4866):
  - Needs a selected target.
  - The copied bbox bottom-centre lands at the target's bottom-centre. Rotation is applied and the pasted objects are **auto-grouped**. The target is kept.
  - Errors: "Select a object you would like to replace with your copied objects", "Please copy objects first".
- **Replace via JSON** (prompt or file, `jsonInput`, E:5963):
  - Accepts an array, `{objects}`, or a wrapped map.
  - The selected placeholder is removed unless `assetAutoGroup` is on (default true).
- **Prefab export** (`exportGroup`, E:4846): prompts for a name, re-centres the objects and downloads `prefab_<name>.txt`.
- **Duplicate** (Shift+R): `clone()` (serialize/deserialize) at the **same position**, no offset.
- **Object Painter** (Prefab panel):
  - Shift+left-click on a surface stamps JSON at hit + (0, 0.5, 0) (E:2820).
  - Source: custom JSON, or one of 8 scrap presets: Junk, Cloth, Wood, Wires, Tin can, Chocolate, Carrot, Frog. These are TRIGGER pickups with `tas` (E:66-89).

**Undo/redo** (E:2461-2574):
- Stacks hold `{action:"add"|"remove"|"update", obj, history}`.
- Add and remove are recorded immediately. Updates are found by a **1000 ms poll** that compares `serialize()` with the cached copy.
- Limit `historyLimit` (default 1000, slider 0..1000); `disableHistory` turns recording off.
- Undo of an update restores only `p s r col/l pe t b en h/hp pr tm v ter c e o d`.
- Keys: Ctrl+Z undo; Ctrl+Y or Ctrl+Shift+Z redo.
- `clearMap` and turning history off clear both stacks.
- Reference bugs:
  - Over the limit, `pop()` drops the **newest** entry.
  - Redo of an update re-applies the old snapshot.
  - New actions don't clear the redo stack.

**Hitbox fixer** (Shift+F, E:5039-5080): replaces the selected object with invisible cubes:

| Prefab | Replacement |
|---|---|
| VEHICLE | 2 cubes |
| TREE | 5 cubes |
| CONTAINERR | floor + 3 walls |
| RAMP | Bresenham staircase, merged |

### 1.4 Object property inspector

**Widget kit** (UI:3-492):
- Blueprint builder `f1330`: `addFolder`, `addDiv`, `addOption`, `add(type, label, object, key, args, onChange, wide, sub)`, `addPanelButton`, `addRule`.
- Control types:
  - `button`, `dbutton`
  - `key`: capture, "Press Any Key"
  - `text`: blurs on change
  - `number`: vertical mouse-drag scrubbing, `value -= movementY*step`, clamped
  - `color`: text field with the colour as background; text is black or white by a luminance threshold of 0x777777; opens a shared Pickr
  - `switch`
  - `slider`: number box plus range input, kept in sync and clamped
  - `select`, `selectClick` (select plus icon button)
  - `vector3`: X/Y/Z; only `min` reaches the HTML (reference quirk)
  - `folder` (standard/small/smaller, `sub` indent), `div`, `rule`
- **Pickr**: one hidden instance (nano theme, hue only, HEX, no opacity). Its 14 swatches are the map's most-used non-default colours (`updateColorSwatches`, E:3312), with fallbacks `#fbc02d #2196F3 #b2f252 #ed4242`.
- `GUI.action` (UI:422-492) reads the DOM value: checkbox → bool, text/select → string, anything else → `parseFloat`. A function-valued key is called as a button. Folder open/closed state lives in `folderStore` (setting `preserveFolderState`).
- **There is no multi-object inspector and no mixed-value display.** Multi-edit is the group bulk-edit described in section 1.3.

**Build** (UI:1267-5831): on select, `KE.configVal` (`pos rot size texture collidable visible color emissive opacity direction border assetTexture texOffX texOffY texRot fog intensity distance decay angle penumbra renderSide forceTrans sprayTexture` plus about 300 `basicConfigVal` keys) is copied into `objConfig`. Controls are gated by prefab flags.

Section order:
1. Header controls.
2. **Transform**.
3. **Style** (omitted if `notStyleable`).
4. **Technical & Logic** (omitted if `notTechnical`).
5. Type folders.

**Header controls** (key: label, range, gate):

| Key | Label / type | Gate |
|---|---|---|
| aiName / botName | text "BOT" / "Gnomie" | `ai` / `bot` |
| visible | switch | `!tool \|\| bounce` |
| collidable | switch | `!tool && !forceCollision && !noCollision` |
| complexColl | switch | `canComplexColl` |
| playerCollidable | switch | TRIGGER |
| bulletColl, aiColl | switch | `editBulletCol`, `editAICol` |
| penetrable | slider 0..1 /0.01 | `editPen` |
| canWallJump, canGrapple, canImpulse | switch | same-named flags |
| boost | slider -100..100 /0.01 | `boostable` (RAMP) |
| bounce, crouch | slider -3..3 /0.1, switch | `bounce` (BOOST_PAD) |
| edgeNoise | slider -5..5 /0.1 | `edgeNoise` (PLANE) |
| border, showMinimap | switch | `hasBorder`, `showMinimap` |
| anchorType | Corner 0 / Center 1 | MAP_ANCHOR |
| minimum | slider 0..1e7 /100 | KR_ZONE |
| spin, spinAxis | slider -20..20 /0.1; Y 0 / X 1 | `gen \|\| hasSpin`, `gen \|\| hasSpinDir` |
| openEnded, radiusTop, radiusBottom, thetaStart | switch, 0..1, 0..1, 0..360 | CYLINDER |
| sides, thetaLength | 3..64 /1, 0.1..360 | LIGHT_CONE, CYLINDER |
| leaderType / leaderTypeCust / leaderOrder / leaderResults | Score/Time, text, ASC/DESC, 1..50 | LEADERBOARD |
| direction | 0..3 (5 if `altDirection`, 7 if `extraDirection`) | `customDirection` |
| forceDir | switch | `optionalDirection` |
| team | Default 0, Alpha 1 .. Delta 4. With `noDefault`: Alpha 0 .. Foxtrot 5. `canNeutral` adds Neutral -1 | `teamable` |
| nodeTag, spread | text, 0..50 | SERVER_NODE |
| enclose | switch | RAMP |
| static | switch | CAMERA_POSITION |
| nodeId | 0..1000 | PATH_NODE |
| stype | Starter / Elite / Heroic | PRIZE_WHEEL |
| mSize | 0.01..100 | `gen` |
| mYOff, mROff | -100..100, 0..360 | `ai` |
| sprayTexture, itemID | selectClick (spray list, skin list) | `sprayTexture`, SHOWCASE |
| objectiveId | 0..10 | `canSetObjective` |
| camType (+objectiveLink -1..10 \| forceCam, followPath "id,id") | Spectate 0 / Player 1 | CAMERA |
| stepSrc | Default / Wood / Water / Sand / Snow | `stepSrc` |

**Transform folder**:
- Position and Rotation are vector3s with 2 decimals.
- Rotation is stored in radians and shown in degrees when "Anti Radians" (`degToRad`) is on. It is hidden for `noRotation` prefabs.
- Size: vector3, min 0. Shown when `scalable && !assetID`; `noScaleX`/`noScaleY` lock an axis.
- Groups also get Reflect.

**Style folder** (UI:2062-2554):
- Texture sub-folder:
  - type (`textureList`)
  - variant (`texVar`)
  - assetTexture (with "Select Custom")
  - texStretch, texScale (0..10)
  - forceTrans
  - texRot (0..360)
  - texOffX/Y (0..1)
  - Movement: speed (-20..20) and axis X/Y
  - Frame Animation: frameCount (0..100) and frameSpeed (0..10)
  - Model variant select (`variants`)
  - texEncode: Default / sRGB / Linear
- color, emissive, opacity (0..1)
- texSmooth (custom, TRIGGER, AI_SPAWNER)
- ambient ("Shading"), fog
- cast and receive shadows, forceRender (CUSTOM_ASSET, TRIGGER)
- alphaTest (0..1)
- renderSide: Game Default -1 / Front / Back / Double
- bobD, bobS (AI_SPAWNER)
- fullcone (LIGHT_CONE)
- mimicSprite (`canSprite`)
- assetID with "Select Custom" (ai, CUSTOM_ASSET, TRIGGER)

**Technical & Logic folder** (UI:2555-4625):

| Area | Controls |
|---|---|
| Deposit box | withdraw/deposit 0..100000 |
| Score | `score`: label is Cost, Amount or Score; max 50000 for machines, else 10000 |
| Perk machine | perk: Krum, Double-Up, Western Hands, Swift, Power Slide, Drift Fuze, Holster |
| Single use | single (CHECK_POINT, TRIGGER, ai); singleP "Once Per Player" (TRIGGER) |
| Interaction | method (8 options: Cost Score .. Level); closeable and startClosed (`canUndo`); hidden "No Popup"; interactM "Custom Message" |
| Health set (`hasHealth`) | canDest; startDest + spawnChance (0..100); selfDestruct (TRIGGER); health 0..50000 /10; respawn timer 0..3600 s and random |
| Hitbox (`alterHitbox`) | width 0.1..10, height 0.1..20; critDmg 0..5 (AI) |
| AI_SPAWNER (UI:2863-3536) | Behavior: stuck handling ×3, friendly, target players, target AI, behaviorType, chase distance, roam radius, roam time, pathType (Loop / Do Once / Ping Pong / Random), pathInput "id,id". Movement: speed, turn speed, gravity, velocity movement. Vision: distance, xray. Melee: canHit, damage, rate, range, delay, shake, hit effect. Shooting: canShoot, rate, spread, shot break, offset, arch Y, delay, shake, shot effect. Projectile: asset, damage, speed, size, mesh scale, spin, gravity, glow, transparency, explode. Rewards: skin ids, method, requirement, node. Nametag: level, scale. |
| BOT_SPAWNER (UI:3538-3995; `localOnly`) | class, force class, lives, team, 5 multipliers, recoil; 19 behaviour switches; 12 cosmetic ids |
| WEAPON_PICKUP | weaponId, weaponReplace, randWep, randWepGroup 0..10000, showPickupIcon, noTexture |
| Trigger events and actions | section 1.7 |
| Interface | interfaceId 0..2000 (`canInterface`, not on WEAPON_PICKUP); interfaceIdT 0..2000 (`canTargetInterface`). Both call `checkConnections()` |
| TELEPORTER | node: Sending / Receiving / Both; channel 0..100; momentum; telCheckPoint |
| Script | scriptId "Script #" (text; `canScript`: TRIGGER, CUSTOM_ASSET) |

**Type folders**:
- Render Faces (`editFaces`): six switches.
- Environment (ENVIRONMENT_ZONE): every Environment-panel field, stored per object (UI:4646-5069).
- Lighting:
  - intensity 0..15
  - distance 0..1000 and decay 0..2 (point, spot)
  - lightTarget vector3 (directional, spot)
  - angle 1..90 and penumbra 0..1 (spot)
- Billboard poster: Random or one of 6.
- Particles:
  - texture: default / grass / fog / ember
  - blending: None / Normal / Additive / Subtractive / Multiply
  - count 0..300, size 0..30, scale -10..10
  - speed, deceleration and gravity
- Text (SIGN; LEADERBOARD keeps only font and background):
  - content and align
  - font: size 1..240, no-scale, colour, outline colour, outline thickness 1..50
  - background: colour, visible
- Spawn: cmpSpwn "Starting Area", teamOnly, spread 0..50.
- Liquid: wave speed 0..25, height 0..5; tiles X/Z 2..100.
- Sound (SOUND_EMITTER):
  - type: 73 emitter sounds
  - assetSound with "Select"
  - volume 0.05..2, rate 0.1..4
  - emitter only: reference distance 0..5000, distance model Inverse/Linear/Exponential, rolloff 0..1
  - otherwise: emit-from
- AI Sound: death, idle, shoot, shootHit, melee, meleeHit, agro (asset ids with "Select").
- Animations (CUSTOM_ASSET, ai, TRIGGER): speed 0.01..10, start clip.

**Prefab catalogue.** There are 97 entries; the id is the array index (PF:329-2570). Bold marks ids that are new since v3.1.1 or changed meaning.

| id | name | id | name | id | name | id | name |
|---|---|---|---|---|---|---|---|
| 0 | CUBE | 25 | CHECK_POINT | 50 | CAGE | 75 | **PARTNER_ZONE** |
| 1 | CRATE | 26 | WEAPON_PICKUP | 51 | EBARREL | 76 | **SPRITE** |
| 2 | BARREL | 27 | TELEPORTER | 52 | SHOWCASE | 77 | **DOMINATION_ZONE** |
| 3 | LADDER | 28 | TEDDY | 53 | POINT_LIGHT | 78 | **ROPE_LADDER** |
| 4 | PLANE | 29 | TRIGGER | 54 | GHOST | 79 | **ENVIRONMENT_ZONE** |
| 5 | SPAWN_POINT | 30 | SIGN | 55 | **AI_SPAWNER** (was BOT) | 80 | **KPD_ZONE** |
| 6 | CAMERA_POSITION | 31 | DEPOSIT_BOX | 56 | PUMPKIN | 81 | **DUMPSTER** |
| 7 | VEHICLE | 32 | LIGHT_CONE | 57 | RUNE | 82 | **SERVER_NODE** |
| 8 | STACK | 33 | **CAMERA** (was SPECTATE_CAM) | 58 | SKELETON | 83 | **LAMP** |
| 9 | RAMP | 34 | SPHERE | 59 | KNIGHT | 84 | **WATER_TOWER** |
| 10 | SCORE_ZONE | 35 | PLACEHOLDER | 60 | **GIFT** | 85 | **AC_UNIT** |
| 11 | BILLBOARD | 36 | CARDB | 61 | **CHRISTMASTREE** | 86 | **SHEET_METAL** |
| 12 | DEATH_ZONE | 37 | PALLET | 62 | **CANE** | 87 | **ANTENNA** |
| 13 | PARTICLES | 38 | LIQUID | 63 | **LIGHT_BAR** | 88 | **SATELLITE_DISH** |
| 14 | OBJECTIVE | 39 | SOUND_EMITTER | 64 | **PATH_NODE** | 89 | **SPRAY** |
| 15 | TREE | 40 | EVENT | 65 | **DIRECTIONAL_LIGHT** | 90 | **WEAPONBOX** |
| 16 | CONE | 41 | TERMINAL | 66 | **RECTAREA_LIGHT** | 91 | **MYSTERY_BOX** |
| 17 | CONTAINER | 42 | PREMIUM_ZONE | 67 | **SPOT_LIGHT** | 92 | **PERK_MACHINE** |
| 18 | GRASS | 43 | VERIFIED_ZONE | 68 | **PRIZE_WHEEL** | 93 | **AI_REDIRECTION** |
| 19 | CONTAINERR | 44 | CUSTOM_ASSET | 69 | **KR_ZONE** | 94 | **KRUM_MACHINE** |
| 20 | ACIDBARREL | 45 | BOMB_SITE | 70 | **NOPVP_ZONE** | 95 | **BOT_SPAWNER** (localOnly) |
| 21 | DOOR | 46 | BOOST_PAD | 71 | **PVP_ZONE** | 96 | **MAP_ANCHOR** |
| 22 | WINDOW | 47 | TEAM_ZONE | 72 | **LEADERBOARD** | | |
| 23 | FLAG | 48 | CYLINDER | 73 | **TILES** | | |
| 24 | GATE | 49 | POLICE | 74 | **CLOUDS** | | |

Notes on the catalogue:
- Many v7 model prefabs gained `variants`: CRATE 5, BARREL 6, VEHICLE 4, TREE 9, CONE 2, ACIDBARREL 2, DOOR 4, WINDOW 7, EBARREL 3, GIFT 5, LAMP 3, WATER_TOWER 2, ANTENNA 4.
- Per-prefab defaultSize, scale, flags and the exact control set are tabulated in the v7 inspector survey. The generator (WP1) must extract them mechanically from PF rather than by hand.
- `BOT_PATH_NODE` is referenced by export (E:3375) and connection lines, but it is absent from PF. Treat it as dead.

**Compact JSON keys.** Written by `serialize()`, OI:1006-1963. A key is omitted when it equals its default.

| Area | Keys |
|---|---|
| core | `p s i r` (radians; omitted if zero); `si`/`ci`/`ei` pool indices on export |
| collision | `l` (not collidable; legacy `col`), `pl`, `ns`, `nai`, `wj` (inverted: present means not wall-jumpable), `gp`, `ip`, `pe`, `v` (invisible), `cpx` (complex collision, plus top-level `tris`) |
| movement | `b` (boost), `bm` (bounce, default 1), `cr`, `en` |
| health | `h` (legacy `hp`), `cdy`, `sd`, `rsc`, `rt` (seconds), `rr` |
| material | `c e o t tv at spt tox toy tro` (legacy `tr` radians), `tsr tsm ts td fct fs ab nf ft rs alt ten txs msp cst rcv fcr sat smm bo da f` (faces string) |
| logic | `d fd tm sp su sup tac tam w` (legacy `wid`), `wr rw rwg wpi ntx bb ant ch n m tcp nm md cl stc nid ct te tce sfd cha tas in tin wd dp obi obl camt frc fp cstc inm sci krs sty zp nt ssp cp to ec skin` |
| text | `st sc so sb sot bv sf nsf sa`; leaderboard `ldt ldc ldo ldr` |
| liquid / cylinder | `wh ws tx tz`; `fc thl ths opn rtp rbt sds` |
| sound | `esa es ev er ed edm ero ef` |
| model / lights | `ms spn spa aid as ast li lds ld lt la lp` |
| AI | `rmt rmr aic tsd spd grv prs prg prgl prgt prsc prms pjs pjd pra vd idl dth atk atkh mel melh agr fr aac shb sha shof shdy ss mldy mls crd hr mlr mld rew rewm rewr rewn bh pht phi xv tAi tPl aif cs cnh igos dos ivos ain ail ains myo mro exp se he bbS bbD hbh hbw` |
| bot | `btn btc btl btt bthm btdm btrm btsm btfc bttc bttt btfcr btat btft btwt btmt btsh btcr btsd btrd btny btno btjm btlm btvc btgm btr btrs btrc btcpr btcd btcf btcht btch btcbk btcb btcw btcwr btcs btcp btcm` |
| ENVIRONMENT_ZONE | long keys inline: `ambInd ambIndC skyDome skyDomeCol0..2 skyDomeEmis skyDomeEmisTex skyDomeTex skyDomeTexA skyDomeMovD skyDomeMovT shadowR shadowD ambientC ambientI light lightD lightI sunAngX sunAngY sky fogC fogD correctLights toneMapping toneMappingExposure outputEncoding` |

**Reference serialization quirks.** Section 1.13 decides whether to reproduce each one.
- `la` (spot angle) is omitted when it equals 15, but its default is 45.
- Particle `ps psp pg pd psc` are written against a default of 1, but load as 0 when absent.
- `sci` is written from `this.sci` (undefined), so it is saved as `null`.
- Bot `speedMlt` is read from `btspm` but never written.

### 1.5 Material, texture and colour editors

**Built-in textures.** The index is `t` and the variant is `tv` (PF:13-292):

| t | name | variants |
|---|---|---|
| 0 | WALL | 8 |
| 1 | DIRT | 4 |
| 2 | FLOOR | 4 |
| 3 | GRID | – |
| 4 | GREY | 3 |
| 5 | DEFAULT (None) | – |
| 6 | ROOF | 3 |
| 7 | FLAG | 3 |
| 8 | GRASS | 4 |
| 9 | CHECK | – |
| 10 | LINES | 3 |
| 11 | BRICK | 11 |
| 12 | LINK | 3 |
| 13 | LIQUID | 4: Water, Voxel Water, Lava, Greek Water |
| 14 | GRAIN | 3 |
| 15 | FABRIC | 4 |
| 16 | TILE | 4 |
| 17 | **METAL** | 2 |
| 18 | **SAND** | 3 |
| 19 | **SNOW** | – |

- Source file: `<src>[_<variantId>]`.
- The inspector stores the texture **name**; `serialize` converts it to an index.
- The texture picker window (T key) shows thumbnails `/textures/<src>[_<id>].png` and has search.

**Material model.** `StrippedLambertMaterial` with vertex colours, shininess 60, specular 0x292929, nearest filtering.
- `alphaTest` comes from the object, else 0.02 for "a"-type asset textures, else the prefab's value.
- `materialList {Default, Phong, Basic, Lambert}` is defined (E:1713) but **not used** by any control.
- Per-object options: encoding (`ten`), force transparency, render side, alpha test, cast/receive shadows, force render, mimic sprite, texture rotation, texture scale, frame animation.

**Colour editing.**
- Pickr colour field, hue only, plus free hex entry.
- Swatches come from the map's own palette.
- Map-wide tools: Colorize Random, Colorize Input, and Colorize Gold, which is a no-op in the reference (E:5137).
- Color Merging (section 1.12).
- Console helpers: `mergeCols()` folds emissive into colour on cubes (E:330); `swapCols(a, b)` replaces one colour with another (E:339).

**Asset ids.** Fields that take an asset id:
- Object: `assetID` (model), `assetTexture` (texture, or video when it ends in `v`), `assetSound`, AI sounds, projectile model.
- Environment: `skyDomeTexA`, `skyDomeEmisTex`, `ambIndC`.
- Map: `mmId` (minimap), `modURL`.
- Config: weapon `pj_asset` and `assetIcon`; class `aID`, `cID` and `ccID`; GUI image `s`.

Ids are validated by regex: models `[\dtlnesacdgxufq]`, textures `[\dv]`.

### 1.6 Terrain, lights, spawns, zones, ocean

**Terrain** (right panel, UI:5970-6015; `src/libs/Terrain.js`). Procedural only, from a seed: there is no heightmap, sculpting or paint brush. Fields:

| Field | Default / range |
|---|---|
| `terrainSeed`, `terrainTSeed` | text, each with a "Generate Random" button |
| `terrainEase` | 13 easings, default 12 |
| `terrainWidth`, `terrainHeight` | 0..8000, default 3000 |
| `terrainMntMlt` | 0..5, default 1 |
| snow / mountain / grass / dirt / sand colours | `#e8fdff #707070 #5e692f #7f6238 #967f51` |
| `sizeMlt` | default 2, no UI |

- Terrain keys are exported only when a seed is set.
- `canTerrain` flags PLANE, BILLBOARD, LIQUID, LIGHT_BAR and SPRITE.
- Same as v3.1.1.

**Zone** (battle-royale cylinder, UI:6016):

| Field | Default / range |
|---|---|
| `zone` | off |
| `zoneSpeed` | 1..50, default 1 |
| `zoneStages` | **0..10, default 0 (new)** |
| `zoneWait` | **5..500, default 5 (new)** |
| `zoneRnd` | |
| `zoneSize` | 0..10000, default 500 |
| `zoneHeight` | **100..10000, default 1000 (new)** |
| `zoneCol0..2` | `#c542d9` |

Zone keys are exported only when `zone` is on.

**Ocean (new)** (UI:6045):

| Field | Default / range |
|---|---|
| `ocean` | off |
| `ocCol` | `#001e0f` |
| `ocOpac` | 0.9 |
| `ocDist` | 3.7 |
| `ocSpd` | 1 |
| `ocYOff` | -1 |
| `ocRefRes` | 1..6 (32..2048), default 3 |
| `ocW`, `ocL` | 100..20000, default 1000 |

Rendered with `src/libs/Ocean.js`. Ocean keys are exported only when `ocean` is on.

**Lights (v7 adds real light types).**

| Prefab | Fields |
|---|---|
| POINT_LIGHT 53 | intensity, distance, decay |
| **SPOT_LIGHT 67** | target, angle, penumbra, distance, decay |
| **DIRECTIONAL_LIGHT 65** | target, intensity |
| **RECTAREA_LIGHT 66** | intensity, size |
| LIGHT_CONE 32 | fake cone; sides, thetaLength |
| **LIGHT_BAR 63** | |

- Light helpers (incl. RectAreaLightHelper) are drawn in the viewport.
- Global sun: a red wireframe sphere marker at `lightD × (cos X, sin X·sin Y, sin X·cos Y)` (E:1912).

**Spawns.**
- SPAWN_POINT 5 has team, direction, `cmpSpwn`, **teamOnly** and **spread** (new).
- Exported as top-level `spawns: [[x,y,z,team,dir,cmpSpwn?1:0,(teamOnly 1),(spread)]]`, 3 to 8 entries per spawn (E:3422).
- Limit 1..20 (CGC:169-185); the editor only warns.
- CAMERA_POSITION exports `camPos: [x,y,z,static,rx,ry,rz]`.
- Team select goes up to Delta 4 (Foxtrot 5 for flags).

**Zone prefabs:**
- Existing: SCORE_ZONE, DEATH_ZONE, OBJECTIVE, CHECK_POINT, TELEPORTER, PREMIUM_ZONE, VERIFIED_ZONE, BOMB_SITE, TEAM_ZONE.
- **New:** KR_ZONE, NOPVP_ZONE, PVP_ZONE, PARTNER_ZONE, DOMINATION_ZONE, ENVIRONMENT_ZONE, KPD_ZONE.

Each zone prefab has a `lineCol` wireframe colour. All zone prefabs are listed in the **Zones** menu.

**Viewport helpers.**

| Helper | Details |
|---|---|
| Grid | `gridSize` 250, `gridSpacing` 10, `gridOpacity` 0.25 |
| Death barrier | plane at `dthY`; `dbVisibility` off by default, `dbOpacity` 0.5 |
| Real hitbox | toggle |
| Border visuals | toggle |
| Altered-hitbox boxes | |
| Camera helpers | |
| Arrow helpers | directional prefabs |

### 1.7 Trigger / logic editor UI

**Which objects.** TRIGGER (29; hotkey R) and AI_SPAWNER (55; `ai`). The gate is `objType=="TRIGGER" || prefab.ai` (UI:4080).

**Layout inside "Technical & Logic"** (UI:2638-4614), in order:

1. Single Use (`su`).
2. Once Per Player (`sup`, TRIGGER only).
3. Custom Message (`inm`), only for event 16 onInteractKey or event 10 onHoldMouse.
4. Health set, including Destroy On Use (`sfd`).
5. Hitbox folder.
6. **Trigger Event** folder (closed by default):
   - Can Trigger (`ct`, AI only).
   - **Event** select (`te`): AI sees only names containing "onAI"; TRIGGER sees the rest.
   - Event Value (`tce`, 0..10000).
7. **Trigger Actions** folder (closed):
   - When the event is 16: Action Method (`tam`; Cost KR, Cost Score, Require Score, Require Kills, Account, Verified, Premium, Partner, Level) and Action Cost (`tac`, 0..10000).
   - Action Chance % (`cha`).
   - **[Add Action]** while `trigActions.length < triggerActionLimit` (5, CFG:932).
   - Per action i, a closed "Action i" folder. **Remove** is offered for i > 0 only: action 0 can never be removed. There is no reorder and no duplicate.
     - Action select (`ta`); `devOnly` actions are hidden in prod. Changing it rebuilds the panel.
     - Value Type (`tce`): Input / Trigger Value / Player Value / Global Value, shown iff the action `hasCust`. When > 0, a Key text (`tcc`) replaces the number.
     - Text (`tct`) when the action `type` is Text/Both.
     - Number (`tc`, -10000..10000, or a select if `actionOptions`).
     - Second value (`tc2`) iff `hasExtra`.
     - Delay (`tad`, 0..60 /0.1).
     - **Condition** sub-folder: condition select (`cd`), then one widget per `vals[]` entry (`cv0..cv2`). The widget is text if `input`, a select if `select`, otherwise a slider with min/max/step.
     - **Sound** sub-folder, for actions 14, 34 and 54 only: type (`es`), asset id (`esa`) with Select Custom (`selectActionAudio`, tracked via `KE.activeAction`), volume, rate, emit-from (Trigger 3D / Player 3D / Global 2D, `ef`).
8. **Interface** folder: ID (`in`, 0..2000) and Target (`tin`, 0..2000).
9. Script # (`sci`).
10. Animations folder (`as`, `ast`).

**Stored shape:**
```
{"i":29, "te":<event>, "tce":<eventVal>, "cha":<chance>, "su":1, "sup":1, "sfd":1,
 "tac":<cost>, "tam":<method>, "ct":1, "inm":"..", "in":<id>, "tin":<id>, "sci":..,
 "tas":[{"ta":<action>, "tc":<n>, "tc2":<n>, "tce":<valueType>, "tct":"..", "tcc":"..",
         "tad":<delay>, "cd":<cond>, "cv0":..,"cv1":..,"cv2":..,
         "es":<snd>, "esa":<asset>, "ev":<vol>, "er":<rate>, "ef":<from>}]}
```
- At most 5 actions are written.
- `tct` and `tcc` hold at most 70 characters. On load, they are blanked if they contain characters outside `[\w:;()?,.|[\] \-+*$@=^!{}]`.
- Legacy single-action fields (`ta/tc/tc2/tct` at object level) migrate into `tas[0]` on load (OI:698).

**Tables.** The editor shows names and parameter widgets only; runtime meaning is in `map-logic.md`.

- **Events (32):**

| ids | events |
|---|---|
| 0-15 | onShoot, onMelee, onDamage, onEnterObject, onDestroy, onRespawn, onTimer, onProjectile, onThrowingMelee, onPunch, onHoldMouse, onCollision, onStorageEquals/Greater/Lesser, onAIDeath |
| 16-31 | onInteractKey, onPlayerSpawn, onPlayerKill, onPlayerDie, onAIDamage, onNukeStart, onNukeEnd, onExitObject, onCustomValEquals, onCustomValLesser, onGameStart, onObjectiveSwitch, onPlayerDamage, onAIHalfHealth, onCustomValGreater, onAIKillPlayer |

  AI sees only 15, 20, 29 and 31.

- **Conditions (12):**

| id | condition | parameters |
|---|---|---|
| 0 | None | |
| 1 | Player Value | key, comparison type, value |
| 2 | Current Class | class |
| 3 | Trigger Value | key, comparison type, value |
| 4 | Player Count | comparison type, filter (Active / Spectating / Overall), amount |
| 5 | Global Value | key, comparison type, value |
| 6 | has Account | |
| 7 | has Premium | |
| 8 | is Verified | |
| 9 | is Partner | |
| 10 | Player Level | comparison type, level |
| 11 | Inventory Value | comparison type, value |

  Comparison type is one of `[Equal, Not Equal, Greater, Greater or Equal, Less, Less or Equal]`.

- **Actions (72; 6 are devOnly: 59, 60, 62, 65, 68, 69):**
  - 0-31 are v3.1.1's list, renamed in places (e.g. 7 "Teleport Player To Interface").
  - 32-71:
    - Reward KR; Clear Checkpoints; Send Sound; Explosion; Spawn AI.
    - Set / Inc-Dec Player Value; Play Animation; Teleport (player / destructible) to Path Node; Reset Destructible Position.
    - Set / Inc-Dec Interface Trigger Value; Set / Inc-Dec Trigger Value (Self).
    - Change Class; Set Deposit Box Amount; Save Data (+ Save Type `tc2`).
    - Change Primary / Secondary Weapon; Set / Change Global Value; Stop Sound.
    - Show/Hide GUI Element (All / Player); Load Data; Custom Action (`tct` param + `tc2`).
    - Reward Junk*, Reward Skin*; Change Ambient Sound; Give Challenge Progress*; Play Animation (Self); Give Effect (10 effects); Give Challenge Progress (All)*.
    - Give Random Effect; Play Video Ad; Drop Powerup*; Spawn Bot at Node*; Force AI Target; Mode Specific.
    - (* = devOnly.)

**Linking.**
- Objects are linked **only by typed numeric ids**: a trigger's `tin` targets every object whose `in` matches it. Path node ids, GUI element ids and script numbers are typed the same way.
- There is **no click-to-link in the viewport**, no variable table and no node graph.
- Variables are free-text keys:
  - Player: the built-ins `health maxHealth score classIndex`, or any custom key.
  - Trigger.
  - Global.

**Connection lines** (`checkConnections`, E:2576-2640; `settings.triggerCons`, default on). Lines are redrawn every frame with `renderOrder=Infinity`:

| Line | Hue |
|---|---|
| Interface: `in` → objects with matching `tin` | `in × 18` |
| Teleporter: same channel, node 0 ↔ others | `node × 3.6` |
| Bot path chains | dead code (no prefab) |

**Limit warnings** (`#chatList`):

| Limit | Value |
|---|---|
| Objects per interface id | 30 ("Interface limit of 30 exceeded [id:N]", E:3039) |
| Event objects | 10 (non-prod) |
| GUI elements | 80 |
| AI | 40 |
| Bots | 16 |
| Signs | 100 |
| Leaderboards | 2 |
| Prize wheels | 10 |

**Other logic UI.**
- Custom GUI component (section 1.9).
- AI paths are typed ids: PATH_NODE `nid`; AI `phi` "id,id,id"; CAMERA `fp`.
- There is no timeline or keyframe editor. Animation means a clip name plus speed, or texture frame or scroll animation.

### 1.8 KrunkScript code editor

**Opening.**
- Toolbar "Scripting", or hotkey **0** (`scriptingKey`), calls `scriptPopup` (E:2264-2284).
- It opens `./scripting.html` as a **separate browser window** at 10% / 10%, 80% x 80% of the editor window. If it is already open, it is focused instead.
- The window closes when the editor unloads.
- On load the editor calls:
  ```
  initCodeMirror(["GAME"], KSS.autoComplete, KSS.alias,
                 {client: code || templates.client, server: code || templates.server},
                 settings.scriptFont || 22)
  ```
  `scriptFont` defaults to 13 and is set by the Settings ▸ General ▸ Scripting slider (5..30).

**The page** (scripting.html + css/scripting.css, recovered: see section 0):
- `#tabHolder` (36 px, `#222`): tabs `#client` / `#server` (`.tab`, GameFont 12 px `#888`, the selected one `#292d3e`), each with a `.tabIcon` (Material Icons: `.iconE` `error_outline` `#ef2929`, `.iconV` `task_alt` `#98ff00`, `.iconC` `circle` `#888`), and `#label` "KRUNKSCRIPT" on the right.
- One CodeMirror 5.61.1 per tab (`fromTextArea` on `#scriptArea_client` / `_server`), at top 36 px, `setSize(null, innerHeight - compile.clientHeight - 30)`; the hidden tab's wrapper is `display:none`.
- `#compile` (bottom, `#2196f3`, monospace 20 px bold, "Validate Script") calls `opener.KE.updateScript()`.
- Options: `lineNumbers`, `matchBrackets`, `indentWithTabs`, `tabSize 4`, `indentUnit 4`, `styleActiveLine`, `autoCloseBrackets`, `foldGutter` (gutters line numbers + fold), theme **material-palenight** (`.CodeMirror-line span {color:#fff}`, active line `rgba(0,0,0,.2)`, `.code-error` `rgba(255,0,0,.3)`). Keys: `Ctrl-/` toggleComment, `Ctrl-Space` the hint, `Alt-F` findPersistent, `Ctrl-Q` foldCode; on the document `Ctrl+S` = `opener.KE.updateScript()`, F5 / Ctrl+R are prevented; `beforeunload` = `updateScript(true)`.
- `setFont(px)` sets the CodeMirror and hint rows' font size. CodeMirror measured its gutters at scripting.css's 22 px first, so with the default 13 px the number column stays 35 px wide and the text starts at 54 px (until the line count gains a digit).
- `highlightErrors(lines)`: `markText({line: n-1, ch: 0}, {line: n, ch: 0}, {className: "code-error"})` per line; no argument clears.
- `updateTabIcons(key, errored, validated, changed)`; a CodeMirror `change` calls it with `(key, false, false, true)` (the grey circle); the editor's three-argument calls clear the circle.
- `changeTab(el)`: switches the shown CodeMirror and calls `opener.KE.updateTab(id)`.

**Syntax highlighting.** `CodeMirror.defineSimpleMode("krunkscript", ...)`, rules in order (start state):
`comment /\s*#[^\n\r]*/`; `keyword /\b(public|static|action|break|continue|return|if|else|for|in|while|addTo|remove|lengthOf|notEmpty|hasProp|toStr|toNum)\b/`; `[def, null] /\b([_a-zA-Z][_a-zA-Z0-9]*)(\()/`; `type /\b(bool|num|str|obj)\b/`; `atom /\b(GAME|true|false)\b/`; `variable /\b[_a-zA-Z][_a-zA-Z0-9]*\b/`; `number /\d+\.?\d*/`; `string /("[^"\n\r]*")|('[^'\n\r]*')/`; `/[\{\[\(]/` indent; `/[\}\]\)]/` dedent; meta `lineComment "#"`, `fold "brace"`. Palenight colours: comment `#676e95`, keyword `#c792ea`, def `#82aaff`, type `#decb6b`, atom `#f78c6c`, variable `#f07178`, number `#ff5370`, string `#c3e88d`, the rest `#fff`; gutter `#676e95` on `#292d3e`, cursor `#fc0`.

**Autocomplete** (show-hint, `completeSingle: false`, on every `cursorActivity` while no hint is active, and `Ctrl-Space`):
- The word is the `[\w.]+` run before the cursor. Nothing when `"word"` or `'word'` occurs in the line.
- The list: the 211 `{text, type}` entries (KSS:6-639) whose text starts with the word (case-insensitive), or that do once an alias (`UTILS → GAME.UTILS`, `Math → GAME.UTILS.math`, KSS:640) stands for its expansion; at most 500, each rendered `text <a>type</a>` (`a` floated right). Picking inserts `text` over the word.
- v7 bug: the alias rewrite is applied to every listed entry and kept in the shared table.

**Templates.** `templates.client` and `templates.server` (KSS:3-4) are the commented skeletons of every `public action` hook:
- Client: start, update, render, onPlayerSpawn, onPlayerDeath, onPlayerUpdate, onKeyPress/Up/Held, onControllerPress/Up/Held, onMouseClick/Up/Scroll, onDIVClicked, onNetworkMessage.
- Server: start, update, onPlayerSpawn, onPlayerDeath, onPlayerDamage, onPlayerUpdate, onCustomTrigger, onNetworkMessage, onChatMessage, onPlayerLeave, onGameEnd, onAdFinished, onServerClosed.

**Validate and save** (`updateScript`, E:2217-2263; reply `io_ksC`, E:1963-1991):
- The status button reads "Saving..." or "Compiling...".
- The current code is always stored in `KE.scripts[key].code`. A "Script Saved" notice shows in `#chatList` for 5 s.
- Unless saving only:
  - Sends socket `ksC(key, code)`. Possible errors: "Validation Pending Please Wait", "Please Reconnect to Validate Script".
  - On error: `alert(err)`. Line numbers are taken from every `/line (\d+)/` match and passed to `highlightErrors(lines)`. The button shows "Failed - errors found!" on `#f44336`.
  - On success: "Success!" on `#4caf50`, and the highlights are cleared.
  - Tab icons are updated (errored / validated). After 1 s the button resets to "Validate Script".
- **There is no console panel and no gutter error list**: errors appear only as an alert plus line highlights plus tab icons.
- Editing the client script invalidates `validatedTest`.

**Storage.**
- Kept in map JSON as `"scripts": {"client": b64, "server": b64}`. Each is UTF-8 base64 and omitted when equal to the template (E:3519-3531, 3571).
- Import: values that pass `b64Test` are decoded as **ascii**, which is lossy for non-ASCII. Raw (non-base64) code is accepted as is.
- No size limit in the editor; the only cap is the 500k map-data publish limit.

**Testing** (`testMap`, E:2183-2215):
- **Test:** the server script is dropped. If a client script exists and the editor is connected and has not validated yet, it shows "Please Wait...", sends `ksT(code)`, and the reply either alerts the error and aborts or substitutes the compiled code (cached in `validatedTest`). Offline, the raw script is passed through.
- **Host:** the full export, server script included, goes through `custToLoad`.
- There is no in-editor run, stop, console or hot reload. Script logs appear in the game client.
- The local server stubs `ksC` as always-success and `ksT` as echo (SRV:3774-3779).

**Script templates window** (ST, 18 entries): Simple .io Game, 2D Car Game (empty), Moving Objects, Bouncing Ball, Popup Window, Flashlight, 3D Pong (empty), Basic Collisions (empty), Snake Demo, Basic Networking, Networking Objects, Converting 3D to 2D, Jetpacks, Sprinting, Triggers & Scripts, Game of Life, Custom Geometry, Attaching.
- Each entry is a full map JSON. `loadQuick` imports it; empty entries are skipped.
- `?scriptT=<i>` auto-loads entry i.

### 1.9 Map settings

**Left panel "Essential"** (UI:1225-1245):

| Field | Key | Default / range |
|---|---|---|
| Name | `name` | "New Krunker Map", ≤16 chars |
| Welcome Msg | `welMsg` | ≤70 chars |
| Mod URL + Select Mod | `modURL` | |
| Death Height | `dthY` | -100; -1000..3000 |
| **Force Old AO** | `oldAO` | |
| **Minimap AssetId** | `mmId` | |

Folders below these fields:

| Folder | Contents | Exported as |
|---|---|---|
| **Game Modes** | Switches for modes without `cost` or `preventCustomGames` | `config.modes`; dropped if `[0]` |
| **Server Preset** | `config.serverConfig` (section below) | `config.settings`, diffed against `def` |
| **Weapon Config** | 48 fields per weapon | `config.weps[i]` |
| **Classes** | Switches | `config.classes`; dropped if all are enabled |
| **Class Config** (new) | 79 fields per class, grouped Camera, Player Asset, Movement, Vehicle Config, Health, Hitbox, Jumping, Shooting, Collision, Weapons, Crosshair. Buttons: "Copy to All Classes", "Reset", per-group "Copy to All" | `config.cls[i]` |
| **Effect Config** (new) | `name`, `max` 1..10, `timer` 0..120, `mlt` 0..10, `hp` 0..100 per effect | `config.prks[i]` |

- Values equal to the stock data are not exported.
- Class Config replaces v3's top-level `game` block. A legacy `game` object found on import is copied into every class.

**Server Preset** (CFG:353-910; validated by CGC:28-104). Widgets: `bool` → switch, `input` → text, `select` → select, otherwise a slider. Rows marked `hideE` are not shown. Fields by group:

| Group | Fields (default) |
|---|---|
| Server Size | maxPlayers 2, minPlayers 0 |
| Timers | gameTime 4, warmupTime, **gamRounds 1**, **intermTmr 30**, forceSpawn |
| Health & Lives | lives, healthMlt, hpRegen |
| Score & Rewards | scoreLimit, **keepTScore**, killRewards, **noAssists** |
| Mode Settings | **objtvTime 1, forceC true, logTim true, lstChkT** |
| World Physics | gravMlt 1 (0..2 /0.001), deltaMlt, **fallDmg, fallDmgThr**, bDrop |
| Jumping | jumpMlt, wallJP, autoJump |
| Movement | **fixMov, slidSpd**, slidTime, strafeSpd, canSlide, airStrf |
| Gameplay Logic | impulseMlt, fiRat, reSpd, headshotOnly, noSecondary, throwMel, **chrgWeps** |
| Collisions | **hitBoxPad 0.6** |
| Other | noStreaks, disableB, allowSpect, thirdPerson, nameTags, **nameTagsFR, kCams, aAnon, headClipFix, minimaps** |
| Teams | selTeam, **frFire, nameTeam1..5, t1..t5Dmg** |

**Environment panel** (UI:5832-5969; the same fields are stored per ENVIRONMENT_ZONE):

| Group | Fields |
|---|---|
| Sky | `skyDome` (Solid / Gradient), `sky`, `skyDomeCol0..2`, `skyDomeTex`, `skyDomeEmis`, **`skyDomeEmisTex`**, `skyDomeMovT`, `skyDomeMovD`, **`skyDomeTexA`** |
| Ambient | `ambInd`: 14 soundscapes with **sparse ids** (`config.soundScapes`, CFG:2404): Default 1, City 2, Desert 3, Market 4, Scary 5, Storm 6, Purgatory 7, Black Market 8, Tomb 11, **Adventure 15, Sea 16, Facility 17, Spooky 18, Festive 19** (new); default 1; **`ambIndC`** custom sound id; `ambientI`; `ambient` |
| Light | `light`, `lightD` (10..5000), `lightI`, `sunAngX` / `sunAngY` |
| Shadows | `shadowR`, `shadowD` (128..4096) |
| Fog | `fog`, `fogD` (10..15000) |
| **Render** | `toneMapping` (None / Linear / Reinhard / Cineon / ACESFilmic), `toneMappingExposure` 0..10, `outputEncoding`, `correctLights` (`useLegacyLights = !correctLights`) |

- Export drops default-valued keys. `oc*`, `zone*`, `skyDome*`, `terrain*` and `sizeMlt` are written only when their feature is on (E:3519-3570).
- Lighting application: render.js:2740-2775. The shadow camera extent comes from map fields `shadWidth` and `shadLength`.

**Custom GUI component** (new; Components ▸ GUI, UI:6288-6560; EO):
- Controls: "Show GUI" (preview in the editor overlay), [Add Element], and per element Remove and Duplicate.
- Element fields:

| Key | Field | Notes / range |
|---|---|---|
| `lbl` | label | |
| `t` | type | Box / Text / Image / Div |
| `vt` | value type | Input / Player / Global |
| `v` | value | |
| `f` | font size | 1..100 |
| `s` | image asset | with Select Image |
| `c` | colour | |
| `o` | opacity | |
| `x`, `y` | position | % 0..100 |
| `w`, `h` | size | 0..2000 |
| `z` | order | |
| `nv` | hidden | |
| `id` | element id | 0..2000 |

- Defaults: `{t:0, s:"", v:"", vt:0, w:100, h:50, x:50, y:50, f:10, c:"#ff0000", o:1, z:0, id:0, nv:0, lbl:""}` (E:6357).
- `importGUI` (E:6396) clamps every field and sanitises strings.
- Limit 80 elements. Exported as `gui`.

**Map validation and limits** (CGC:152-333; CFG:916-936):

| Limit | Value |
|---|---|
| Objects | 5000 (7500 verified/premium) |
| Collidable objects | 4500 (6000) |
| Complex-collision triangles | < 5000 |
| Spawns | 1..20 |
| Signs | ≤100; text ≤1500; font 1..240 |
| Wheels | ≤10 |
| Leaderboards | ≤2 |
| AI | ≤40 |
| Bots | ≤16 |
| Trigger actions | 5 |
| Objects per interface id | 30 |
| GUI elements | 80 |
| Game data | 500k chars |

**Map-level physics and game config** lives entirely in `config.settings` (gravMlt, jumpMlt, deltaMlt, ...) and `config.cls` (per-class movement). There are no top-level gravity or speed keys.

### 1.10 Asset browser, import/export, saving and publishing, test-play

**Asset browser** (windows 2 Assets, 7 Textures, 9 Sounds; card builder `f954` at E:232):
- Data comes from REST `assets/list?index&cat&username` and `assets/search?index=2&cat&val` (`src/libs/apiClient.js`).
- `config.assetCat` has 19 categories, including 16 Textures, 17 Sounds and Triggers.
- Each asset's kind comes from flags in its `dat`:

| Kind | Flag in `dat` |
|---|---|
| 0 model | (none) |
| 3 prefab | `d` |
| 4 texture | `x` |
| 5 sound | `u` |
| video | `v` |

- `onlyShow` filters the list: Custom shows `[0,3]`; model pickers show `[0]`.
- Thumbnails come from `user-assets/<id>/thumb.png` or `texture.png`.
- Sounds have a play button (`testSound`).
- Right-click copies the asset id. Owners also get a delete button that sends `delC`.

**Map export** (`getMapExport(raw)`, E:3363-3597):
```
{ <mapConfig keys != default>, colors?, xyz?, objects, config?, gui?, scripts?,
  camPos?, spawns, botPaths?, tris? }
```
- **Colour dedup:** colours become `ci`/`ei` indices into `colors[]`.
- **Size dedup:** sizes become `si` indices into a flat triplet array `xyz[]` (new in v7).
- **Complex collision:** objects with complex collision append world-space triangles to `tris[]`.
- **Extracted objects:** SPAWN_POINT, CAMERA_POSITION and BOT_PATH_NODE are pulled out of `objects` into their own keys.
- **`config`:** `{modes, classes, settings, weps, cls, prks}`, each delta-encoded against its defaults.
- **Text fields:** `name` and `welMsg` go through `sanitizeHTML`.
- **Raw mode** keeps everything inline (used for saves).

**Save project** (`save()`, E:4271):
- Shape: `{map: rawExport, groups:{uuid:{ids}}, selected, cam:{p, r:[pitch,yaw]}, history:[]}`.
- Written to `<name>_save.txt`. Triggered by Ctrl+S, which works even while typing, or by File ▸ Save Project.

**Import** (`importMap`, E:3904-4253):
- **Accepted inputs:** a raw map, or a wrapper with `states`, `id` or `map` (the `.map` member is used).
- **Restored from saves:** `cam`, `selected`, `groups`.
- **Pool expansion:** `ci`, `ei` and `si` are expanded back into per-object values.
- **Extracted keys:** `camPos`, `spawns` and `botPaths` become objects again.
- **Server settings:** `config.settings` strings that fail `/[^\w \-\|]/` fall back to their defaults.
- **Legacy keys:** `cls.asset` becomes `aID`; a legacy `game` block is copied into every class.
- **Scripts:** `scripts` values are decoded from base64.
- **Remaining keys** are validated before use:
  - `name` and `welMsg` against the regex `[^\w:;()?,.|[\] \-+*$@=^!{}]`
  - `modURL` must parse as a URL
  - `mmId` must match `[\dtlnesacdgxufq]`
  - terrain seeds must match `\w_-.`
  - numbers and booleans are coerced
- **Timelapse:** an optional mode adds objects with a per-object delay and can make the camera follow each one.
- **Errors** show "Failed to import map with error:".

**Entry points:**
- Import popup.
- File picker (Shift+Import).
- Drag-and-drop.
- `localStorage.mapToEdit` (`checkEdit`).
- URL parameters `?scriptT=` / `?gameT=`.
- Templates.
- Default maps (synchronous XHR to `maps/<file>.json`).
- My Games ▸ Load in Editor (socket `edMp` → `editMap`).
- Backups.
- Legacy importer `importOldMap` (dev only).

**Other exports:**
- **GLTF** (E:3608). Uses GLTFExporter over all non-tool objects:
  - borders are turned off for the export
  - materials are Lambert copies
  - output is `.gltf`, or `.glb` when the result is an ArrayBuffer
- **MTL** (E:3656). Writes OBJ + MTL + PNG textures into a JSZip archive, `<name>.zip`. Alerts "Big maps can take a while" first.
- **Prefab export** (section 1.3).
- **Converter outputs:** `convertedImage.txt`, `convertedVoxels.txt`, `reflect.txt`.

**Backups** (new):
- Stored in IndexedDB: database "Editor", store "MapBackups", keyPath `timestamp`.
- `doBackup` writes `save(true)` every `backupTimer` minutes when `autoBackups` is on. Defaults are on, 10 minutes (range 1..30).
- A backup is also written on unload.
- There is no pruning. Window 15 lists the backups, loads one, or clears them all.
- (v3.1.1 instead downloaded export files.)

**localStorage keys:**

| Key | Contents |
|---|---|
| `kro_editor` | settings |
| `custMap` | test map |
| `custToLoad` | host map |
| `mapToPub` | publish handoff |
| `mapToEdit` | game → editor handoff |
| `krunker_username` / `krunker_id` / `krunker_token` | account |
| `testCode` | `tc` query parameter on the socket |

**Account and socket.**
- The socket connects to `config.editorURL + "/ws"`. On the local server that is `/editor/ws` (SRV).
- Messages in both directions:

| Message | Purpose |
|---|---|
| `a` | login: `[user, pass, token]`; autologin with the token when `autoLog` is on |
| `pi`/`po` | ping |
| `2fa`/`c2fa` | 2FA |
| `ksC` | validate a script |
| `ksT` | compile the test script |
| `plm` | preload publish |
| `uploadM`/`pubMR` | upload a map |
| `uploadA`/`pubR` | upload an asset |
| `edMp`/`mR` | edit an owned map |
| `ulb`/`upMp` | leaderboard settings |
| `mfnds`/`mfr` | map funds |
| `delC` | delete a creation |
| `dbE` | (also handled; purpose not surveyed) |
| `pir` | (also handled; purpose not surveyed) |

- On disconnect, the warning "Error: DISCONNECTED" appears and the client reconnects after 30 s.

**Publish game:**
1. Login is required.
2. The client sends `plm` and gets back the publish popup.
3. The popup has these fields:
   - thumbnail PNG, up to 250 KB (500 KB for premium)
   - category: one of 24 `mapCats`
   - Post to Hub
   - Mobile Friendly
   - data
   - YouTube link
   - description, up to 500 chars
4. The button reads "Publish", or "Publish v<n>" for an update.
5. `uploadM`: map data is capped at 500,000 chars.
6. `pubMR` → `POST /upload-user-asset` uploads the thumbnail.
7. The result popup has a share link that copies `/?play=<name>`.

v3.1.1 instead redirected to `social.html`.

**Publish asset:**
- Types: Model, Object Prefab, Texture/Image, Sound, Video.
- Each upload has a category and a private flag.
- Size limits:

| Upload | Limit |
|---|---|
| model file (`.obj` `.gltf` `.glb` `.fbx`) | ≤100 MB total (250 MB premium) |
| each of 10 PBR texture maps | ≤10 MB |
| icon | ≤100 KB |
| texture | ≤10 MB |
| sound (mp3) | ≤5 MB |
| video (mp4) | ≤20 MB |
| prefab data | ≤500k chars |

- Sent as `uploadA` followed by `POST /upload-user-asset`.

**My Games** (window 14):
- **Edit** → `editPop` with these options:
  - Load in Editor
  - leaderboard settings: type Score/Time, custom text, order
  - funds deposit/withdraw, 10% fee
- **Delete** → confirm.

**Test-play** (`testMap(host, atCamera)`, E:2183-2215):
- **Test** (▶): runs the script validation/compile from section 1.8, stores `custMap`, then opens `/` in a new tab, or in an iframe popup when `testInEditor` is on. The game runs `loadSandbox` on it.
- **Right-click ▶:** adds `player:{pos, rot:[pitch,yaw]}` taken from the editor camera.
- **Host:** stores `custToLoad` and opens `/`, which calls `selectHostMap`.
- **Teleport popup** (E:6159-6178):
  - X, Y and Z inputs.
  - Teleport tweens the camera there over 900 ms, linear.
  - Reset jumps back to (0, 50, 100).

### 1.11 Camera controls, keybinds, editor settings

**Camera:**
- PerspectiveCamera with FOV 70, near 1, far 10000. `renderDist` (20000) is applied only after it is changed in settings.
- Starts at (0, 50, 100).
- PointerLockControls:
  - sensitivity 0.002 rad/px
  - pitch clamped to ±90°
  - mouse-look while the **right mouse button** is held, with pointer lock when the `pointerLock` setting is on (default)
- Fly movement is camera-relative and includes pitch. The movement vector is not normalised.
  - speed `speedNormal` 70 units/s
  - `speedSprint` 180 units/s while Shift is held
- Mouse wheel dollies along the view direction by `70 × dt × 5`. The 70 is captured once at startup, so sprint and later speed changes don't affect the wheel.
- Double-click focuses the clicked object. Scene Graph has a Focus button. The teleport popup moves the camera.
- There is no orthographic view and no camera bookmarks. Saves store the camera.

**Keybinds:**
- Handled at E:2333-2395 and E:2642-2773.
- Inactive while typing in an input and while a window is open. Ctrl+S and Esc are the exceptions.
- Rebindable keys live in `kro_editor` and are compared with `charCodeAt(0)`.

| Key | Action | | Key | Action |
|---|---|---|---|---|
| W/S/A/D, arrows | fly | | Del / Backspace | delete (group-aware) |
| E / Q (not with Ctrl) | up / down | | Shift+R | duplicate |
| Shift (hold) | sprint | | Ctrl+C | copy (also creates a cube, quirk) |
| RMB (hold) | look | | Shift+C | toggle collidable |
| Wheel | dolly | | Ctrl+V | paste (auto-group) |
| LMB | select / face / group-pick | | Shift+V | toggle visible |
| dbl-LMB | focus | | F / Shift+F | flip X/Z / hitbox fixer |
| Alt+LMB | select through invisible | | Shift+G | group create/stop |
| Shift+LMB | object painter | | Shift+Alt+G | stop all groups |
| MMB (or RMB) | Quick GUI | | Ctrl+Z | undo |
| M | face selection | | Ctrl+Y, Ctrl+Shift+Z | redo |
| N | group selection | | `+` / `-` (and numpad) | gizmo size ±0.1 |
| `,` | **wireframe** | | Ctrl+S | save project |
| 1 / 2 / 3 | translate / rotate / scale | | Esc | close window |
| `` ` `` | world/local | | **0** | **scripting window** |
| P | placeholder | | **T** | **texture/variant/spray/skin list** |
| X | Quick Add | | **C / G / R** | **create cube / gate / trigger** |

**KrunkNative: E turns the selection.** With an object selected, **E** starts a turn about Y: the
selected object (or group placeholder, with its members) turns about its own centre as the cursor
moves horizontally, 0.5 degrees per CSS px, rounded to `rotationSnapping` while snapping is on.
E or a click confirms, Esc cancels; the turn is one undo step (`editor_render.c` `ed_rotate_*`).
Differences: v7.1.4 has no E turn (its rotate mode is the gizmo on key 2) and E keeps its `upKey`
fly-up while held; v10 has no editor in its client bundle.

**Editor settings** (`initSettings`, E:1779-1883). Stored in `kro_editor`; "Reset All" restores them.

| Area | Setting | Default / range |
|---|---|---|
| General | autoBackups | on |
| | backupTimer | 10 (1..30) |
| | uiScale | 1 (0.5..1.5) |
| | pointerLock | on |
| | disableWarnings | off |
| | **testInEditor** | off |
| | **autoLog** | on |
| | resMltV | 0.6 (0.1..2) |
| | antiAlias | off; the switch is mis-bound in the reference |
| | **renderDist** | 20000 (100..20000) |
| | disableHistory | off |
| | historyLimit | 1000 |
| | showRealHitbox | off |
| | **scriptFont** | 13 (5..30) |
| Display | **showFPS** | off |
| | darkMode | on |
| | grid visibility / opacity / size / spacing | on / 0.25 / 250 / 10 |
| | dbVisibility / dbOpacity | off / 0.5 |
| Object | triggerCons | on |
| | selectThroughInvis | on |
| | borderVisuals | on |
| | objectHighlight | off |
| | createObjectNear | off |
| | degToRad | off |
| | preserveFolderState | on |
| | translation / rotation / scale snapping | 1 / 1 / 1 |
| | light-cone and model bounding-box toggles | |
| Quick GUI | quickGUI | on |
| | quickGUIOpen | 0 middle, 1 right |
| Hidden | assetAutoGroup | on |
| | mergeVoxels | off |
| | voxelSize | 10 |
| | imageSize / imageDepth | 1 / 1 |
| | imageCol | on |
| | speedNormal / speedSprint | 70 / 180 |
| | quickAddButtons | 8 slots |
| | key binds | |

### 1.12 Performance tools and stats

**Stats:**
- `#canvasInfo` counters (section 1.1).
- Optional FPS counter (`showFPS`). EditorOverlay draws the frame count over the last 1000 ms at (18, h-5) in green, 13px GameFont, with a stroke.
- There is no draw-call or triangle counter.

**Live limit warnings** (`warningCheck`, E:3003-3096):
- Shown in `#chatList` with a red icon. They are advisory only; nothing is blocked.
- Limits:

| What | Limit (verified/premium) |
|---|---|
| collidable objects | 4500 (6000) |
| objects | 5000 (7500) |
| spawns | 20 |
| event objects | 10 |
| GUI elements | 80 |
| AI | 40 |
| bots | 16 |
| signs | 100 |
| leaderboards | 2 |
| prize wheels | 10 |
| objects per interface id | 30 |

- The same channel shows notices: "Script Saved", "Light Cones are hidden", "Models & Assets are hidden", "Error: DISCONNECTED".

**Render settings:**
- Resolution multiplier `resMltV` (pixel ratio = DPR × 0.6).
- `antiAlias`.
- `renderDist`.
- Lighting/model visibility toggles, wireframe.

**Tools panel** (Game ▸ Tools, UI:6087-6200). This replaces v3.1.1's separate "Customize" and "Tools" panels.

| Tool | Behaviour |
|---|---|
| **Optimize Faces** (new, E:6280-6356) | For axis-aligned, visible CUBE pairs, hides any face fully covered by an opaque neighbour (FLAG and LINK textures excluded). Alerts "N% Face Reduction". |
| **Color Merging** (new, E:6212-6270) | Range 500..50000 (default 1500). Snaps each colour/emissive to the first palette colour whose integer hex is within range. Alerts "N Colors Merged". The reference reuses the loop variable (bug). |
| Text Generator (E:5379) | Block font; space advances 5; `.` starts a new line. Placed as a group at the selection. |
| Image Converter (E:5277) | Pixel → cube. Settings: size, depth, collidable. Convert (download) or Import (replace selection, grouped). |
| Voxel Manager (E:5247) | `{voxels:[{x,y,z}]}` × size, with optional merge. |
| Layout Planner (E:5200) | Grid of `#460050` cubes. Settings: size, count and spacing per axis. |
| Timelapse (E:5173) | 100..5000 ms per object, optional follow. |
| Transform | Scale map XYZ; Reflect X/Y/Z. |
| Frame (E:5892) | Walls of a thickness (≥1, default 10) around the selection, with ceiling and floor. The reference binds the Ceiling switch to the thickness (bug). |
| Breakable (E:5180) | Health 1..500 on all objects; optionally forces collision. |
| Colorize | Random / Gold (no-op) / Input list. |
| Drunk Map (E:5192) | Random `movingSpeed` ±20 and axis. |

**Prefabs panel** (UI:6202-6257):
- Auto Group.
- Rotation (0..359).
- Import JSON/File.
- Object Painter, using JSON or one of 8 scrap presets.

### 1.13 Reference quirks: decisions for the port

The default is to match v7.1.4. The rows below break that default. Where Krunker Box Office fixes something, a code comment must say "intentional difference from v7.1.4".

| Quirk | Decision |
|---|---|
| Snapping label reads OFF at boot | **fix**: label reflects the real state |
| Ctrl+C also creates a cube | **fix**: C/G/R ignore Ctrl and Shift |
| History over-limit drops the newest entry; redo of an update re-applies the old state | **fix both**: drop the oldest; redo re-applies the newer state. Redo stays one-way, the same as v7. Done (WP1, `editor_objects.c`). |
| `duplicateGroup` ReferenceError | fix (no observable behaviour) |
| Wheel dolly speed frozen at 70 | keep (feel parity) |
| "Toggle snapping: Control" not implemented | keep KN's Ctrl-hold disables snapping (already documented as a KN addition) |
| Mis-bound switches (Anti-aliasing → `resMltV`, Frame Ceiling → thickness, Quick GUI Fix Hitbox `this`) | **fix** (Frame: "Has Ceiling" edits `frameCeiling`, so the Tools dump shows it off where v7 shows the thickness as on; Fix Hitbox: `editor_quickgui.c`) |
| Color Merging loop-variable bug; Colorize Gold no-op | **fix** Color Merging: every object is visited once (`editor_tools.c`; `tools_check.py` models v7's loop, which reproduces v7's capture, and checks the native fix). Gold: the v7 button stays (the Tools dump stays v7's) and colours every object `#FFD700` with emissive `#4D4100` (WP6) |
| `la` omitted at 15 (default 45); particle defaults 1 vs 0 | **match on write** so exports are byte-compatible with v7; read with v7's load defaults |
| `sci` written as `null` | **fix**: write `sci` from `scriptId` (`map-logic.md` decides runtime use) |
| Bot `speedMlt` never written | keep (BOT_SPAWNER is `localOnly`) |
| `importMapBackup` reads global `event` | n/a in C |
| Script import decodes base64 as ASCII | **fix**: decode as UTF-8 |
| `camera.far` 10000 until `renderDist` is changed | **fix**: apply `renderDist` at start |
| Object keys ObjectInstance never reads (v3.1.1 `dyn` / `gy` / `sad`, anything unknown) are dropped by `serialize()` | **keep them** after the v7 keys, so maps round-trip without loss (`editor_model.c`) |
| An invalid texture name or index throws ("Invalid texture id."), an invalid prefab id aborts the whole import ("Invalid Object") | **fix**: the texture loads as DEFAULT; an object with an invalid prefab id is skipped on import (a cube when pasted) |
| Non-scalable model prefabs take their size from the loaded model's bounds | **keep the imported `s`** (the bounds v7 exported); a new object uses the generator's OBJ bounds |
| ENVIRONMENT_ZONE `skyDomeEmis` is tested on the output object, so it is always written `#000000` | **fix**: the zone's own colour is written |
| Select-type server settings (`tmSize`) export `parseInt(<config>[varN])`, i.e. `null` | **fix**: the option index is written |
| An import keeps the old `colors` / `xyz` (and empty `game` / `gui`) as mapConfig keys, so a stale palette is re-exported | **fix**: dropped on import |
| `botPaths` import as CUBE objects (no BOT_PATH_NODE prefab; `getPrefabIdByName` returns 0) | **drop** them (never produced by v7) |
| A legacy trigger's object-level `ta` 14/34/54 writes the sound keys next to `tas` once (the next export drops them) | keep (v7's export; the round-trip test checks the second export as the fixed point) |
| `camPos` / `spawns` export the exact transform, objects the rounded one | keep: the editor holds exact transforms (`ED_MODEL_EXACT_TRANSFORM`), the export rounds objects |
| The left panel's v3 "Game" folder (no v7 equivalent until WP4's Class Config) | **removed**: v7's Class Config edits each class (`editor_gameconfig.c`); a legacy `game` block is still copied into every class on import |
| Teleporter connection lines take the hue of the node 0 end's `node` (always red) | **fix**: hue `channel × 3.6` (`editor_render.c`) |
| A folder header finds its body with `getElementById` in KN's first port | v7 looks inside the header's parent (`querySelector("[id='xB']")`); KN does the same, so Class Config's repeated Camera / Movement / ... folders open their own body (`editor_gui.c`) |
| "Element Label" has two `onChange` handlers; only the delayed refresh survives | keep: the label is stored as typed and sanitised by the export |
| Pending Select Asset / Icon / Image / Custom targets | dropped when another window opens (v7 keeps the last viewModels / KE.activeAction argument) |
| Group members follow the owner's scale (Krunker Box Office) | **removed**: translation and Y rotation only, as v7 (the scale branch is disabled) |
| `config.isProd` (the counters' Event row, `localOnly` prefabs in the menus) | the page is production, as krunker.io: Event Objects stay hidden and Event / Terminal / Bot Spawner are not offered (the v7 reference capture is production too) |
| Rebinding a key stores `e.key` (`","`), which never equals the `keyCode` (188) it is compared with | **fix**: binds match by key code and by character (`editor.c:key_is`) |
| Templates window: the Load tile quotes `'importMap'` inside a single-quoted attribute (a syntax error) | **fix** (`editor_panels.c`) |
| Tutorials / Tips & Tricks are YouTube iframes | link cards to the videos (no iframes in the engine; open question 3) |
| Backups in IndexedDB, never pruned | `html_storage` (`knEditorBackups` + one key per backup), the newest 20 kept (`editor_backup.c`) |
| Account ▸ Login opens `loginPopup` (socket `a`) | opens the hub, which owns login (open question 1); the editor shows the hub session's name |
| Wireframe (`forceWire`) | desktop GL only: OpenGL ES (web, ANGLE) has no `glPolygonMode` and draws filled (`scene.c`) |
| Drag & drop import | native only (GLFW drop); the web build has no drop glue yet |
| File menu | gains Krunker Box Office's **Projects** submenu (several maps, detached views, the editor bus) |
| The Trigger Event folder never renders in v7's page as served (the hook script's `Object.prototype.events` accessor swallows `vO234.events`) | **fix**: shown (WP4's `ed_triggers_panel`). `v7ref.js` captures `prefabs` and `logic` without `public/js/hook.js`, so the v7 dumps of prefabs 29 / 55 show it too (`doc/ref/.../editor/README.md`) |
| AI_SPAWNER "Offset Shot" binds the misspelt `objConfig.shoOffH`: always shows off, never edits `shotOffH` | **fix**: the switch shows and edits the object's `shof` (`editor_inspector.c`) |
| ENVIRONMENT_ZONE "Emissive" has no stringified getter, so the input shows its `#ffffff` default | **fix**: shows the zone's `skyDomeEmis` |
| T-key spray list: `selectSpray(i)` stores the store position, not the spray index the Spray select and `spt` use | **fix**: the item passes the spray's index |
| Ocean (`Ocean.js` / Water.js) in the editor view | the editor previews a flat plane (`ocCol` at `ocOpac`, `ocW` x `ocL` at `ocYOff`); the reflective water is the game's (`client/src/ocean.c`) |
| Asset browsers v7 adds for the inspector (`selectShootHit`, `selectMelee(Hit)`, `selectProjModel`, `selectCloud(EN)`, `selectEmissiveCloud(EN)`, `selectAmb(EN)`) | done (WP6): `ed_net_select` opens the Assets / Textures / Sounds window with v7's `from` and `KE.selectAsset` applies the id (`editor_net.c`) |
| The API windows (2 / 5 / 7 / 9 / 14) without an answering API show "Loading..." forever | the list shows its empty text ("No Assets found", ..., "No Games found!") when there is no brain (`windows_check.py`) |
| `config.modTabs` / `mapTabs` / `assetTabs` indices | mapped onto kn_brain's `/api` index space (0 Popular, 1 Featured, 2 New, 3 Favorites, 4 My Games / My Mods), as the hub does; `username` goes only with My Assets (kn_brain filters by it whenever it is given) |
| Card thumbnails, prefab `data.json`, mod zips on user-assets.krunker.io | the asset tree's `user-assets/<id>/` (native: the local file or `noimg.png`; web: the asset host); a map card shows the `map_image` kn_brain stored |
| `publishMap` sends `plm` | kn_brain has no `plm`: the editor looks the name up in the account's My Games (`/api/maps` index 4) for "Update Game" / "Publish v<n>" |
| `uploadM` / `uploadA` post the files to `/upload-user-asset` (FormData) | `uploadM` carries the thumbnail inline (kn_brain's v3.1.1 shape + v7's Post to Hub / Mobile Friendly flags, which kn_brain ignores); the thumbnail defaults to the 3D view (Select picks a PNG), at most 200 kB of data URL; `uploadA` is v7's frame and kn_brain answers "File uploads are not supported on this server" (no asset hosting); a `pubR` reply ends in "Upload Request Failed" (no multipart POST in the client) |
| `mfnds` waits for a reply forever when the server never answers | "No reply from the server" after 8 s (kn_brain has neither yet) |
| `ulb` (leaderboard) | v10's behaviour: no reply timeout, the "Please wait..." popup stays until closed; its `upMp` reply goes to `#gameUpResp` (absent in the leaderboard popup), so nothing shows. kn_brain stores the settings on the map's LEADERBOARD objects (`ldt` / `ldc` / `ldo`, `creations.c` `sh_update_map_lb`) and answers `upMp` "Leaderboard Updated!" |
| `editMap(null)` (no such map) does nothing | "Map not found" popup |
| The Sounds window writes its empty / error text into `#assetTList` (the Textures list) | its own `#assetSList` |
| `mapResp` share link / update-video URL come from an archive.org capture (`https://web.archive.org/web/.../https://youtube.com/...`) | the plain URLs |
| Prefabs panel: the Object Painter div is built shown when `objectPainterType` is set (`addDiv(.., objectPainterType)`), so a rebuilt panel hides the JSON field of an active painter | **fix**: the div follows the Object Painter switch (`editor_tools.c`) |
| Optimize Faces / Color Merging change objects outside the history poll's one-second steps | each is one undo step (Krunker Box Office) |
| Export GLTF / Export MTL build from three's meshes (Lambert copies of what the renderer made) | the meshes are built from the object data, GL-free (`editor_export.c`): the shapes of `prefab.c` and the prefab OBJ models; asset models, ladders, sprites, lights, AI, canvases and random billboards are left out; the material colour is the object's `c` (no vertex colours); one node per object |
| OBJMTLExporter cuts the last character off the texture name and keeps the slash (`map_Kd /wall_0.pn`, zip entry `wall_0.pn`; every asset texture is `texture.pn`) | **fix**: `map_Kd wall_0.png`, asset textures `<id>_texture.png` |
| GLTFExporter re-encodes each texture upside down (flipY) | the PNG is embedded as is and `TEXCOORD_0` is flipped (same result) |
| KrunkScript window opens `scripting.html` as a second browser window | an **in-page window** (`#knScriptWin`) at the same place (10% / 10%, 80% x 80%) with a close button; CodeMirror is the engine's code view (`html_dom_code_view`), folding and the find dialog are not ported (the fold gutter shows its markers) (`editor_script.c`) |
| Validation is the `ksC` / `ksT` socket round trip (offline: "Please Reconnect to Validate Script") | **local**: the C checker (`ks_validate`, as ksc), immediate; diagnostics also run live (300 ms after an edit) and mark the error line; Krunker Box Office lists them under the code (`#knScriptErrors`, v7 used `alert()`); the button and tab icons still change on Validate only |
| The code reaches `KE.scripts` on Validate, tab switch and window close only | it is kept in the project as typed (300 ms), so exports and saves never miss edits; "Script Saved" still shows on those three |
| show-hint's alias rewrite touches every listed entry and stays in the shared table | **fix**: only the entries matched through the alias show it (`editor_script_lang.c`) |
| The `def` rule swallows a call's `(` but the bare `)` rule dedents, so auto-indent drops a level after any line with a call | **fix**: the call's `(` counts as an opening bracket (`editor_script_lang.c`) |
| Test-play `ksT` compiles to JS | the client script is compiled with `ks_compile` (the game runs the source); an error is reported (chat line + the window on the error) and the test does not start |

---

## 2. Differences between the v3.1.1 and v7.1.4 editors

**New in v7.1.4:**

- **Shell:**
  - The menu bar is reorganised:
    - Map → Game; Component → Components; Help gains Guide.
    - New menus: Scripting, Publish, Account, Documentation.
    - File gains Save Project, Load Backup, Export MTL, Copy Export and Import Old Project; v3's File ▸ Publish and Save As are replaced.
    - Object gains Scene Graph and the Lights and Zones submenus, and drops the AI submenu.
    - Components gains Ocean and GUI.
  - Shift+click on a prefab reuses the selected object's pos and size.
  - The playbar gains a Teleport button and right-click "spawn at camera".
  - New canvas toggles: Wireframe and Snapping. Snapping is really toggleable now; in v3 it was always on.
  - `#canvasInfo` gains Event Objects, Server Nodes and Total.
  - 7 new windows: Sounds, Templates, Publishing guide, Tutorials, Tips & Tricks, My Games, Load Backup. The Welcome window is now shown at boot (v3 defined it but never opened it).
  - Login/2FA and the socket now live inside the editor.
- **Placement and transform:**
  - New hotkeys: C/G/R create objects, T opens the texture/variant/spray/skin pickers, `,` toggles wireframe, 0 opens scripting, `+`/`-` resize the gizmo.
  - New gizmo scale modes: modelScale (uniform `mSize`) and directional one-sided scaling with Shift/Ctrl, plus fakeScale mode 2.
- **Data:**
  - Prefabs go from 60 to 97. Id 55 BOT becomes AI_SPAWNER and id 33 SPECTATE_CAM becomes CAMERA.
  - Textures go from 17 to 20, and texture/model variants (`tv`) are new.
  - Many new object properties (section 1.4).
  - Keys v3 showed but never saved (`wj cdy sd sfd cha ct esa`, AI fields) are now serialized.
  - Export gains size pooling (`si`/`xyz`), complex-collision `tris`, 8-tuple spawns, 7-tuple `camPos`, `botPaths`, `gui`, `scripts`, `config.cls` and `config.prks`.
- **Logic:**
  - Events go from 16 to 32 and actions from 32 to 72.
  - New: 12 conditions, up to 5 actions per trigger (`tas`), per-action delay, value types (Input/Trigger/Player/Global) with keys, action cost/method, once-per-player, custom interact message, and sounds on actions 14/34/54.
  - Connection lines now work (`checkConnections`; v3 called it but never defined it).
  - The interface id range grows from 1000 to 2000 and the per-id limit from 20 to 30.
- **Scripting:**
  - The KrunkScript client/server code window, with validate (`ksC`), compile-on-test (`ksT`), error line highlights and tab icons.
  - 18 script templates and the scriptFont setting.
- **Map settings:**
  - New Essential fields: Force Old AO and Minimap AssetId.
  - Class Config (79 fields) replaces the `game` block.
  - Effect Config.
  - About 25 new server-preset fields.
  - Environment gains tone mapping, exposure, output encoding, correctLights, the custom ambient sound id, sky emissive/cloud asset textures and 5 new soundscapes (ids 15-19).
  - Ocean component; zone stages, wait and height.
  - Custom GUI elements with an overlay preview.
- **Assets and publishing:**
  - In-editor publishing of games and assets (v3 used a social.html redirect).
  - Asset kinds gain sound and video.
  - Backups move to IndexedDB (v3 downloaded export files).
  - New in-editor actions: Templates, Load in Editor, leaderboard and funds management, deleting creations, Export MTL.
- **Tools:**
  - New: Optimize Faces, Color Merging and the Object Painter scrap presets.
  - v3's Customize panel is merged into the Tools panel, and Prefabs gets its own panel.
- **Stats:**
  - FPS overlay.
  - New warnings: AI 40, bots 16, signs 100 (was 20), leaderboards, wheels, GUI elements 80, collidable 4500 (was 3500).

**Changed:**
- Quick Start loads game template 0 instead of creating a floor plus a spawn.
- Team selects go from Default + 2 teams to Default + Alpha..Delta (`teamLimit` 5, CFG:351). Flags (`noDefault`) offer Alpha..Foxtrot, and `canNeutral` prefabs add Neutral -1.
- Several ranges widen: health max 20000 → 50000, respawn 300 → 3600 s, boost ±10 → ±100, bounce 0.1..3 → -3..3, light intensity 0..2 → 0..15, sign font 10..120 → 1..240.
- The Light Intensity slider updates the right light; v3 updated the ambient light by mistake.
- `degToRad` displays degrees.
- The World/Local toggle stays enabled in scale mode; v3 refused it (v3E:2797, 2802). v7 `setTransformSpace` (E:4367) only relabels and calls `setSpace`. TransformControls scales in local space either way.

**Removed (v3.1.1 → v7.1.4):**
- Top-level `game` key on export (still imported).
- The v3 Customize panel, the AI submenu, the BOT prefab, the auto-backup-by-download setting (0..25 min), and File ▸ Publish redirect.
- The "Map" menu name.
- Dynamic/gravity controls (`canBeDynamic`, keys `dyn`/`gy`): v7.1.4 has no prefab flag, inspector control or serializer branch for them (checked with grep across PF, OI and UI). Their runtime fate is a `map-logic.md` question.

**Unchanged:**
- Placeholder groups, clipboard model, 1 s history poll, hitbox fixer, face selection, Quick GUI, Quick Add, terrain generator, zone basics, Text/Image/Voxel/Layout tools, grid and death barrier.
- Camera controls and the remaining hotkeys.
- Settings storage `kro_editor`.

---

## 3. Gap analysis against Krunker Box Office's editor

### How the native editor works today

The native editor (`client/src/editor/*`, about 11.3k lines) is a near-complete port of the **v3.1.1** editor.

**Architecture:**
- `client_bin --editor [map=<name>|new]` is one page in `app_mode.c` (`k_pages`, `editor_app_page()` in `editor.c`).
- The UI is the imported `ui/editor.html`, generated by `tools/ui-import/editor_page.py` from the v3.1.1 page and rendered by the in-house HTML engine.
- The panel markup is generated by `editor_gui.c`. It ports `GUI._build`/`action`, a Pickr rebuild and key capture.
- Viewport: `editor_render.c` draws through the game's own meshes (`map_init` per object) plus GL overlays.
- Data tables come from `editor_data.c`, generated by `tools/editor/gen_editor_data.py` from v3.1.1 sources.
- Networking: `editor_net.c` talks to `kn_brain` over HTTP and the social socket.

**Gaps that apply everywhere:**
1. Every table is v3.1.1: 60 prefabs, 17 textures, 16 events, 32 actions, v3 config lists.
2. The serializer `PROPS` table in `editor_objects.c` holds v3 keys only.
3. There is no editor reference capture or uidiff scenario. `KRUNKNATIVE_SCREENSHOT` and `KRUNKNATIVE_INPUT_SCRIPT` don't fire in editor mode (`debug_capture.c:137-147` waits for `game.ready`).

**Status legend:**
- **P** = present, matching v7 (or v3 behaviour that v7 left unchanged).
- **Pa** = partial or v3-shaped.
- **M** = missing.

### 3.1 UI layout, panels, menus

| Feature | St | Owner today | Gap |
|---|---|---|---|
| Page skeleton, hotbar, resizable/lockable panels, restore tabs, UI scale, dark theme | Pa | `ui/editor.html`, `ui/css/editor-kn.css`, `editor.c` (resize_drag, scaleUI), `editor_panels.c` (h_lock) | Re-import v7 `editor.html`/`editor.css`: wireframe/snapping buttons, the extra `#canvasInfo` rows, a 3-button playbar, `#editor-overlay` canvas. Version text "v2.0.0". |
| Toolbar File | Pa | `editor_panels.c:ed_toolbar_show/toolbar_action` | Rename and add Save Project, Load Backup, Export MTL, Copy Export, Import Old Project (dev). Implement Export GLTF (currently "not available"). New Project confirm. |
| Toolbar Edit | P | `editor_panels.c` | – |
| Toolbar Object | Pa | `editor_panels.c` | Scene Graph; Lights/Zones submenus (v7 categorisation; drop the AI submenu); Shift+click `reuseSelected`; Group ▸ Prefab ▸ Tools → Prefabs panel. |
| Components (Environment, Terrain, Zone, Ocean, GUI) | Pa | `editor_panels.c` | Ocean and GUI missing. "Tools" moves to the Game menu; Customize is merged into Tools. |
| Scripting menu | M | – | WP5 |
| Game menu (Test, Host, Quick Start = template 0, Templates, Tools) | Pa | `editor_panels.c`, `editor.c:editor_test_map`, `editor_io.c:ed_quick_start` | Templates window; Quick Start semantics. |
| Publish (Game, Asset, Guide) | Pa | `editor_net.c:ed_net_publish` | Asset publish, publishing guide window, update versioning "Publish v<n>", Post to Hub / Mobile flags, 24 categories (v3 had 12). |
| Account menu, login/2FA in the editor | M | (the hub owns login) | Show the hub account; "My Games"/"My Assets"; a login popup that reuses the hub's auth path. |
| Help (Controls, Guide), Documentation | Pa | `editor_panels.c` | Guide = Welcome window; Documentation link. |
| Playbar Test / Host | P | `ui/editor.html`, `editor.c` | – |
| Playbar right-click "spawn at camera" | M | `editor.c:editor_test_map` | Add `player:{pos,rot}`. The game side must honour it (`test_map_host`, `local_game.c`). |
| Playbar Teleport popup | M | – | `editor_prompt.c` (popup), camera tween in `editor.c` |
| Canvas transform / space / face / group buttons | P | `editor.c:wire_canvas_buttons` | – |
| Canvas Wireframe / Snapping buttons | M | – | `ObjectInstance.forceWire` equivalent in `editor_render.c`; snapping state in `editor.c` |
| `#canvasInfo` Event / Server Nodes / Total | Pa | `editor.c:editor_update_object_count` | `obj_count` grows to 6 slots. |
| Windows 1-8 (Controls, Assets, Settings, Quick Add, Mods, QA Customize, Textures, Welcome) | Pa | `editor.c:editor_show_window`, `editor_panels.c:ed_window_show` | v7 content: Assets sort select and sound/video kinds; Welcome v7 layout (missing `img/map_show.png`); 8 Quick Add defaults. |
| Windows 9-15 (Sounds, Templates, Publishing, Tutorials, Tips, My Games, Load Backup) | M | – | Tutorials and Tips are YouTube iframes: show link lists instead (intentional difference, no iframe engine). |
| Popups (import, list picker, login, edit-map, confirm, publish, share, 2FA) | Pa | `editor_prompt.c` | Only prompt/confirm and the publish dialog exist. |
| Quick GUI | P | `editor_quickgui.c` | – |
| `#chatList` warnings / notices | P | `editor.c:editor_message/do_warning` | v7 warning set (section 3.12). |
| Editor event bus (`objectAdded`, ...) | M | – | Only needed if the edu/tutorial hooks are ported. Low priority. |

### 3.2 Placement, gizmo, snapping

| Feature | St | Owner | Gap |
|---|---|---|---|
| Create at origin / near camera; placeholder P | P | `editor_objects.c:ed_obj_create(_placeholder)` | – |
| Hotkeys C/G/R (always near) | M | `editor.c:page_key` | Add, with the `createCubeKey`/`createGateKey`/`createTriggerKey` settings. |
| Shift+click reuseSelected | M | `editor_panels.c` | – |
| Gizmo translate / rotate / scale, world/local | P | `editor_render.c:ed_gizmo_*` | – |
| fakeScale 1 | P | `editor_render.c` | Add fakeScale 2 (Y/Z linked). |
| modelScale (uniform `mSize` for `gen`) | M | `editor_render.c:ed_gizmo_drag` | Clamp 0.01..100, 2 decimals. |
| directionalScale (Shift/Ctrl one-sided) | M | `editor_render.c` | Conflicts with KN's "Ctrl disables snapping". Resolve as: Ctrl-hold disables snapping **except** while dragging an X/Z scale handle of a `canDirectionalScale` prefab. |
| Gizmo size `+`/`-` | Pa | `editor_render.c` | KN sizes the gizmo by camera distance / 6. Multiply by the v7 user factor (1.0, ±0.1). |
| Snapping values and settings | P | `editor_render.c`, settings | Add the toggle button; snapping ON at boot. |
| Face selection | P | `editor.c:canvas_click` | – |
| Flip X/Z (F) | P | `editor_objects.c:ed_obj_flip_xz` | – |
| T key: texture/variant/spray/skin picker | M | – | New list popup with thumbnails and search. |

### 3.3 Selection, groups, clipboard, history

| Feature | St | Owner | Gap |
|---|---|---|---|
| Single select, Alt through-invisible, double-click focus, hover highlight | Pa | `editor.c`, `editor_objects.c:ed_obj_select`, `editor_render.c:ed_pick` | The `objectHighlight` setting has no effect. |
| Scene Graph (objectList) + Focus | M | – | New right panel. |
| Shift+V / Shift+C | P | `editor.c:page_key` | – |
| Light-cone and model bounding-box toggles | Pa | `editor_panels.c` (Lightcone button has a NULL action) | Implement both toggles plus their notices. |
| Groups (volume, click mode, follow, stop, stop all, delete, duplicate) | P | `editor_objects.c:ed_group_*`, `ed_groups_sync` | KN propagates owner **scale**; v7 does not. Remove it (parity). |
| Group bulk edit (texture, colour, reflect, key+execute) | P | `editor_tools.c:panel_grouping` | It moves from the "Grouping" panel into the object panel when a group is selected (v7 layout). |
| Copy / cut / paste / prefab import-export / JSON replace | P | `editor_objects.c:ed_obj_copy/ed_paste_objects`, `editor_tools.c:ed_obj_replace` | Accept the wrapped-map form `{states|id, map:{objects}}`. |
| Object Painter + 8 scrap presets | Pa | `editor_quickgui.c:ed_object_painter_click` | Add the presets (E:66-89) and the Prefabs panel. |
| Undo/redo (poll, limit, keys) | P | `editor_objects.c` 1070-1211 | Apply the 1.13 fixes (drop oldest entry, redo state). |
| Hitbox fixer | P | `editor_tools.c:ed_fix_hitbox` | – |

### 3.4 Object property inspector

| Feature | St | Owner | Gap |
|---|---|---|---|
| Widget kit (folder, text, number-drag, slider, select, switch, colour, vector3, key, button) | P | `editor_gui.c` | Add `selectClick`, `dbutton`, `div` show/hide, and the "Press Any Key" capture (exists). |
| Pickr with map-palette swatches | Pa | `editor_gui.c:pickr_*` | Swatches = the 14 most-used colours (`updateColorSwatches`). |
| Inspector gating and controls | Pa | `editor_panels.c:ed_panel_right_object` (628-997) | Rewrite for the v7 control list (section 1.4). About 150 new controls: collision flags, texture variant/scale/rotation/encoding/frames, render side, shadows, cylinder, leaderboard, camera, step sound, minimap, spray/skin, hitbox, AI blocks, bot block, perks, spawn teamOnly/spread, teleporter checkpoint, script #, animation. |
| Lighting folder (point/spot/directional/rectarea) | Pa | `editor_panels.c` | Only point and cone exist. Need target vector3, angle, penumbra, plus helpers in `editor_render.c`. |
| Environment-zone per-object env fields | M | – | Reuse the environment-panel builder over an object. |
| Serializer (`PROPS` table) | Pa | `editor_objects.c:83-197` | Replace with the v7 OI key list (section 1.4). Today's scratch-only props (`wj cdy sd sfd cha ct esa`, AI) become real keys. |
| Prefab table / flags | Pa | `editor_data.c`, `editor_prefab_flags.h` (generated) | Regenerate from v7: 97 prefabs; new flags `editBulletCol editAICol canGrapple canImpulse showMinimap stepSrc canComplexColl canDirectionalScale fakeScale(2) frameTexture canTrans editAlphaTest editRenderSide canSprite hasSpinDir altDirection extraDirection noDefault canNeutral noRotation noScaleX noScaleY canSetObjective alterHitbox canScript sprayTexture variants edCat zone bot perkMachine packapunch mysteryBox interactMsg canTerrain genGeo`. |

### 3.5 Materials, textures, colours

| Feature | St | Owner | Gap |
|---|---|---|---|
| Texture select (17) | Pa | `editor_panels.c`, `editor_data.c` | 20 textures + variants (`tv`). Thumbnails need `assets/textures/<src>_<id>.png` (check the asset import, `tools/ui-import/game_assets.py`). |
| Asset texture browser (window 7) | P | `editor_net.c` | Video textures (`at` ending in `v`): picker only; rendering belongs to the renderer. |
| Material options (encoding, alphaTest, render side, force trans, cast/receive, force render, mimic sprite, tex rotation/scale, frame anim) | M | – | Editor writes the keys. Preview rendering depends on `client/src/prefab.c` and `map-logic.md`. |
| Colorize Random/Input/Gold | P | `editor_tools.c` | Gold per 1.13. |
| Color Merging, Optimize Faces | M | – | New in `editor_tools.c`. |

### 3.6 Terrain, lights, spawns, zones, ocean

| Feature | St | Owner | Gap |
|---|---|---|---|
| Procedural terrain panel and generator | P | `editor_terrain.c` | Unchanged in v7. (The game runtime still ignores terrain: a map-logic/renderer item.) |
| Zone panel | Pa | `editor_terrain.c:ed_zone_regen`, `editor_panels.c` | Add zoneStages, zoneWait, zoneHeight. |
| Ocean panel and preview | M | – | Panel and keys; preview via a simple animated plane (a full Ocean.js port is renderer scope). |
| Light prefabs SPOT/DIRECTIONAL/RECTAREA/LIGHT_BAR | M | – | Prefab table, inspector, helpers. The editor viewport previews only the global sun (the same as today), so per-light preview depends on renderer work outside this spec. |
| Spawn teamOnly/spread, 8-tuple export, 5 teams | Pa | `editor_io.c:ed_map_export` | Extend the tuple and team select. |
| camPos 7-tuple (static, rotation) | Pa | `editor_io.c` | – |
| New zone prefabs (KR, PVP, NOPVP, PARTNER, DOMINATION, ENVIRONMENT, KPD) | M | generated tables | Via the regenerated prefab table, plus `lineCol` boxes in `editor_render.c:draw_tool_volumes`. |

### 3.7 Trigger / logic editor

| Feature | St | Owner | Gap |
|---|---|---|---|
| Event select, Event Value | Pa | `editor_panels.c` (Technical folder) | 32 events, AI filter by name. |
| Multi-action list (≤5), add/remove (not action 0) | M | – | Today: one action with v3 keys `ta/tc/tct`. |
| Value type + key, text/number/second value, action options selects | M | – | Driven by TR action flags (`hasCust`, `type`, `bypassType`, `hasExtra`, `actionOptions*`, labels). |
| Per-action delay, condition sub-folder (12 conditions, `vals`) | M | – | – |
| Sound sub-folder for actions 14/34/54 | Pa | `editor_panels.c` (action 14 only) | – |
| Action method/cost (event 16), once-per-player, custom message, chance, destroy-on-use | Pa | scratch-only today | Real keys `tam tac sup inm cha sfd`. |
| Interface ID/Target 0..2000 + count warning (30) | Pa | `editor_panels.c`, `editor.c:warning_check_objects` | Range 1000 → 2000; limit 20 → 30. |
| Connection lines: interface | P | `editor_render.c:draw_trigger_connections` | Use the v7 hue rule (`in×18`). |
| Connection lines: teleporter channels | M | `editor_render.c` | Hue `node×3.6`. |
| Legacy single-action migration on import | M | `editor_io.c:import_doc` | `ta/tc/tc2/tct` at object level → `tas[0]`. |

### 3.8 KrunkScript code editor

| Feature | St | Owner | Gap |
|---|---|---|---|
| Script window (client/server tabs), open via menu or key 0 | P | `editor_script.c` | In-page window (1.13). |
| Syntax highlighting (the krunkscript mode, material-palenight), line numbers, fold markers, active line, matching brackets, font size setting | P | `editor_script_lang.c`, engine code view | Folding itself and Alt-F find are not ported. |
| Autocomplete (211 entries + aliases, show-hint keys) | P | `editor_script_lang.c` | – |
| Editing keys: closebrackets (pairs, type-over, explode, Backspace), smart indent, Tab / Shift-Tab, Ctrl-/ | P | `editor_script.c` | – |
| Validate (errors, line highlights, tab icons, status button) | P | `editor_script.c` | Local and live (1.13); diagnostics list. |
| Templates (client/server skeletons) and 18 script template maps | P | WP1 / WP2 | – |
| Storage `scripts{client,server}` base64, omit-if-template | P | `editor_model.c` | – |
| Test: drop the server script, compile the client, pass it to the game; Host keeps both | P | `editor_script.c`, `editor.c:editor_test_map` | – |
| Editor bus / MCP | P | `editor_bus_link.c`, `tools/editor-mcp` `script_get` / `script_set` | Krunker Box Office addition. |

### 3.9 Map settings

| Feature | St | Owner | Gap |
|---|---|---|---|
| Essential: name, welMsg, modURL, dthY | P | `editor_panels.c:ed_panel_left_show` | Add oldAO and mmId. |
| Game Modes, Classes switches | P | same | v7 lists. |
| Server Preset (grouped by `sub`) | Pa | same (`server_config.settings`) | v7 serverConfig (62 shown), grouped folders, `hideE`. |
| Weapon Config | Pa | same | 48 fields (v3 had 26). |
| Class Config (79) with copy-to-all/reset | M | – | Replaces the v3 `game` (gameConfig) folder; import migrates `game` → every class. |
| Effect Config | M | – | – |
| Environment panel | Pa | `editor_panels.c:ed_panel_right_environment` | Add `skyDomeEmisTex`, `skyDomeTexA`, `ambIndC`, soundscapes 15-19, toneMapping/exposure/outputEncoding/correctLights. Fix value ranges. |
| Custom GUI component + overlay preview | M | – | WP4. Overlay renderer shared with HUD (`map-logic.md`). |
| Export gating (`oc*`/`zone*`/`skyDome*`/`terrain*`) and default dropping | Pa | `editor_io.c:keep_map_key` | v7 default table and ocean gating. |

### 3.10 Assets, import/export, publishing, test-play

| Feature | St | Owner | Gap |
|---|---|---|---|
| Assets window (New / My / Search) | Pa | `editor_net.c` | Sort select, sound/video kinds, sound preview, owner delete, Sounds window. Prefab assets (type 3) are "Error loading Prefab": the brain must host `data.json`. |
| Mods window | P | `editor_net.c` | – |
| Export JSON (colour pool) | Pa | `editor_io.c:ed_map_export` | Add the `si`/`xyz` size pool, `tris`, 8-tuple spawns, 7-tuple camPos, `botPaths` (dead), `gui`, `scripts`, `config.cls`/`prks`, sanitizeHTML. |
| Import | Pa | `editor_io.c:import_doc` | Run size-pool expansion (`map_sanitize_json`, today skipped). Add v7 validation regexes, the legacy `game` migration and legacy trigger migration. |
| Save project / Ctrl+S | P | `editor_io.c` | v7 file name `<name>_save.txt` (same). |
| Autosave | Pa | `editor.c:page_tick` (`knEditorMap` every 2 s) | Keep as a KN addition. Add v7 timed **backups** (list, load, clear): files under `$XDG_DATA_HOME/krunknative/editor-backups/` natively, `localStorage`/IDBFS on the web. |
| Export GLTF | M | – | glTF 2.0 JSON + embedded base64 buffers writer (no new deps). |
| Export MTL zip | M | – | OBJ/MTL writer plus a store-only zip writer (small, no deps), or reuse a zip writer if one already exists in the tree. |
| Drag & drop file import | Pa | – | Native: GLFW drop callback → `importMap`. Web: `kn-shim.js` is DOM glue only, so route the drop to C. |
| `mapToEdit`, templates via URL (`?scriptT`, `?gameT`) | Pa | `editor.c:page_enter` | Template query keys. |
| Test map / Host map | P | `editor.c:editor_test_map`, `app_mode.c:app_test_map` | v7 `testInEditor` iframe → open or raise the game window (as today). Document. |
| Publish game | Pa | `editor_net.c:ed_net_publish` | v7 form fields, versioning. |
| Publish asset, My Games (edit/leaderboard/funds/delete), Load in Editor | M | – | Needs kn_brain endpoints (`doc/backend.md`). Stub cleanly if the brain lacks them. |

### 3.11 Camera, keybinds, settings

| Feature | St | Owner | Gap |
|---|---|---|---|
| Fly camera, RMB look, sprint, wheel dolly, double-click focus | P | `editor.c:set_move/page_cursor/page_scroll` | – |
| Teleport | M | – | See 3.1. |
| Keys: `,` wireframe, 0 scripting, T pickers, C/G/R, `+`/`-` | M | `editor.c:page_key` | Rebindable defaults in settings. |
| Controls window (Default/Advanced, rebinding) | Pa | `editor_panels.c` | v7 key list. |
| Settings tabs | Pa | `editor_panels.c`, `editor.c:editor_settings_save` | Add showFPS, testInEditor, autoLog, renderDist, scriptFont, autoBackups/backupTimer (v7 semantics), model bounding-box toggle. Wire the dead settings (`objectHighlight`, `borderVisuals`, `resMltV`, `antiAlias`). |

### 3.12 Performance tools and stats

| Feature | St | Owner | Gap |
|---|---|---|---|
| Counters | Pa | `editor.c` | Add Event, Server Nodes, Total. |
| Limit warnings | Pa | `editor.c:warning_check_objects` | v7 values: collidable 4500/6000, AI 40, bots 16, signs 100, leaderboards 2, wheels 10, GUI 80, interface 30, spawns 20, events 10. |
| FPS overlay | M | `editor_render.c` | 2D text in the viewport corner (or a DOM element). |
| Tools: Text / Image / Voxel / Layout / Timelapse / Transform / Frame / Breakable / Colorize / Drunk | P | `editor_tools.c` | Move into the v7 Tools panel layout. |
| Optimize Faces, Color Merging | M | `editor_tools.c` | – |
| Render: resolution multiplier, AA, render distance | M | `editor.c` / `editor_render.c` | `resMltV` → render-target scale for the editor viewport. |

---

## 4. Implementation plan

The work is split into six packages for parallel agents.

### Shared rules
- **v7.1.4 wins.** Remove v3-only behaviour rather than gate it. Mark every intentional difference with an "intentional difference from v7.1.4" code comment, and list it in section 1.13 of this doc.
- **Shared code.** Don't touch `shared/` outside the hooks listed per package. The v7 prefab ids/names, trigger tables and runtime behaviour in `shared/src/map.c` and `shared/src/triggers.c` belong to the map-logic agents.
- **Agreed tables.** The editor consumes the agreed tables from map-logic: the 97-entry prefab id list, 32/12/72 trigger indices and `tas` field names. A test asserts that the generated editor tables match those shared enums by index and name.
- **Headless only.** Every run uses `env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 XDG_CONFIG_HOME=/tmp/ed-<wp>`. Never open windows on `:0`.

### Reference capture

WP0, owned by WP2, provides the v7.1.4 reference capture that every package tests against:
- Serve `R/public` statically, so the read-only reference tree is never written to:
  ```sh
  python3 -I -m http.server 6898 --directory /home/user/jails/kru/src/Krunker_v7.1.4/public
  ```
- `npm run server` writes `server.pid` and `accounts.db` inside the tree. Use it only when a socket reply (`ksC`, `uploadM`) has to be observed, and say so in the commit.
- Launch headless Chromium:
  ```sh
  chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader \
    --remote-debugging-port=9337 --window-size=1920,937 about:blank
  ```
  Drive it with `tools/kref/cdp.js` (`setViewport`, `eval`, `shot`, `dom`).
- A new driver, `tools/editor/v7ref.js`, opens `/editor.html` and scripts the states through `Runtime.evaluate` on `KE`/`GUI`:
  - `KE.closeWindow()`
  - `GUI.panel.right.environment.show()`
  - `KE.createObjects[i]()` for each prefab
  - `KE.showWindow(n)`
  - toolbar menus opened by hovering `.menuItem`
- For each state it writes:
  - a PNG,
  - a **structural dump** of the panel: folder tree, labels, control type, min/max/step and value, in the same format as the native `KRUNKNATIVE_EDITOR_SCRIPT=dump:right:<file>`,
  - a `serialize()` JSON golden for each prefab's default object.
- Outputs go to `doc/ref/krunker-v7.1.4/editor/` (PNGs plus `*.dump.txt` and `*.json`). They are regenerated, never hand-edited.
- Structural dumps are the primary parity check. Pixel diffs (`tools/uidiff`) are secondary, because Chromium and the native engine differ in font rasterisation.

### WP1: Data model and serialization (foundation; land first)

**Status (landed):** `client/src/editor/editor_model.c` (GL-free) is the ObjectInstance constructor +
`serialize()`, getMapExport / importMap, the `tris` bake and the property table; `editor_objects.c`
and `editor_io.c` sit on it. `editor_serialize_test` checks it against v7's own `ObjectInstance`
(goldens made with `tools/editor/dump_oi_golden.js`; the whole corpus of `assets/maps` and the 53 v7
reference maps, 184 801 objects, matches) and round-trips every `assets/maps` map; the baked `tris`
equal the ones v7 shipped. `editor_panels.c` is split into `editor_inspector.c`, `editor_env.c`
(+ the terrain / zone panels from `editor_terrain.c`) and `editor_gameconfig.c` over
`editor_panel_kit.h`. Templates are loaded at run time from `assets/editor/templates.json`
(~250 KB). WP2-6 build on: `ed_prop_get/set` by v7 property name (every OI field, incl. AI / bot /
ENVIRONMENT_ZONE), `ed_model_action_*` for the multi-action list, `EditorProject.gui` / `.scripts`,
`ed_map_set_all_classes`, `ed_load_quick`, the generated `kss` / `mapCats` / `assetCat` / class and
effect values, and `EditorPrefab.variants` / `.fake_scale` / `.ed_cat`.

**Owns:**
- `tools/editor/gen_editor_data.py` and `dump_*.js`, retargeted to `R/src`, which is readable and needs no source-map extraction.
- `client/src/editor/editor_data.c/.h` and `editor_prefab_flags.h` (generated).
- `editor_objects.c`: the `PROPS` table, `ed_obj_rebuild` hooks and history.
- `editor_io.c`: export, import, sanitize, legacy migrations, the size/colour pools, `tris`, spawns, camPos, `config.cls`/`prks`, the `gui` and `scripts` passthrough.

**First commit:** a mechanical split of `editor_panels.c`, with no behaviour change, so other packages can own whole files:
- `editor_panels.c`: toolbar, windows, blank/tools/prefab panels.
- `editor_inspector.c`: object panel (WP3).
- `editor_env.c`: environment, terrain, zone and ocean panels (WP3).
- `editor_gameconfig.c`: left Essential panel (WP4).

**Generated data:**
- 97 prefabs with all flags, 20 textures with variants.
- serverConfig/classConfig/weaponConfig/effectConfig.
- modes, classes, weapons, effects, perks.
- 32 events, 12 conditions, 72 actions with all UI flags.
- 73 emitter sounds, soundscapes, `mapCats`, `assetCat`, limits.
- `gameTemplates`, `scriptTemplates`, `defaultMaps` as embedded JSON. Script templates are large, so they could be loaded at run time from `assets/editor/` instead; decide on size.

**Depends on:** `map-logic.md` for the prefab id and name list and the `tas` field names. It should match PF exactly.

**Tests:**
- A new ctest, `editor_serialize_test` (client-side but GL-free: link `editor_objects.c` and `editor_io.c` without GL). For every prefab, the native default object serializes to the same JSON as v7's `ObjectInstance.defaultFromType(i).serialize()`, using the golden from `v7ref.js` or a node dump.
- Round-trip every `assets/maps/*.json` and every v7 `R/public/maps/*.json` through import/export. Expect key equality modulo the documented quirks.
- Headless smoke:
  ```sh
  KRUNKNATIVE_EDITOR_SCRIPT="objdump:/tmp/o.json" client_bin --editor map=kanji
  ```
  then diff `/tmp/o.json`.

### WP2: Editor shell (page, menus, windows, viewport chrome, camera/keys, settings, stats)

**Status (landed):** `ui/editor.html` and its CSS are v7's (`import.py` imports the editor page by
default). `editor_panels.c` builds v7's toolbar (File .. Documentation, Lights / Zones menus,
12-row Models / Tools columns, Shift+click `reuseSelected`), the Scene Graph panel and windows
1-15 (Controls / Settings / Quick Add / Welcome / Templates / Publishing / Load Backup in full;
2 / 5 / 7 stay editor_net.c's (WP6), 9 / 14 show v7's offline state until WP6's data). `editor.c`
has v7's keys (rebindable via `kro_editor`, C / G / R / T / 0 / `,` / `+` / `-`), the Wireframe and
Snapping toggles, the six counters and v7's limit warnings, the teleport popup (900 ms tween),
right-click Test (`player:{pos,rot}`), timed backups (`editor_backup.c`) and native drag & drop;
`editor_prompt.c` ports `genericPopup` / `clearPops`; `editor_render.c` the gizmo's modelScale /
fakeScale 2 / one-sided scaling / size factor, the light helpers and the bounding-box toggles;
`editor_overlay.c` the FPS counter. WP0: `tools/editor/v7ref.js` writes
`doc/ref/krunker-v7.1.4/editor/` (window / panel / toolbar dumps, PNGs, per-prefab right-panel
dumps and `serialize()` goldens); the native `dump:` script command writes the same format
(`window`, `popup`, `toolbar` modes). `KRUNKNATIVE_SCREENSHOT`, `KRUNKNATIVE_INPUT_SCRIPT` and
`KRUNKNATIVE_UI_CMDS` work in page windows (they start once `AppPage.ready`), `wait:S` and
`ui:CALL` extend `KRUNKNATIVE_EDITOR_SCRIPT`, and `KE.kn.script('cmd:arg')` runs a script command
from a UI-cmds `ui` line. WP3-6 build on: the v7 widget markup (`eButton`, switch rows),
`editor_warning()`, `ed_generic_popup()`, `editor_script_popup()` (WP5's key 0 / menu hook),
`editor_texture_list()` (WP3's T-key hook), `ed_overlay_canvas()` (WP4's GUI preview),
`ed_backup_*` and the window shells of 9 / 14 (WP6).

**Owns:**
- `ui/editor.html` and `ui/css/editor-kn.css`, by re-importing v7 `editor.html` + `editor.css` + `editor_dark.css` via `tools/ui-import/editor_page.py`.
- `editor.c`: keys, camera, teleport, wireframe/snapping toggles, counters, warnings, settings, backups, drag & drop.
- `editor_panels.c` after the split: toolbar, windows 1-15, the Scene Graph panel.
- `editor_render.c` items: wireframe, FPS overlay, gizmo modelScale/directionalScale/fakeScale 2/size, light helpers.
- `editor_prompt.c`: popups.
- The reference-capture tooling (WP0) and editor uidiff scenarios: `tools/uidiff/scenarios/editor-*.json` with `"page":"editor"`.
- Fixing `debug_capture.c` so `KRUNKNATIVE_SCREENSHOT` and input scripts work in page modes. Coordinate with whoever owns `debug_capture.c`.

**Depends on:** WP1 (prefab categories for the Lights and Zones menus; settings keys).

**Tests:**
```sh
KRUNKNATIVE_EDITOR_SCRIPT="window:N;dump:window:/tmp/wN.txt" client_bin --editor new
```
- Diff each window's dump against `doc/ref/krunker-v7.1.4/editor/windowN.dump.txt`.
- `KRUNKNATIVE_UI_CMDS` with `key`/`click`/`shot` for hotkeys: for example, C must create a cube at the near position, checked with `objdump`.
- `--render-scenario tools/uidiff/scenarios/editor-1920.json` vs the Chromium capture.
- Run `make uidiff UIDIFF_HEADLESS=1 SCENARIOS=editor-*`, then `make smoke-native`.
- Run on the NVIDIA EGL device (`KRUNKNATIVE_EGL_DEVICE=nvidia`) once per change to the overlay GL code.

### WP3: Object inspector, materials, environment/ocean/zone panels

**Status (landed):** `editor_inspector.c` builds v7's object panel (UI:1267-5831) in blueprint key
order: the header controls, Transform (+ the group Reflect), Style (texture folder with type /
variant / asset / stretch / scale / transparency / rotation / offset / movement / frame animation,
model variants, encoding, colours, shading, fog, shadows, alpha test, render side, mimic sprite),
Technical & Logic (score, perks, interaction, health / respawn, hitbox, the AI and bot folders,
weapon pickup, interface, teleporter, script) and the type folders (faces, ENVIRONMENT_ZONE's
environment, lighting, billboard, particles, text, spawn, liquid, sound, animations). A selected
group shows v7's bulk-edit header (Texture, Color, Key + Execute, Reflect). The Trigger Event /
Trigger Actions folders are WP4's `ed_triggers_panel()` (`ED_HAVE_TRIGGERS_PANEL`; until it lands
a fallback stands in). `editor_env.c` has v7's Environment (cloud asset ids, soundscapes 1-19 +
custom sound, Render folder), Terrain, Zone (stages, wait, height) and Ocean panels, and
`editor_terrain.c` the ocean preview plane. `editor_gui.c` adds `selectClick`, the "smaller"
folder and the Pickr swatches (`updateColorSwatches`); the T key and the selectClick icons open
`viewTextureList` / `viewModelList` (textures x variants, model variants, sprays, skins, with
search). Checks: `make editor-inspector-test` (`tools/editor/inspector_check.py`): 95/97 right
panel dumps identical to v7's (29 since WP6's hook-free capture), 97/97 with the documented differences (section 1.13); the 41 edit
cases of `tools/editor/inspector_edits.json` (v7 golden `inspector-edits.json`, `v7ref.js` state
`edits`) match v7 and round-trip in `editor_serialize_test`; uidiff `editor-inspector`.

**Owns:**
- `editor_inspector.c`: the v7 object panel per section 1.4, all prefab gates and type folders, the group bulk-edit header.
- `editor_env.c`: Environment (v7 fields), Terrain, Zone (stages/wait/height), Ocean.
- `editor_gui.c`: widget kit additions `selectClick`, `dbutton`, `div`, palette swatches.
- The T-key list picker: textures × variants, model variants, sprays, skins.
- ENVIRONMENT_ZONE per-object env fields.
- `editor_render.c:draw_tool_volumes` `lineCol` for the new zones and the ocean preview plane.

**Depends on:**
- WP1, for flags and keys.
- `map-logic.md`, for which fields the runtime honours; the inspector shows all v7 fields regardless.
- Renderer owners, for previewing new material options. Until those land, the editor writes the keys but previews as today. List the unpreviewed keys in the commit.

**Tests:**
- For each of the 97 prefabs:
  ```sh
  KRUNKNATIVE_EDITOR_SCRIPT="add:<i>;dump:right:/tmp/p<i>.txt"
  ```
  Diff against the v7 dump of `KE.createObjects[i]()` → right panel. Add an `add:<id>` command to `run_script` if it's missing.
- Edit-then-serialize tests: set a value through `ui GUI.action(...)` in `KRUNKNATIVE_UI_CMDS`, then `objdump`, then compare with v7 after the same `GUI.action` via CDP.
- Screenshot the Sandstorm and Kanji maps in the editor viewport to catch regressions in normal geometry.

### WP4: Logic UI (triggers, conditions, links), game config, custom GUI

**Status (landed):** `editor_gameconfig.c` is v7's Essential panel (Force Old AO, Minimap AssetId,
Game Modes / Classes with `[ID: n]`, Server Preset grouped by `sub`, Weapon Config with Select
Icon / Asset, Class Config with Copy to All / Reset / per-group Copy, Effect Config); its
`dump:left` is identical to `left-essential.dump.txt` and the uidiff `left-panel` region is no
longer skipped. The v3 "write every class" Game folder is gone. `editor_triggers.c` builds the
Trigger Event / Trigger Actions folders (`ed_triggers_panel`, called by the inspector; per-action
controls bind `"<prop>@N"` properties, which `editor_model.c` stores in `tas[N]`).
`editor_customgui.c` is Components ▸ GUI and the overlay preview (v7's `overlayHelpers.customGUI`
on `ed_overlay_canvas()`). `editor_render.c` draws v7's connection lines (`in × 18` hue,
teleporter channels). Asset buttons (Select Asset / Icon / Image / Custom) go through
`ed_asset_target_open` / `KE.selectAsset`. Tests: `tools/editor/logic_cases.json` replayed in v7
(`tools/editor/v7ref.js logic` → `doc/ref/krunker-v7.1.4/editor/logic/`) and natively
(`tools/editor/logic_parity.py`, `editor_serialize_test`; `triggers_test` loads the v7 output);
`kn_editor_mcp` gains `actions` (the v7 list) on `triggers_create` / `_update` and
`config.cls / prks / weps` on `map_settings_set`.

**Owns:**
- A new `editor_triggers.c`: the Trigger Event and Trigger Actions folders, the multi-action list, value types, conditions, sound sub-folder, method/cost. `editor_inspector.c` calls into it through one function.
- `editor_gameconfig.c`: Essential panel, Game Modes, Server Preset grouped folders, Weapon Config (48), Classes, Class Config (79, copy-to-all/reset), Effect Config, and the `game` → `cls` import migration (shared with WP1's `editor_io.c`; WP1 owns the code, WP4 provides the mapping table).
- A new `editor_customgui.c`: the GUI component panel, element add/remove/duplicate, `importGUI` clamps, and the overlay preview.
- `editor_render.c:draw_trigger_connections`: v7 hue rules plus teleporter lines.

**Depends on:**
- WP1, and `map-logic.md` for the trigger table semantics and labels: `devOnly`, the action flags.
- `map-logic.md` for the custom-GUI renderer. The preview should call the same C function the game HUD uses to draw map GUI elements, so editor and game match. If map-logic hasn't landed one, WP4 writes a minimal box/text/image drawer in `editor_customgui.c` and hands it over.

**Tests:**
- Create a TRIGGER, add 5 actions (the 6th must be refused), set conditions and value types, then `objdump` and compare with the v7 golden made by the same `GUI.action` sequence over CDP.
- Import v7's Triggers & Scripts template and re-export it: `tas` must be unchanged.
- Check the interface count warning at 31 objects.
- Screenshot the link lines in a headless editor shot.

### WP5: KrunkScript code editor

**Status (landed):** `editor_script.c` is the window (scriptPopup / updateScript / updateTab /
io_ksC and scripting.html's page logic) as an in-page `#knScriptWin` (`ui/css/editor-script.css`
= scripting.css), `editor_script_lang.c` (GL- and DOM-free) the krunkscript mode, show-hint,
matchBrackets, closebrackets, the simple mode's indentation, toggleComment, the script store and
the diagnostics (`ks_validate`). The engine gained a generic code view of a `<textarea>`
(`html_dom_code_view` / `_spans` / `_lines`: gutter, colours, caret line, marks, no wrap, tab
stops; `widgets.c`, and on the web an overlay `<pre>` in `web_dom.c`) and C-side text editing
(`html_dom_text_selection` / `_set_selection` / `_replace` / `_caret_rect`), plus `HtmlEvent.mods`.
The reference is v7's own page (section 0): `v7ref.js scripting*` writes
`doc/ref/krunker-v7.1.4/editor/scripting*.{png,dump.txt}` in the format of the native
`dump:script:FILE`; `tools/editor/script_window_test.py` (`make editor-script-test`) checks the
four states against it, the diagnostics against ksc, editing, the UTF-8 export and the test-play
hand-off; `editor_serialize_test` checks the language side and the store round trip; uidiff
`editor-script-1920`; MCP `script_get` / `script_set`. Open: folding, find (Alt-F), the
`validatedTest` cache (Test always compiles).

**Owns:**
- A new `editor_script.c`: script state `{client, server}`, tabs, validate/save, status button, tab icons, error-line highlights, autocomplete popup, templates, font size.
- A new in-page script window in `ui/editor.html`. The v7 page (`scripting.html`) is lost. KN also runs each page as its own process, so the window becomes an **in-page window** rather than a second OS window. This is an intentional difference.
- The HTML-engine code view: a `<textarea>` mode that paints per-token colour spans (the recovered page's material-palenight theme; the monokai CSS that `editor.html` links is not what the window used), a line-highlight background and a gutter with line numbers. This lives in `client/src/html/engine/widgets.c` / `paint.c`; coordinate with the engine owners and keep it generic.
- The `web_dom.c` equivalent: on the web, a plain textarea plus an overlay `<pre>` is acceptable.
- The test-play path: drop the server script for Test, compile the client script before handing it off, keep both for Host.

**Depends on:** `krunkscript.md` for:
1. A C validate/compile entry point returning `{ok, errors[{line, col, msg}], compiled}`.
2. The lexer token classes, reused for highlighting so the editor and compiler agree.
3. The API list. Autocomplete uses the KSS 211 entries plus aliases, generated by WP1.

Until the compiler lands, the script window ships with highlighting and save, and Validate reports "Please Reconnect to Validate Script" (v7's offline text).

**Tests:**
```sh
KRUNKNATIVE_EDITOR_SCRIPT="script:open;script:tab:server;dump:window:/tmp/s.txt"
```
- Feed a script with a known error and assert the highlighted lines and the "Failed - errors found!" state.
- Check the base64 export round-trip with non-ASCII text.
- Test-play hand-off: `custMap` must not contain `scripts.server`.
- Pixel-check the window against v7's own (recovered) page: uidiff `editor-script-1920`.

### WP6: Tools, assets, import/export formats, publishing

**Status (landed):** `editor_net.c` is v7's API windows, asset selection and publishing: Assets (2,
sort select, `onlyShow`, kinds incl. sound / video, `testSound`, owner delete, right-click copies the
id), Mods (5, v7 tabs), Textures (7), Sounds (9), My Games (14: `openEdit` with Load in Editor
(`edMp`), leaderboard (`ulb`), funds (`mfnds`, 10 % fee), delete (`delC`)); `KE.selectAsset` with
every v7 `from` case (`ed_net_select` codes 0-14 and 20-22), prefab assets from
`user-assets/<id>/data.json` (`ed_obj_replace_at`); Publish > Game (`publishGamePopup`: 24
categories, Post to Hub, Mobile Friendly, "Publish v<n>" for an update, `mapResp` share link) and
Publish > Asset (`publishAssetPopup` / `updateAssetType` / `uploadAsset`); Account > My Assets. No
kn_brain or database change: the flows use kn_brain's existing `uploadM`, `edMp`, `delC`, `uploadA`
(refused) and `/api/maps|mods|assets|search` (the 1.13 rows above list where that differs from v7).
`editor_export.c` writes Export GLTF (glTF 2.0 JSON, embedded buffer and PNGs) and Export MTL (a
store-only zip of OBJ + MTL + PNGs) from the object data, GL-free. `editor_tools.c` has v7's Tools
panel (Optimizations: Optimize Faces, Color Merging; the v3 Customize panel merged in) and Prefabs
panel (Object Painter with the 8 scrap presets). `v7ref.js` captures `prefabs` without
`public/js/hook.js` (29 / 55 now show Trigger Event) and adds the states `tools` and `net-*` (the
publishing popups). Checks: `make editor-windows-test` (`windows_check.py`: windows 2 / 5 / 7 / 9 /
14 and the 7 popups vs v7, with the documented difference), `make editor-publish-test` /
`ctest -R brain_editor_publish_e2e` (`publish_e2e.py`: publish, update, publish asset, My Games,
Load in Editor, leaderboard timeout, selection, delete against a local kn_brain on a scratch
database), `make editor-tools-test` (`tools_check.py`), `ctest -R editor_export_test` (OBJ / MTL /
GLTF goldens, fast_obj / cgltf / zip read-back), uidiff `editor-win-assets` / `editor-win-sounds`.
Open: kn_brain asset hosting (`uploadA` files, `/user-assets`), `plm`, `ulb`, `mfnds`, storing Post to
Hub / Mobile Friendly, mod `loadMore` paging.

**Owns:**
- `editor_tools.c`: the v7 Tools panel layout, Optimize Faces, Color Merging, the Prefabs panel with scrap presets, Colorize Gold per 1.13, the 1.13 fixes to Frame/Quick GUI.
- `editor_net.c`:
  - Assets window v7 (sort, kinds, Sounds window, owner delete, prefab `data.json` fetch).
  - Publish game (v7 form, versioning).
  - Publish asset.
  - My Games window (edit, leaderboard, funds, delete) and Load in Editor.
  - The account menu, reusing the hub session.
- Export GLTF and Export MTL zip in a new `editor_export.c`.
- Backups storage and the Templates window data loading. This is a small overlap with WP2: WP2 owns the window markup, WP6 the data.

**Depends on:**
- WP1, WP2.
- `doc/backend.md` / kn_brain for asset, map and account endpoints. Each brain call must stub cleanly when the endpoint is missing, with a chat-list notice and no crash.

**Tests:**
- Optimize Faces on a 3×3×3 cube block: compare the "% Face Reduction" with v7 on the same JSON via CDP.
- Load the GLTF export with a glTF validator if one is available, otherwise use node `JSON.parse` and check accessor counts. Unzip the MTL zip and check the obj/mtl/png set.
- Publish against a local kn_brain in headless mode (`KRUNKNATIVE_EDITOR_SCRIPT=publish:0`, as today).

### Ordering

```
WP1 (split + tables + serializer) ──┬─> WP3 inspector ──┐
                                    ├─> WP4 logic/config ┼─> integration: full uidiff + round-trip suite
WP2 shell + WP0 ref capture ────────┼─> WP6 tools/net ──┘
krunkscript.md compiler ────────────┴─> WP5 script window
```

- WP2 and WP1 start at the same time. WP2's first task is WP0, the reference capture, which needs no native changes.
- WP3, WP4 and WP6 start once the WP1 split commit and the generated tables land.
- WP5 can start the window, code view and storage immediately. Validate waits on the KrunkScript compiler.

### Verification for every package

1. `make` must pass.
2. Run `./cmake-build-debug/proc_input_test` if `shared/` was touched.
3. Run `make smoke-native` and `make uidiff UIDIFF_HEADLESS=1 SCENARIOS='editor-*'`.
4. Run `make web smoke-web` if `web_dom.c` or `ui/js` changed.
5. Run a headless editor session on Sandstorm and Kanji with a screenshot, and inspect it.

---

## 5. Open questions

1. **Account in the editor.** v7 logs in from the editor (socket `a`). KN's hub owns login, and the client has no DB access (kn_brain only). Proposal: the editor reads the hub's stored session and shows "Username: X". Login opens the hub's login flow.
2. **Game support for test-play features.** Spawn-at-camera (`player:{pos,rot}` in `custMap`) and the KrunkScript test hand-off need game-side support. Confirm whether map-logic or krunkscript owns `local_game_host_custom`.
3. **Tutorials and Tips windows.** These are YouTube iframes. Replace them with link lists (proposed) or drop them.
4. **Video textures.** `at` ending in `v`, and the Video asset kind: does the renderer plan to support them? If not, the editor still writes the key and previews a placeholder.
5. **Renderer scope.** Ocean, tone mapping, spot/rect lights and per-object shadow flags are editable now but only preview correctly once the renderer supports them. Decide whether that renderer work is tracked in `map-logic.md` or in a separate renderer spec.
