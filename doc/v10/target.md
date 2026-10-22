# Krunker v10.0.3: the target

Krunker Box Office plays **Krunker v10.0.3**, the latest v10 build. It is the target of the
client, the server, the ruleset identity and the UI work (set by the user, 2026-10-21).

## Reference bundles

- **Target: v10.0.3.** `~/jails/kru/src/Krunker_v10.0.3/game.js` (13.1 MB). It is the
  bundle decrypted from the WASM loader pipeline, and it is still minified: one long line,
  mangled identifiers, a string array. Read it through the static webcrack pass that
  `changelog-10.0.3.md` describes. Its version literal is `'10.0.3'`.
- **Prior version: v10.0.2 (reference only).** `~/.cache/kn-v10/v10.js`, the renamed copy of the
  2026-10-13 bundle (the original is `~/Downloads/v10.0.2.js`). It stays as the reference for the
  values v10.0.3 did not change. The `v10:N` line citations in this directory point into it.
- **Older context.** Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4`, specs in
  `doc/v7/`) stays the reference for what v10 kept unchanged.

## What the target changes

- Identity: the ruleset `version` is `"10.0.3"` (`shared/src/rules/ruleset.c`). The build's client
  version is `10.0.3r1` (`shared/include/krunker_build.h`, `tools/krunker/build.json`). The client
  and server logs read `Plays Krunker v10.0.3`. The Android and iOS version fallbacks and the
  example news item name 10.0.3.
- What's New: `ui/windows/kn-versions.txt` leads with UPDATE 10.0.3, and the What's New tile shows
  the newest entry.

## What it does not change (yet)

- Gameplay values, the HELLO / INIT ruleset bytes (HELLO bit 2, INIT ruleset 3) and the packet
  layouts. A value stays as it is until someone checks it against the 10.0.3 bundle.
  `changelog-10.0.3.md` lists what the bundle changed.
- Provenance. A ported value or behaviour keeps its citation of the bundle it was checked against
  (`v10.0.2`, `v10:N`) until it is re-checked against 10.0.3.
- `doc/ref/krunker-v10.0.2/` and `doc/ref/krunker-v10/` are captures of the 10.0.2 page. The
  uidiff scenarios refer to them by path.
- `ui/` is the v10.0.2 page import. It is regenerated from the 10.0.3 bundle only by a separate
  `make ui-import` pass with a pixel diff.
- The filenames in this directory are unchanged. The spec files describe the 10.0.2 bundle unless
  a section says otherwise. `changelog-10.0.3.md` is the 10.0.2 to 10.0.3 review.
