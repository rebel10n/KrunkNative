# Skin thumbnails in /data/skins (Krunker v10.0.3)

The official 10.0.3 `/data/skins` table gives every skin a `thumbnail`: 8069 of 8069 records.
KrunkNative's table (assets/skins.json) has none, so until this change `/data/skins` carried a
thumbnail only on the three video skins sent as stills (v10_skin_still.c). This note records the
rule the 10.0.3 client applies, where it is in the bundle, and how the brain now serves it
(services/brain/v10_skin_thumb.c, called by h_data_skins in v10_api.c).

## The rule

Source: the decoded bundle `~/.cache/kn-v103/deob-10.0.3.js` (a webcrack unminify of
`~/jails/kru/src/Krunker_v10.0.3/game.js`). Offsets are byte offsets in the decoded file.
game.js itself is minified: the folder names are in its string table (for example `'weapons/weapon_'`
at game.js:782959, `'paints/paint_'` at game.js:264433, `'playercards/card_'` at game.js:722984), but
the builder is obfuscated, so the decoded copy is the readable form.

| What | Where (decoded bundle) |
| --- | --- |
| item folders, indexed by item type (19 entries) | 4794545: `var iíiîïìï = ["weapons/weapon_", "hats/hat_", "body/body_", "melee/melee_", "sprays/", "dyes/", ... "paints/paint_"]` |
| path builder `iîïïìiì(item, folders)` | 1051172 |
| asset-host wrapper `iìiïíîï(path)`: drops the leading `/`, prefixes `https://assets.krunker.io/` (or `./` for local) | 1044677 |
| store loop: `texExt = "webp"` for types 16 and 17 | 4804080 |
| store loop: `item.thumbnail = iîïïìiì(item, folders)`, unconditional for every store record | 4804872 (call at 4804884) |

The path builder, written out (`P` is the path inside `/textures/`):

```
path = "/textures/" + P + "." + (prevExt || texExt || "png")

P = "previews/" + cosmetics/<assetType||type>_<id>[ _c<pat> | _<tex> ]          if type is truthy and not 3
  = "previews/" + folder[assetType||type||0] + tail                           otherwise
      tail (type 3)      = <id>[ _c<pat> | _<tex> ]                           (tex != null)
      tail (the rest)    = <weapon||0>_ + ( m<mid>[_<midT>]  if mid != null
                                           | c<pat>          if pat != null
                                           | <tex>           if tex is truthy
                                           | <id>            otherwise )
P = folder[type] + <id>                                                       type 4, 16, 17 or 18
```

- A missing value reads as JavaScript's `undefined` (its text `undefined`), a JSON null as `null`.
- Types 16 and 17 take `texExt = "webp"` before the path is built (4804080), so they end in `.webp`
  unless `prevExt` says otherwise.
- The store loop sets the thumbnail after `applyShopMutations` has processed the record (templates,
  `assetType` for BACK and HEAD, `texExt`), so the rule runs on the processed record. assets/skins.json
  is that processed table (tools/ui-import/v10_store.js, which also drops the server's own thumbnail).

Examples (store index: record, path):

| index | record | path |
| --- | --- | --- |
| 0 | Arctic Hunt, weapon 1, id 0 | `/textures/previews/weapons/weapon_1_0.png` |
| 445 | Intervention VI, weapon 1, mid 0 | `/textures/previews/weapons/weapon_1_m0.png` |
| 311 | x0n-voX, weapon 4, pat 0 | `/textures/previews/weapons/weapon_4_c0.png` |
| 111 | Top Hat, type 1, id 1 | `/textures/previews/cosmetics/1_1.png` |
| 412 | Hollow Fade, type 3, id 0, tex 2 | `/textures/previews/melee/melee_0_2.png` |
| 4211 | Mysterious Orb, type 3, id 20, tex 3 | `/textures/previews/melee/melee_20_3.png` |
| 4208 | Deep Space, type 5, id 21 | `/textures/previews/cosmetics/5_21.png` |
| 246 | Demonic Wings, type 14, assetType 2 | `/textures/previews/cosmetics/2_0.png` |
| 5904 | Kanji, type 16, id 0 | `/textures/playercards/card_0.webp` |
| 7447 | Love, type 17, id 0 | `/textures/emoji/emoji_0.webp` |
| 7776 | Red Paint, type 18, id `red` | `/textures/paints/paint_red.webp` |

The brain writes the path under `/textures/` (no host), as v10_skin_still.c already did. The client
prefixes `https://assets.krunker.io` (1044677); in KrunkNative the path resolves in the asset tree.

## Agreement with the official table

- Decoding `data/skins.jspck` of 10.0.3 gives 8069 records, all with a `thumbnail`.
- The rule, run on the processed records, equals the official thumbnail (host removed) for 8067 records.
- Two records differ in extension only: store 7794 "KrunkCup Jerseys" (type 2, tex 40, prevExt `webp`)
  and store 7803 "KrunkCup Participation Band" (type 11, id 7, prevExt `webp`). The official value
  ends `.png`; the rule, as the client's store loop computes it, ends `.webp`. The asset tree holds
  only the `.webp` files (`previews/cosmetics/2_190_40.webp`, `previews/cosmetics/11_7_1.webp`), so the
  brain serves `.webp`.

## Before and after

- `/data/skins` rows with a `thumbnail`: before 3 of 8069 (the video stills, store 4208, 4211, 7446);
  after 8069 of 8069, each equal to the rule's path for its type.

## Does the path resolve?

Checked against the installed asset tree (`assets/textures/` of the main checkout, 19061 files) and
against `tools/ui-import/live_assets_v10.lock`:

- 8069 records give 8067 distinct paths (two paths are shared: `weapon_19_100` by stores 6552 and 6883,
  `weapon_6_m0_347` by 7416 and 7728).
- All 8067 distinct paths exist in the asset tree, none is zero bytes, and 1587 of them are also listed
  in `live_assets_v10.lock`. 134 are `.webp`.
- Nothing is missing, so no file was invented.
- Not checked: the live asset host. Per the repository rules, no request went to production.

The worktree this change was made in has no installed assets (`assets/textures` is installed by
the importers), so the check used the main checkout's installed tree.

## Notes

- The native client does not read `thumbnail`. Its previews come from `viewer_skin_preview_path`
  (client/src/viewer/viewer_store.c, a port of `UTILS.getPreview`), which this change leaves alone.
- The rule is the store loop's. The client's other code paths that build a default preview for
  records without one (for example 5936245 and 12899619 in the decoded bundle) are fallbacks that the
  store loop never reaches for a store record.

## Tests

- `services/brain/tests/test_v10_skin_thumb.c` (ctest `brain_skin_thumb`): 23 pinned records covering each
  branch, with their official paths (the KrunkCup Jerseys row is the `.webp` case above); then the whole
  assets/skins.json through the still pass and the thumbnail pass: 8069 of 8069 records carry a
  thumbnail, each equal to its rule path.
- `services/brain/tests/v10_api_data_test.py` (ctest `brain_v10_api_data`): the served `/data/skins`
  rows are compared with a Python port of the rule, and a check counts the rows with a thumbnail equal
  to the rule's path. Before the change: 3 of 8069; after: 8069 of 8069.
- `services/brain/tests/test_v10_skin_still.c` (ctest `brain_skin_still`): the still pass leaves
  `thumbnail` alone.
