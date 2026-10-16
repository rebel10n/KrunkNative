# Map editor (`editor.html`) and Krunker Hub (`social.html`): Krunker v10 parity

Krunker v10.0.2 is the only version. This file lists every feature of the live v10 editor and hub
pages (krunker.io, fetched 2026-10-13), what Krunker Box Office had before this work, what it has
after, and the evidence in the v10 code.

## Sources

| Abbr | What | Where |
|---|---|---|
| P10 | the live pages, raw | `~/.cache/kn-v10/pages/live/{editor,social,scripting}.html` + `css/`, `static/` (fetched through the HTTP proxy; byte-identical to the `fetch_live_site.py` mirror apart from `?build=`) |
| E10 | v10 editor bundle `static/editor-BB8B-v0i.js`, webcracked, identifiers renamed | `~/.cache/kn-v10/webcrack/editor/renamed.js` |
| S10 | v10 hub bundle `static/social-HkHNbDV5.js` | `~/.cache/kn-v10/webcrack/social/renamed.js` |
| IDX / CFG / CLS | shared chunks `index-ijbf4YAj.js` (editor GUI blueprints, Svelte components), `config-BESDNTCo.js`, `classes-CpVJB-jW.js` | `~/.cache/kn-v10/webcrack/shared/{index,config,classes}/renamed.js` |
| V10 | the v10 game client (prefab / trigger tables) | `~/.cache/kn-v10/v10.js` (`doc/v10/ui.md`) |
| v7 | Krunker v7.1.4 | `/home/user/jails/kru/src/Krunker_v7.1.4/` (`src/editor.js` E:, `src/ui/editor.js` UI:, `src/social.js`) |

webcrack: `webcrack` 2.16 from `~/src/kru-deob-1`, run from a private copy with isolated-vm 6.2 for
Node 24 (`~/.cache/kn-v10-tools/wc`), then `~/.cache/kn-v10-tools/rename.py`. The bundles were only
read; none of v10's JavaScript was run (the uidiff references are reconstructed without it:
`doc/ref/krunker-v10/README.md`).

Pages and CSS: `tools/ui-import/v10_hub_editor.py` (`make ui-import`). Status: **done** = at v10
parity, **partial**, **missing** (not ported), **KN** = intentional difference.

## 1. Editor

### 1.1 Page, CSS, chrome

| Feature | v10 (evidence) | Before | After |
|---|---|---|---|
| Page skeleton | P10 editor.html: `#twoFAPop` removed; `#entityHolder/#entityCount/#archetypeHolder/#archetypeCount`, `#viewStack` added; FRVR SDK, altcha, cdnjs CodeMirror | v7.1.4 page | **done**: v10 page; KN keeps `#twoFAPop` (2FA logins, `css/kn-twofa.css`), `#knAiOption`, canvas + web glue |
| editor.css | +`.entityPop`, `.history-*`, `#mpToolbarBtn`, `.mpBtn*`, `.settRow`, `#mp-chat`; −8 `twoFA*` rules; `#playbar` 4 columns; `#popupHolder` z-index 2147483646 | v7.1.4 | **done** (v10 file) |
| Version text | "Krunker Editor v2.0.0 - FRVR" (E10:13908) | "- Yendis Entertainment" | **done** (`editor.c`) |
| Toolbar menus File..Documentation | identical to v7 (IDX:14434-14552) | v7 | **done** |
| Object > Group > Save as Archetype; Object > Entity > Place / Edit Selected / Archetypes | IDX:14511-14516 → E10:18065, 18095, 18128, 18417 | missing | **missing** (entities, 1.5) |
| Tools / Basic menu lists | 99-prefab table: Tools gain BOT_PATH_NODE, ENTITY; Basic gains SCRAPCRATE | 97 v7 prefabs | **done** (`editor_data.c` regenerated) |
| Playbar | ▶ / host / teleport + 4th `#mpToolbarBtn` group_add (E10:9700-9714) | 3 buttons | **done** (button opens window 16) |
| canvasUI toggles | unchanged | v7 | **done** |
| canvasInfo | + Entities n/64, Archetypes n/32, shown when either > 0 (E10:19940-19960; CFG:7869-7870) | 6 stats | **done** (`editor_update_object_count`) |
| Editor events `transformEnd`, `objectPropertyChanged`, `mapCleared` | E10:15286-15301, 9771 (multiplayer sync) | none | **missing** (only multiplayer uses them) |

### 1.2 Windows, popups, settings

| Feature | v10 | Before | After |
|---|---|---|---|
| Windows 1-15 | same headers (E10:12926-13514) | v7 | **done** |
| 8 Welcome | no "earn real money", no "$ paid to developers" item, no Scripting Discord (E10:13296-13323) | v7 text | **done** |
| 1 Controls | Advanced tab + "Multiplayer" folder (Toggle Player List TAB, Focus Chat ENTER; IDX:21141-21170) | v7 | **missing** (multiplayer keys have nothing to drive) |
| 3 Settings | 5th tab "Multiplayer": Player Name, Render Coop Outlines, Player Opacity, Streamer Mode, Auto Chat Scroll in localStorage `mp_settings` (E10:13035, 9416-9468; IDX:21705-21760) | 4 tabs | **done** (settings stored, unused: no rooms) |
| 16 Multiplayer, 17 Players | host form / room view; player list with TP / SEL / KICK / BAN (E10:9595-9646, 9686-9696) | missing | **KN partial**: v10's "no room" form; Host / Join say there is no editor room server |
| `#twoFAPop` | removed (dead `twoFAres` E10:20838) | v7 popup | **KN**: kept for KrunkNative 2FA |
| Entity / archetype popups (`entityPop`) | E10:18099, 18138, 18425, 18589 | missing | **missing** |
| initSettings `mpPlayerListKey`, `mpChatKey` | E10:14605-14606 | missing | **missing** (multiplayer) |
| Quick GUI | unchanged | v7 | **done** |
| Altcha captcha, FRVR auth | IDX:3055-3250, E10:12823 | hub login | **KN**: the account server signs in over the hub socket |

### 1.3 Panels and inspector

| Feature | v10 | Before | After |
|---|---|---|---|
| Essential: closed "History" folder, Recent Actions (redo items, then undos newest first) | IDX:15111-15122, 14362; E10:15451-15503 | missing | **done** (`editor_gameconfig.c`, `ed_history_html`) |
| Undo: new action clears redo; redo goes back on the undo list | E10:15319-15338, 15344-15450 | v7: redo kept, redo not undoable | **done** (`editor_objects.c`) |
| Bounce -10..10 | IDX:15348 | -3..3 | **done** |
| Spin Axis Y / X / Z | IDX:15442 | Y / X | **done** |
| Dynamic switch (canDynamic: CUBE, TRIGGER, CUSTOM_ASSET), key `dyn` | IDX:15953-15962, E10:7236, 7814 | missing (key kept as unknown) | **done** |
| Crate Type (WEAPONBOX 4, SCRAPCRATE 5 options), key `crt` | IDX:16786-16829, E10:8758-8768 | missing | **done** |
| AI Boss (`boss`), Can Contact Hit (`cnch`) | IDX:17040, 17226; E10:8459, 8480 | missing | **done** |
| ENVIRONMENT_ZONE / Environment Fade Time (`ambFadeTime`, map key dropped when 0) | IDX:18990, 20052; E10:8720, 14071, 16568 | missing | **done** |
| Asset map-slot folders (Lightmap .. Roughness, `mp`) | IDX:16449-16501, E10:6740-6770, 8261 | key passed through | **partial**: `mp` round-trips, no folders |
| BOT_PATH_NODE block (GroupId / NodeId / Next / Reversible, Fix Node, Create Next Node / Group, chain lines) | IDX:15704-15776, E10:17833-17885, 15532-15584 | missing | **partial**: prefab, keys and botPaths round trip; no inspector block or lines |
| ENTITY inspector buttons (Edit Entity / Edit Archetype) | IDX:15179-15193 | missing | **missing** |
| Tools > Deoptimizations > Turn On All Faces | IDX:20250-20252, E10:20159-20188 | missing | **done** (`editor_tools.c`, `tool:turnOnAllFaces`) |
| objectList / prefab / gui / terrain / zone / ocean panels | unchanged | v7 | **done** |

### 1.4 Map data

| Feature | v10 | Before | After |
|---|---|---|---|
| Prefabs 97 SCRAPCRATE, 98 ENTITY; 41 BOT_PATH_NODE (v7 TERMINAL) | V10 L156724, L156744, L155465 | dropped on import (id ≥ 97 invalid) | **done** (`gen_editor_data.py` + the shared appendix of `tools/maps/gen_prefab_data.py`) |
| Conditions 13 (+ Custom Action), actions 75 (+ Spawn / Despawn Entity, Set Entity Value) | V10 L162580, L163648-163700 | 12 / 72 | **done** |
| triggerActionLimit 8; entityLimit 64, archetypeLimit 32, archetypeObjectLimit 40 | CFG:7866-7871 | 5, none | **done** |
| botPaths import (a BOT_PATH_NODE per point, `bng/bn/bnn/bnr`) and export (grouped, sorted by `bn`) | E10:17046-17064, 16405-16420, 8038-8043 | dropped | **done** (`editor_model.c`, `editor_serialize_test` v10_checks) |
| ENTITY `eid/ar/tag/as/sp`, parts `pn/sfz` | E10:7443-7457, 8770-8787 | dropped | **done** |
| `scripts.shared` | E10:10253 | dropped | **done** (kept, exported and edited in the Shared tab; the template is omitted on export) |
| top-level `archetypes` | E10:1951-2023 validation | kept as an unknown map key | **partial**: kept, counted; no editing |

### 1.5 Entities and archetypes, multiplayer, scripting

| Feature | v10 | After |
|---|---|---|
| Archetype save / place entity / entity popup / archetype popup / stage editor (plate at y 5000, radius 600) / browser / ghost previews / "archetypes" undo kind / prefab dangling and strip | E10:18065-18658, 18241-18300, 15319-15364, 17611, 17990 | **missing** (L; the game-side entity runtime is open too, `doc/v10/map-format.md` §9) |
| Multiplayer co-editing: `mpHost/mpJoin/...` (21 sent, 27 received) on `editor.krunker.io/ws`, invites `?room=`, chat `#mp-chat`, remote players and outlines, script locks | E10:8945-10250, 20263-20314 | **missing** (needs an editor room relay in kn_brain) |
| KrunkScript Shared tab (third tab after Server; template with `onEntityInit` / `simulateEntity`; 48 shared autocomplete entries), 16 new client/server entries (227 in all), client/server templates, `ksT` sends `{client, shared}` after validateMap | E10:10253, 11795-12624, 14901-14930, 15045-15075 | **partial**: the tab, template, hints, Validate (`KS_SIDE_SHARED`: GAME.UTILS and the entity hooks only, `ks_check.c`), the export round trip and Test play's check are **done** (`tools/editor/kss_v10.json`, `gen_editor_data.py`, `script_e2e.py`); the entity runtime that runs shared scripts is **missing**; the 18 v10-only GAME members the checker now accepts are no-ops until their subsystems land (`ks_api.h`) |
| `scripting.html` / `css/scripting.css` | v10's page has a third tab (`shared`, `scriptArea_shared`) and loads `static/scripting-*.js` (not cached; the window's hint rules are the v7 page's); the markup otherwise matches the recovered v7-era page | **partial**: the Shared tab is in the window (editor_script.c), the page's JS is unverified |
| Dev bridge `?devbridge=` RPC | E10:10356-11300 | **KN**: `editor_bus_link.c` + `tools/editor-mcp` already cover it |
| Publish Asset per-map UV channels | E10:20417-20456 | **missing** (S, brain storage) |

## 2. Hub (`social.html`)

v10's hub is still v7's social.js with targeted changes (S10 routes, settings and globals match v7).

### 2.1 Page and navigation

| Feature | v10 (evidence) | Before | After |
|---|---|---|---|
| Page markup and CSS | P10 social.html, css/social.css (+`.oCurrencyLine`, `.oCurrencyItem`, `.playerProfileRank`, `.pStTitle`; −`twoFA*`; `#popupHolder` z-index), shared `static/index.css` (`io-*`, CSS variables) | v7.1.4 | **done** (v10 files) |
| navBar, menu buttons feed / market / leaders / maps / mods / items, `updateWindow(p,q,s,i)` routes, `?games` | S10:6943-7003 | v7 | **done** |
| Settings popup (13 settings) | S10:1749, 1949 (= v7) | v7 | **done** |
| Krunk TV, Who to Follow side boxes | removed from the page | still written | **done** (`side_boxes`: the ad box only, S10:2389) |
| App badges: Android only | S10:1514-1518; no `#steamDB` | iOS + Android (+ Steam in scenarios) | **done** (KN: opens the Download window) |
| `#hackText` BANNED | page markup | from markup | **done** |
| Krunker News side box | S10:4342 (`news`) | static | **missing** (brain route GET /social/news exists now) |
| Market ambient loop dropped (ambient_8 Black Market kept) | S10:6953 | neither | **missing** (S) |
| Page title per route | S10:7095 | none | **missing** (optional) |
| Deep links autoReport / autoGift / autoTrade / openPubMo | S10:2810-2831, 2178 | none | **missing** (S) |

### 2.2 Login

| Feature | v10 | Before | After |
|---|---|---|---|
| LoginModal on the view stack (`#viewStack`): Email / Username toggle (email first with no saved name), hidden password until typed, Login, message line; overlay click dismisses | S10:4375 → IDX:8478 (stack push), 11466-11870 (component), 8534 (popup frame), 12537 (view stack) | v7 username / password popup | **done** (`social_ui.c`, `css/hub-svelte.css`). **KN**: username + password over the hub socket (`a`); an email address is refused with a message; no sign-in code, no altcha |
| 2FA popup | removed (`twoFAres` dead, S10:6893) | v7 | **KN**: kept |

### 2.3 Profile

| Feature | v10 | Before | After |
|---|---|---|---|
| Header, follow counts, Since, clan | S10:2748-2779 | done | **done** |
| Followers / Following popups (`gfs`/`gfg` → `fwsr`/`fwr`), Follow (`instaFollo`), `deleteFollower` | S10:5346-5399 | request sent, reply ignored | **done** |
| Ranked rank icon `.playerProfileRank` | S10:2772 | missing | **missing** (brain profile has no rank) |
| Chat Ban button | S10:2791-2800, 4692-4714 | missing | **missing** (brain) |
| Stat tabs General / Killstreaks / Other / Class / Raids / Match History | CFG:9472, S10:2905-2940, 1090-1500 | v7 four + KN Season | **missing** (Raids data and match history by name need the brain) |
| Badges tab (68 badges) | S10:30-457 | "No Badges" | **missing** (M) |
| Flag / report / verify / give badge popups | S10:2205-2214, 2779-2790 | buttons only | **missing** (brain serves `rpt`, `flg`, `ver`) |
| Reports tab (`grp` → `rps`) | S10:4167-4200 | hidden | **missing** |
| Creations actions (favourite, feature, delete, edit, publish mod) | S10:6608-6842 | partly no-ops | **missing** (brain serves `fvM`, `ftC`, `delC`, `edMp`, `uploadMo`) |

### 2.4 Leaderboards, market, maps, feed

| Feature | v10 | Before | After |
|---|---|---|---|
| Ranked leaderboard `player_ranked_matches` ("N matches") | CFG:9497, 9535; page button | missing | **partial**: page button + formatter; the brain has no such query yet |
| Nukes / Inventory / Zombies boards | CFG:9497 | formatter only | **missing** in the brain (`leaderboards.c` k_queries) |
| Market, inventory, sales, quick sell, Black Market, trading, gifting | S10:3179-3816, 5152-5797 | economy port (57b5e4c) | **done** |
| Item metadata (nametag, wear, serial) | S10:2566-2580, 3324, 5452-5495; CFG:7039 | none | **missing** (L: item instances) |
| Trade history currency line, paging | S10:2560, 2630, 3267, 3836 | none | **missing** (S / S-M) |
| Black Market `rank` / `ownSkins` requirements | CLS:3885-3910, S10:3225 | accAge / stats | **missing** |
| Item tooltips (`createTip`) | S10:6843-6880 | none | **missing** |
| Maps / mods / map page tabs; map leaderboard `glb` | S10:6040-6213, 3123-3162 | lists done, `glb` missing | **partial** |
| Feed tabs, posts, comments, likes | S10:4930-5099 | client done, brain ignores the tab | **partial** |
| Giveaway posts | S10:4930, 5077-5099 | none | **missing** (escrow in the brain) |
| DMs, chat sidebar | S10:2224-2343 | done | **done** |
| Live DM push `msg` | S10:6922 | none | **missing** |
| Turf Wars, notifications, in-game socialModal | not in social.html (main client, `doc/v10/ui.md`) | — | out of scope |

## 3. Tests and references

- `tools/uidiff/scenarios/{editor,hub}-*`: references `doc/ref/krunker-v10/` (reconstructed without
  v10's JS, `tools/uidiff/capture-v10-static.js`); `editor-script-1920` keeps v7.1.4's (identical page).
- `editor_serialize_test` `v10_checks()`: BOT_PATH_NODE, ENTITY, SCRAPCRATE, CUBE keys, botPaths and
  `scripts.shared` round trips; v7.1.4 goldens ignore prefab 41 and accept the v10 keys.
- `tools/editor/logic_parity.py` compares with v7.1.4's dumps except the v10 deltas (action / condition
  option lists, History folder, the 8-action limit).
