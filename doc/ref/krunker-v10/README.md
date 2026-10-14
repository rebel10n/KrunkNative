# Krunker v10 reference captures (hub and editor), reconstructed

The acceptance references of the `hub-*` and `editor-*` uidiff scenarios (`tools/uidiff/gen_scenarios.py`)
for the Krunker v10 pages `ui/social.html` and `ui/editor.html` (imported from the live krunker.io,
`tools/ui-import/v10_hub_editor.py`). Parity notes: `doc/v10/editor-social.md`.

**No Krunker v10 JavaScript was run to make them.** `node tools/uidiff/capture-v10-static.js` builds each
page from the matching Krunker v7.1.4 capture's DOM (`doc/ref/krunker-v7.1.4/{hub,editor}/<name>.html.gz`,
made by v7's own client: `tools/uidiff/capture-v7.js`, `tools/editor/v7ref.js` with `KN_V7REF_HTML=1`):

- the `<head>` is the live v10 page's (`~/.cache/kn-v10/live/site/public/{social,editor}.html`, the
  `fetch_live_site.py` mirror of 2026-10-13, scripts removed), so the v10 stylesheets apply;
- the edits listed in the tool (`HUB_OPS`, `EDITOR_OPS`, `HUB_LOGIN`, `SETTINGS_MP_TAB`, `CUBE_DYNAMIC`)
  turn v7.1.4's markup into what v10's page and generators produce in that state. Each one cites its
  evidence in the webcracked v10 bundles (`~/.cache/kn-v10/webcrack/<chunk>/renamed.js:<line>`) or the v10
  page markup. Svelte component CSS (the hub login modal) is the `{hash, code}` literal of the
  webcracked shared chunk.

Chromium renders the result from a read-only 127.0.0.1 serve of the mirror (then `ui/` for files the
mirror lacks) with page scripts disabled, every other host unresolvable and https/wss blocked;
CSS animations paused at 800 ms. Each capture is `<name>.png`, `<name>.rects.json` (the region rects)
and `<name>.html.gz` (the reconstructed DOM).

| Reference | v7.1.4 source | v10 edits |
|---|---|---|
| `hub/hub-{maps,feed,leaders,market,mods,settings}` | `hub/<name>` | Krunk TV / Who to Follow / Steam badge removed, App Store badge hidden (v10 shows Android only), "BANNED", the Ranked leaderboard button, `#viewStack` |
| `hub/hub-login` | `hub/hub-login` | the above; v7's login popup replaced by v10's LoginModal (email field first for a guest with no saved name) |
| `editor/editor-1920`, `window1`, `window2`, `window9` | `editor/<name>` | `#twoFAPop` removed, Entities / Archetypes stats (hidden), the multiplayer playbar button, "- FRVR" version text, `#viewStack` |
| `editor/window3` | `editor/window3` | the above + the Settings "Multiplayer" tab |
| `editor/right-object-cube` | `editor/right-object-cube` | the above + the Cube's "Dynamic" switch |

Limits: states the v10 bundles reach differently from v7.1.4 beyond these edits are not represented
(the generators were compared function by function, see `doc/v10/editor-social.md`). The KrunkScript
window (`editor-script-1920`) keeps v7.1.4's reference: v10's `scripting.html` and `css/scripting.css`
are v7's.
