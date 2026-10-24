# Krunker v10.0.3: the target

Krunker Box Office plays **Krunker v10.0.3**, the latest v10 build. It is the target of the
client, the server, the ruleset identity and the UI work (set by the user, 2026-10-21).

## Reference bundles

The references live in `~/jails/kru/src/Krunker_v10.0.3/deob/` (read only; its `README.md` lists
every file). New extracted tables may be added under `deob/tables/` and listed in that README.

- **Target: v10.0.3. Read `deob/game.deob.js`.** It is `~/jails/kru/src/Krunker_v10.0.3/game.js`
  (13.1 MB, decrypted from the WASM loader pipeline: one minified line, a string array) through
  webcrack: strings inlined, pretty-printed, ~263k lines; identifiers stay mangled. Cite it as
  `deob-10.0.3.js:<line>` (or `deob:<line>`): every line number and byte or character offset in
  this directory is into this file. Cite the minified `game.js` only for a raw byte offset. Its
  version literal is `'10.0.3'`.
- **Tools and tables.** `deob/tools/deob.cjs` is the decoder (webcrack with a `node:vm` sandbox that
  only evaluates the bundle's string-array decoder; `cd deob/tools && npm install --ignore-scripts`).
  `deob/tools/extract.js` dumps a bundle's string / number / key sets (version diffs);
  `deob/tools/extractors/` are the table dumpers. `deob/tables/` holds the 10.0.3 weapon, class,
  mode, prefab, packet and config tables and their diffs against KN's; `deob/audit-2026-10-24/`
  is the per-area parity audit.
- **v10.0.2: history only.** `deob/v10.0.2/game.js` (the 2026-10-13 bundle, formerly
  `~/Downloads/v10.0.2.js`) and `deob/v10.0.2/game.deob.js`. It explains what 10.0.3 changed; no
  value is taken from it. The older `v10:N` / `v10.js:N` citations are lines of `deob/v10.0.2/game.js`
  (they were taken from an identifier-renamed copy with the same line layout; its `_N` names are not
  in the file).
- **Older context.** Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4`, specs in
  `doc/v7/`) explains the code v10 kept; v3.1.1 and v7.1.4 map JSON must still load.

## Source policy

Every value and rule is checked against v10.0.3 directly (user, 2026-10-24, once the whole bundle
was decoded): weapon, class and mode stats, physics, map fields, object flags, packet shapes and UI.
There is no "unchanged since 10.0.2" fallback. A ported value cites its `deob-10.0.3.js:<line>` in a
code comment; a deliberate difference from 10.0.3 is documented in code. A comment or spec that
cites only v10.0.2 (`v10:N`) or v7.1.4 is not verified yet: re-check it against `game.deob.js` when
it is touched.

## What the target changes

- Identity: the ruleset `version` is `"10.0.3"` (`shared/src/rules/ruleset.c`). The build's client
  version is `10.0.3r1` (`shared/include/krunker_build.h`, `tools/krunker/build.json`). The client
  and server logs read `Plays Krunker v10.0.3`. The Android and iOS version fallbacks and the
  example news item name 10.0.3.
- What's New: `ui/windows/kn-versions.txt` leads with UPDATE 10.0.3, and the What's New tile shows
  the newest entry.

## What it does not change

- The HELLO / INIT ruleset bytes (HELLO bit 2, INIT ruleset 3) and KN's own packet layouts (wire
  compatibility). `changelog-10.0.3.md` lists what the bundle changed.
- `doc/ref/krunker-v10.0.2/` and `doc/ref/krunker-v10/` are captures of the 10.0.2 page. The
  uidiff scenarios refer to them by path.
- `ui/` is the v10.0.2 page import. It is regenerated from the 10.0.3 bundle only by a separate
  `make ui-import` pass with a pixel diff.
- The filenames in this directory are unchanged. The spec files describe the 10.0.2 bundle unless
  a section says otherwise; `gameplay-1003-audit.md`, `constants-1003.md`, `rules-1003.md` and
  `deob/audit-2026-10-24/` re-check their values against 10.0.3. `changelog-10.0.3.md` is the
  10.0.2 to 10.0.3 review.
