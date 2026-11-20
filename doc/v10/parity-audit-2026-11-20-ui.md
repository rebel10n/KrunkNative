# Client UI parity audit against Krunker v10.0.7 (2026-11-20)

Scope: the menu page (`ui/`), every window (`windows[]`), the settings, keybinds, HUD / player list, the
prize wheel and the account window, compared with `Krunker_v10.0.7/deob/game.deob.js` (cited
`deob-10.0.7.js:<line>`) and the live page (`Krunker_v10.0.7/live/`). The 10.0.7 changelog items themselves
are in `target.md` "10.0.7"; this audit looks for anything else the client draws or calls but does not do.

## Method and results

| Check | How | Result |
| --- | --- | --- |
| i18n keys | live `i18n/en.js` (2394 keys) vs `ui/i18n/en.json` (2341) | 53 missing: 51 `oauth.*` strings of the OAuth consent page (not used by game.js: Box Office's consent page is the brain's own, doc/v10/oauth-provider.md) and the two matchmaker captcha keys removed on purpose. Nothing to port. |
| Static element ids | live `index.html` (404 ids) vs `ui/index.html` (474) | none missing |
| Static inline callees | the 18 functions the live page's own markup calls | all handled in C |
| Settings | `SETTINGS.list` (:210172-214408): 292 entries, name / val / min / max / step / hide | every key is in the settings tabs or `ui_settings_defaults.c` (`scoreBoardTrigger` / `voiceChatTrigger` are hidden and defaulted there); 10.0.6 -> 10.0.7 changes no default (only renamed variables) |
| Keybinds | the 47 `binds` (:208949-) storage keys and defaults | all present in `ui_controls.c` with the same defaults |
| Windows | `windows[]` (:217977-222594), `showWindow(1..54)` | every used window has a gen or template; showWindow 11, 12, 22, 28, 29 are empty "unused" stubs in 10.0.7 too |
| Page-callable globals | 337 functions game.js puts on `window`, minus the ones KN's C names, minus internals (timers, storage, ads, FRVR, OneTrust, Steam, Meta) | 20 called from generated markup; triaged below |

## Findings

| # | Item | deob-10.0.7.js | KN location | Severity | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | Settings > Account "Hide Detailed Online Status" switch drawn but `toggleStatus` unhandled; brain had no `hds` route | :217688-217693, HIDE_STATUS `hdsr` :98648, answer :225259-225261 | `account_ui.c`, brain `progress.c` / `db_progress.c` | medium | fixed (e8fc4abb, 8584382b) |
| 2 | Charm already on the other slot: 10.0.7 shows a Charm Error popup with Override (clears both slots, reselects); KN showed a plain alert | :217418-217446 | `ui_loadout.c` select_charm | low | fixed (8584382b) |
| 3 | Punish popup lacked moderator Flag / Unflag and Chat Ban / Unban | :224082, :224064-224125 | `hud_ui.c` punishPopup | medium (staff) | fixed (8584382b): brain staff flag `["flg", name, on]`, `POST /moderation/chat` |
| 4 | Prize wheel "+ More" was inert; 10.0.7 opens every item the wheel can roll | :224381, :224391-224445, modal :127588-127700 | `store_ui.c` show_more_items, `kn-alt.css` | medium | fixed (e6da86b9) |
| 5 | Mod post-processing shaders: a mod's shaders add "Shader Mod #N" toggles and params to Settings (`setShaderInput`) | :205829-205935, :216668, :218092-218120 | none: KN loads no mod shader passes | medium | open: a renderer feature (TSL node graph as a full-screen pass); the settings rows follow once passes exist |
| 6 | `copyGameLink` (host message "Click to Copy", team links) | :204616-204621 | n/a | none | dead in 10.0.7: its only caller passes the try-again text without a link (:204804) |
| 7 | `selectPack` / `selectPackPop`, `downloadPack` | :217738-217746, :223432 | n/a | none | official desktop client only (`window.ClientSession`) |
| 8 | `debugBots`, `botsPopup` | :224130-, :221123 | n/a | none | developer tools (`isDev`) |
| 9 | `premiumConfirmed`, `clearName` | :223529, :219105 | n/a | none | the premium subscription, which Box Office does not sell (target.md) |
| 10 | `pfpPopup` / `uploadProfilePic` | :218978, :223294 | `account_ui.c` `knPfpPick` | none | Box Office's own picker covers it |
| 11 | `getDCExport` / `disconnectPopup` (copy disconnect diagnostics for support) | :229972-229975 | none | low | open: needs a decision on what Box Office support receives |
| 12 | `grabMap` / `openEdit` | :228260-228276 | `editor_net.c` | none | handled in the editor page |

## What remains

- **Mod shaders (5):** a renderer port (mod shader graph -> post-process pass in `client/src/`), then the
  Shaders folder rows in `ui_windows.c`. Out of this UI pass.
- **Disconnect report (11):** Krunker's popup builds a JSON blob for its support. Box Office would need a
  support destination first.
- **Not checked here:** the svelte components' inner layouts window by window (the uidiff scenarios cover
  them where captures exist), and the HUD's in-game behaviour (owned by the gameplay audit).
