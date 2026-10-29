# Krunker v10.0.4: the target

Krunker Box Office plays **Krunker v10.0.4**, the latest v10 build. It is the target of the
client, the server, the ruleset identity and the UI work (set by the user, 2026-10-28: "the game is
now 10.0.4. we need parity with the recent features"; v10.0.3 was the target from 2026-10-21).

## Reference bundles

- **Target: v10.0.4. Read `~/jails/kru/src/Krunker_v10.0.4/deob/game.deob.js`.** It is kru's
  decoded copy of the live bundle (`https://kru.eli.gift/game.deob.js`, last-modified 2026-10-28
  02:37 UTC), byte for byte; its `README.md` gives the sha256. Strings are inlined and the file is
  pretty-printed; identifiers stay mangled. Cite it as `deob-10.0.4.js:<line>`. It was decoded by
  kru's pipeline, not by `deob.cjs`, so its line numbers are not comparable with 10.0.3's file. Its
  version literal is `"10.0.4"` (:16403).
- **v10.0.3: history.** `~/jails/kru/src/Krunker_v10.0.3/deob/` (read only; its `README.md` lists
  every file). `deob/game.deob.js` is 10.0.3's `game.js` through webcrack (~263k lines); the code
  ported before 2026-10-28 cites it as `deob-10.0.3.js:<line>` (or `deob:<line>`). Those citations
  stay valid for what they describe; re-check the value against 10.0.4 when the code is touched.
  New extracted tables may be added under `deob/tables/` and listed in that README.
- **Tools and tables.** `Krunker_v10.0.3/deob/tools/deob.cjs` is the decoder (webcrack with a
  `node:vm` sandbox that only evaluates the bundle's string-array decoder; `cd deob/tools && npm
  install --ignore-scripts`). `deob/tools/extract.js` dumps a bundle's string / number / key sets
  (version diffs); `deob/tools/extractors/` are the table dumpers. `deob/tables/` holds the 10.0.3
  weapon, class, mode, prefab, packet and config tables and their diffs against KN's;
  `deob/audit-2026-10-24/` is the per-area parity audit of 10.0.3.
- **v10.0.2: history only.** `Krunker_v10.0.3/deob/v10.0.2/game.js` (the 2026-10-13 bundle,
  formerly `~/Downloads/v10.0.2.js`) and `deob/v10.0.2/game.deob.js`. It explains what 10.0.3
  changed; no value is taken from it. The older `v10:N` / `v10.js:N` citations are lines of
  `deob/v10.0.2/game.js` (they were taken from an identifier-renamed copy with the same line layout;
  its `_N` names are not in the file).
- **Older context.** Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4`, specs in
  `doc/v7/`) explains the code v10 kept; v3.1.1 and v7.1.4 map JSON must still load.

## Source policy

Every value and rule is checked against the target bundle directly (user, 2026-10-24, once the whole
bundle was decoded): weapon, class and mode stats, physics, map fields, object flags, packet shapes
and UI. There is no "unchanged since" fallback. A ported value cites its `deob-10.0.4.js:<line>` in
a code comment (code ported before 2026-10-28: `deob-10.0.3.js`); a deliberate difference from the
target is documented in code. A comment or spec that cites only v10.0.2 (`v10:N`) or v7.1.4 is not
verified yet: re-check it against the target bundle when it is touched.

## 10.0.4 (2026-10-28)

- Build: `clientVersion` `10.0.4r1` (deob-10.0.4.js:16403-16404), `buildVersion`
  `DV6B3wX1sg41pVR92UCbVWEpILgvBHbR` (:19598), ahNum step `0x2b` (:98686; 10.0.3: `0x3`).
  `shared/include/krunker_build.h` and `tools/krunker/build.json` are regenerated from it
  (`make krunker-build`); official joins take the published copy (`dl.krunk.io`, krunker_build_fetch.h).
- Wire: one new server-to-client packet, `SKIN_STAT` `"sks"` (skin id, stat, value; :184311). The
  10.0.4 client has no handler for it and neither does KN (unknown packets are counted and dropped,
  `krunker_link.c`). The cheat check, the per-player layouts ("0" 53 values, "k", "l") and the
  client-to-server packets are unchanged. config.networkSounds is the same 399 names (:18263).
- Changelog (krunker.io/docs/versions.txt, "UPDATE 10.0.4"):
  - Candy Heist: the Halloween event until November 7 (collect candy, bank it at the Cauldron, avoid
    the Hollowmen; mode 37, map 46), its challenges with KR rewards, titles, event drops and the
    Lil' Cauldron pet.
  - General: a map loading screen, less stutter when a match starts, an Interact button on mobile, a
    Privacy & Age section in profile Settings, item drops equippable without relogging,
    chat-restricted players can vote to forfeit, matches played alone end with ENDED instead of
    DEFEAT, and skin / waist / shoe / Shotgun preview and map-replay fixes.
  - Inventory & Market: item locks (against quick-sell, listing and trades), Select mode to
    quick-sell or list many items at once, Buy multiple on listings, the "Market is busy" and
    listed-paints fixes.
  - Raids: bosses no longer drop nothing; Bastion's Hydra and Catacombs give double drops on double
    drop weekends.
  - KrunkScript & Editor: AI Rarity Chances, `doubleRaidDrops` in `GAME.CONFIG.getMatch()`, studios
    can publish / unpublish / edit currencies, setting a score counts only the difference toward
    score challenges, a better third-person arm for `player.pose.holdL`.
- Real-money checkout (doc/v7/decisions.md): C drives the purchase (the game socket's `xst` /
  `xpurchase` / `xsin`, FRVR's iap requests); the payment provider's own hosted page opens in the
  system browser, never krunker.io's page or the JS game. The provider credits the account; the
  client waits for the server's answer or a funds increase (client/src/ui/kio_checkout.c,
  kio_checkout_stripe.c; doc/v10/checkout-browser.md, doc/v10/checkout-stripe.md). Tested against
  fakes only: no live payment has gone through it yet.

## KrunkNative additions

Some things are KrunkNative's own, added on top of v10 on purpose. They are mods, not parity
gaps, and audits leave them out. One is the Nettspend class: its id is always the last class, after
v10's `CLASS_COUNT`. The other is the kept v3.1.1 and v1.5.2-era maps (`311_*`, `152_*`).
`kn-additions.md` lists them, says why they are kept, and says what to migrate if v10's class table
grows. The KN id ranges above Krunker's tables (weapons from 64, windows from 100, `NET_PACKET_EXT`
subtypes) follow AGENTS.md.

## What the target changes

- Identity: the ruleset `version` is `"10.0.4"` (`shared/src/rules/ruleset.c`). The build's client
  version is `10.0.4r1` (`shared/include/krunker_build.h`, `tools/krunker/build.json`). The client
  logs `[client] Krunker v10.0.4` and the server `Plays Krunker v10.0.4`. The Android and iOS
  version fallbacks and the example news item name 10.0.4.
- What's New: `ui/windows/kn-versions.txt` leads with UPDATE 10.0.4 (CI's shipped version,
  `tools/release/version-meta.sh`), and the What's New tile shows the newest entry.

## What it does not change

- The HELLO / INIT ruleset bytes (HELLO bit 2, INIT ruleset 3) and KN's own packet layouts (wire
  compatibility). `changelog-10.0.3.md` lists what 10.0.3 changed; the 10.0.4 section above lists
  10.0.4's.
- `doc/ref/krunker-v10.0.2/` and `doc/ref/krunker-v10/` are captures of the 10.0.2 page. The
  uidiff scenarios refer to them by path.
- `ui/` is the v10.0.2 page import, edited by hand for parity since. It is regenerated from a newer
  bundle only by a separate `make ui-import` pass with a pixel diff; `ui/windows/versions.txt`
  (Krunker's served changelog) comes from that pass too.
- The filenames in this directory are unchanged. The spec files describe the 10.0.2 bundle unless
  a section says otherwise; `gameplay-1003-audit.md`, `constants-1003.md`, `rules-1003.md` and
  `Krunker_v10.0.3/deob/audit-2026-10-24/` re-check their values against 10.0.3.
  `changelog-10.0.3.md` is the 10.0.2 to 10.0.3 review.
