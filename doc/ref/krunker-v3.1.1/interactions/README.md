# Menu interactions: real Krunker v3.1.1 vs Krunker Box Office

Every clickable element of `ui/index.html` and of the menu windows, driven in the
real game (offline v3.1.1 server, headless Chromium over CDP) and in Krunker Box Office's
native client (Xvfb + xdotool), both at 1280x720, guest (logged-out) state,
2026-10-10. Each `<name>.png` is a pair: **left = real v3.1.1, right = Krunker Box Office
native**, both scaled to 50 %. The world behind the menu differs (different maps /
camera), compare the page. `web/<name>.png` are triptychs real | native | web
build (`make web`, same 1280x720 viewport). `detail-*.png` are 1:1 crops.

Reproduce: real game `cd /home/user/jails/kru/src/Krunker_v3.1.1 && PORT=6777
LOBBY_COUNT=1 npm run server`, Chromium with `--remote-debugging-port=7777`, then
`node tools/kref/interactions/capture-real.js tools/kref/interactions/steps-*.json`
(`OUT=` output dir, `CDP_PORT=7778` + `steps-web.json` against `make web-serve`);
native: `KN_BIN=<build>/client_bin tools/kref/interactions/fullpass.sh` (Xvfb `:77`,
isolated `XDG_CONFIG_HOME`).

Status: **OK** behaves like v3.1.1 · **FIXED** fixed in this pass · **BLOCKED**
leaves the page in Krunker (link / new tab / download), logged instead ·
**ENGINE** UI logic correct, renderer bug (see bottom) · **GAP** not done.
External links (Krunker: `window.open(url, "_blank")`) open in a new browser tab:
`window.open` on the web build, the system browser natively (`xdg-open` / `open` /
`ShellExecute`, `client/src/ui/ui_dialog.c`); only http(s) URLs are opened, relative
links resolve against `https://krunker.io/` (the page's own site), and
`KRUNKNATIVE_NO_BROWSER=1` turns opening into logging for headless runs.

## Page (`ui/index.html`)

| Element | v3.1.1 (guest) | Krunker Box Office | Status | Pair |
|---|---|---|---|---|
| `.lgn` Login or Register / Profile item | `showWindow(5)` Account | same | OK | `w5-account` |
| Shop / Challenges / Ranked | windows 14 / 33 / 27: LOGIN REQUIRED card (click: Account) | same | OK | `w14-store`, `w33-challenges`, `w27-ranked` |
| Hub | `window.open('/social.html')` new tab | opens `https://krunker.io/social.html` | FIXED | - |
| Games | window 10, tabs re-request lists | tabs now render the offline server's lists (Featured, New, My Games, empty Favorites, Search box + `Loading...`) | FIXED | `w10-games`, `w10-tab1/3/5` |
| Mods | window 4, tabs (all "No Mods Found") | same | OK | `w4-mods` |
| Settings | window 1, tabs, search, folders | same | OK | `w1-settings` |
| Settings Export | downloads `settings.txt` (non-default values) | browser download on web; natively `$XDG_DOWNLOAD_DIR` or `~/Downloads/settings.txt` (was: log only) | FIXED | - |
| Settings Import | `prompt("Copy Paste Settings Text Here")` + `settings.import` (every setting = pasted value or default; error alert); the settings window stays open, re-rendered | web: the browser prompt; native: the page's `#confPop` popup with a text field (Enter/OK, Esc/Cancel/backdrop; the engine's text widgets handle the clipboard) | FIXED | `native-dialogs` |
| Settings preset select | `selectSettingPre`: Default (confirm), Pro, Performance (v3.1.1 `pro`/`perf` values), Custom (restores `krk_customPre`), `preIndex` | ported with the v3.1.1 value tables | FIXED | `preset-pro` |
| Settings Reset | `confirm("Are you sure you want to reset all your settings?")`, Default preset, binds reset | web: browser confirm; native: `#confPop` YES/NO popup | FIXED | `native-dialogs` |
| Spectate switch | `wantsSpectate`: next play spawns as spectator | `spectate_set_requested` (client/src/spectate.c, spectator agent) | OK | - |
| Merch / Discord / Reddit / Twitter / client installers / Terms / window links | `openURL` / `openTab` / `<a href>` new tab | opened in a browser tab (see top) | FIXED | - |
| Region label | `showWindow(2)` | same | OK | `w2-servers` |
| Download button | `showWindow(25)` | same | OK | `w25-client` |
| Host Game / Servers / Join | host window / 2 / join window | same | OK | `host`, `w2-servers`, `join` |
| Host Game: Select Modes / Toggle / Start Server | every mode without `preventCustomGames`, `gameMode<id>`, FFA checked; `windows[7].toggle(prefix)` flips the boxes; no mode checked: "Select at least one Mode" | list generated from the shared mode table (`mode_list` / `mode_hostable`); Toggle implemented (was a no-op); the first checked map + mode host the embedded server (`client->net_mode_index` at start, `local_game_host()` when it already runs); missing map/mode show Krunker's `#hostGameMsg` errors | FIXED | - |
| Idle in the menu | 90 s (`config.kickTimer`) with the menu up on a production page, not lobby host, mode without `noKick`: `disconnect("Kicked for inactivity", "000005")`, reset by any key / showWindow / entering or leaving the game | same (`menu_idle_tick`); "production" = a server that is not 127.0.0.1 / localhost / 192.168.*; never for the embedded local server (lobby host); ends on the persistent error screen "(Error 0x000005)" with the retry link (`client_net_disconnect`); `KRUNKNATIVE_IDLE_KICK=<s>` forces it for tests | FIXED | - |
| Invite | text "Copied" for 1250 ms | was 1000 ms | FIXED | `invite-copied` |
| `#policeButton` (K.P.D badge) | `shoPolicePop()`: K.P.D popup, Officer Buddy (Detective) in the class preview + name tag, intro line, `ambient_9` loop, idle lines every 15-30 s | implemented (was a no-op); the preview stays bright above `#popupBack` like Krunker's canvas | FIXED | `kpd`, `detail-kpd` |
| `#popupBack` | `clearPops()` (restores the player's class preview) | implemented | FIXED | `kpd-closed` |
| Customize | window 3 | rebuilt (below) | FIXED | `w3-loadout` |
| Footer Advertise / Support / v3.1.1 + `#mapInfo` | `#infoHolder` shown once connected, hidden in game; windows 21 / 29 / 35 | footer was never shown; now shown in the menu when connected (`ffa_<map>`), hidden in game; windows added | FIXED | `w21-advertise`, `w29-support`, `w35-changelog` |
| `#instructionHolder` | click to play / close window | same | OK | `menu` |
| KR display (`windows[13].purchase`), mail icon, team switch, claim, 2FA, consent, birthday, Twitch | hidden for a guest / not offline | handlers route like Krunker (Store; claim shows Krunker's "Ad Failed" alert) | OK | - |
| Chat input / mic / reactions | controls agent | - | - | - |

## Change Loadout (window 3) and the pickers

| Element | v3.1.1 (guest) | Krunker Box Office | Status | Pair |
|---|---|---|---|---|
| Rows per class | Attachment only if the primary has a non-forced attach slot, Reticle if it has one, Secondary greyed for classes without | was a fixed Triggerman template (wrong rows for 10 classes) | FIXED | `w6-pick-runngun` |
| Class | window 6 | same | OK | `w6-class` |
| Class card | `selectClass`: class `txts` line (or 1/3 generic), back to window 3 | class lines added | FIXED | `w6-pick-runngun` |
| Primary | `skinSelector(w)`: window 15 None/Random for that weapon's preview | new window | FIXED | `w15-skins` |
| Attachment / Reticle / Secondary | windows 31 / 32 / 20 | new windows; picks shown in window 3 (None, Tau, Custom, Desert Eagle...) | FIXED | `w31-attach`, `w32-reticle`, `w20-secondary` |
| Melee / Face / Hat / Body / Waist / Dye | windows 26 / 39 / 16 / 17 / 38 / 34 (None + Random) | new windows; "Random" shown | FIXED | `w26-melee` ... `w34-dye`, `w3-after-random` |
| Spray | window 9 (default sprays) | new window, picked name shown | FIXED / ENGINE (images) | `w9-spray`, `detail-w9-spray` |
| Skin colour swatches | `selectSkinColor(i)`: saved, swatch becomes `activeSkin` (rounded square) | implemented (was a no-op) | FIXED / ENGINE (shape) | `w3-skincolor`, `detail-w3-skincolor` |
| Hair / Chat colour `+` | `windows[13].premium()` → Store (LOGIN REQUIRED) | same | FIXED | - |
| Killcard Stat | `selectStat` saved | implemented | FIXED | - |
| Preset name / select / Load / Save / Delete | `windows[2].action`, `krk_loadouts` | implemented | FIXED | - |
| Preview colours | skin colour / cosmetics in the 3D preview | not applied (renderer) | GAP | - |

All selections use Krunker's localStorage keys (`skinColIndex`, `attachIndex`,
`reticleIndex`, `secondaryInd`, `meleeIndex`, `hatIndex`, `backIndex`, `waistIndex`,
`dyeIndex`, `faceIndex`, `sprayindex`, `kcStatIndex`, `skins`, `krk_loadouts`).
`secondaryInd` is sent in the spawn packet (`ui_loadout_secondary()`).

## Changelog (window 35)

Generated from v3.1.1's `docs/versions.txt` (`ui/windows/versions.txt`) with the
real split rule; the markup is byte-identical to the real window (318,479 chars on
the web build). `windows[34].toggleLog` folds sections. FIXED.

## Renderer findings (not fixed here)

1. **Swatch shape** (`detail-w3-skincolor`): `.skinColorItem` (30x30, border 3px,
   `border-radius: 20px` → clamped to a circle) paints its background as a rounded
   square whose corners stick out past the circular border under `#uiBase`'s
   `scale(0.627)`; Chrome clips the background to the inner circle.
2. **RESOLVED (2026-10-10): text widths match Chrome exactly; the anonymous-block ellipsis is fixed in layout_inline.c and the 3-period fallback in paint.c.** Original note — **text-overflow / text width** (`detail-w6-class`, `w31-attach`, `w20-secondary`):
   the generated card markup is byte-identical to v3.1.1's (Chrome renders the web
   build's window 31 card with clientWidth 153 / scrollWidth 173, exactly like the
   real page). Chrome's gamefont 20px advances: "Reflex Sight" 157.5 px, "Combat
   Knife" 172.5 px (R 17.5, e 15, f 12.5, l 7.5, x 15, space 7.5, S 17.5, i 7.5,
   g 15, h 15, t 12.5, C 17.5, o 15, m 22.5, b 15, a 15, K 17.5, n 15), wider than
   the 133/143 px content boxes, so a native width of ~130 px is a text-measurement
   difference, not markup. Also:
   `.classCard/.noBtnCard/.skinCard` set `overflow:hidden; white-space:nowrap;
   text-overflow:ellipsis`, but the title text sits in an anonymous block next to
   block children; Blink takes text-overflow from the parent for anonymous blocks
   ("Spray N P...", "Reflex S...", "Combat ...") while ours clips without "...".
3. **Inline-style `url()`** (`detail-w9-spray`): `background: url("textures/sprays/0.png")`
   in a `style` attribute is resolved against `css/main.css`
   (`paint.c` `paint_bg_layer` / `bg_layer_needs_clip` use `css_base_url`), so the
   spray images are missing natively; the web build shows them.
5. **Scrolled positioned text**: in the Host window (`#menuWindow` scrolled), the
   `.optName` labels (`position: relative; z-index: 1`) of the mode and class
   toggles are painted at their unscrolled positions (class names show up over
   the mode grid, mode names are clipped away); non-positioned content scrolls
   correctly.
4. **Box-shadow hole seam**: `#knPreviewHole` placed on whole device pixels (969,261)
   still leaves a 1 px under-dimmed line just above/left of it.
