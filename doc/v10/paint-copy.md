# Per-copy paint (Krunker v10.0.3)

An owned skin can be painted. The paint belongs to that copy: a painted copy keeps its paint, a second copy of the
same skin does not get it. This file is the official rule (read from the 10.0.3 bundle), what KrunkNative does with it,
and what is not verified.

## Sources and citations

- Readable copy: `~/.cache/kn-v103/deob-10.0.3.js` (the bundle with its string table decoded; the identifiers are still
  minified). `file:LINE (byte OFFSET)` below is that file: LINE is the line, OFFSET the byte offset of the line's start.
- Minified original: `/home/user/jails/kru/src/Krunker_v10.0.3/game.js`. Its strings are hex-indexed lookups, so the
  readable anchors are the decoded lines. Byte offset of the paint API definition there: `game.js@2560926`
  (`'setPaint':function(…){…'/items/'…'/paint'…'POST'…}`).
- The bundle is read-only. Nothing here comes from a live capture (see "Not verified").

## The official rule

### 1. The owned entry (the account's `skins`, data slot 10)

`$account.skins` holds one entry per owned unit or stack:

- a stack: `{ind, cnt, st}` (`addSkin` / `setSkin`, deob-10.0.3.js:129116-129120, 129149-129153);
- a copy: `{ind, cnt: 1, id, m, st: 0}`, `id` the copy's id, `m` its metadata (deob:129099-129105, 129133-129139).

`m` is read through a normaliser that takes an object or a JSON string (deob:128883-128894, and on load deob:129081-129083).
Account data is set at deob:128914 and deob:128977 (`this.skins = data[10]`).

The paint lives in `m`: `m.paint` is the colour `"#rrggbb"` and `m.paintShader` is a shader number. `m.seed` is optional;
without it the seed is a hash of the copy id (deob:27829-27839).

### 2. Which paint a copy shows

- Metadata of an item (`metadata` prop, string or object): deob:124567-124583. Painted when `paint` is set or
  `paintShader != null` (deob:124584). The paint is the store paint of type 18 with that colour (case-folded,
  deob:124594) or with that `paintShader` (deob:124590). The name is the store paint's name, else `Shader #n`
  (deob:123203-123210). This is the paint badge rule that `client/src/ui/skin_paint.c` ports.
- The item viewer's `instance` prop (deob:123127) gives `paint`, `paintShader`, `wear` and `seed` for the viewer URL
  (deob:123160-123173); the item card passes its `instance` on to the viewer (deob:253644).

### 3. The owned-paint list

The paint picker builds its list from `$account.skins` (deob:122013-122042): for each owned entry of a type-18 store
item it counts `cnt ?? 1` per `ind` (deob:122014-122019); a paint qualifies when it has `paintColor` or a numeric
`paintShader` (deob:122029-122035). Sorted: owned first, then by index (deob:122041).

### 4. Applying a paint

- The call: `items.setPaint(skinId, {item_id, paint_item_id})` (deob:122139-122142; the route is
  `POST /items/{skinId}/paint`, deob:30996-30999). `item_id` is the store index of the skin being painted and
  `paint_item_id` the paint's store index.
- `skinId` is the owned copy's `sid` (deob:149066 for an inventory row). The apply is refused without it
  (deob:122132: `skinId != null`) and without a paint unit (deob:122133-122134: "You don’t have any of that paint
  left.").
- On success the client takes one unit from its own paint stack (deob:122152-122170), calls `onApplied(colour)` and
  shows "Painted <item> with <paint>" (deob:149073-149077). It then reloads the inventory from the server
  (deob:149054-149060, `market.getInventory()` at deob:122564-122571). The copy's new `m` comes from that reload,
  not from the client.

### 5. The market inventory rows

`GET /market/inventory` (deob:30863-30864, 122564-122571) is read as the chooser's list, keeping rows with
`cnt === undefined || cnt > 0` (deob:149209, 149970): a copy has no `cnt`, a stack does. A copy row carries `sid` (its
copy id: deob:149016, 149066, 124751, 150107) and `m` (a trade row copies it: deob:125069-125075, `{i, c: 1, sid, m}`).

### 6. Loadout and spawn

- A loadout slot is the string `"<copyId>#<index>"` when the owned entry has an `id`, else the plain index
  (`iïíïïïï`, deob:241503-241508; `iìîìïïí`, deob:27876-27880; `selectSkin` stores it, deob:245684-245687).
- The server side splits the slot: `skinId` = the copy id, `skinIndex` = the store index (deob:27857-27875; the `#`
  split at deob:27812-27815).
- The local match finds the owned copy by that id, or, for a plain index, the first copy of that index with `m`
  (deob:252090-252095). Its `m` (parsed) is the metadata of the slot (deob:194014-194035), and the
  weapon model reads it through `getSkinForLoadout` (deob:194690-194691).
- The player model reads each cosmetic slot as an index or an `{i, m}` object: `iíiíííì` gives the index and `iíiïîîí`
  (deob:27818-27820) gives `m` of an object slot. Where a slot is a plain index, the model uses the player's
  `xMetadata` field instead (deob:194014-194035). This is the same code for every player, ours included.

## What KrunkNative does

KrunkNative keeps every owned unit in `inventory(account_id, skin_id, count)`. The painted units are a second table:

- `skin_copies` (migration 0352, sqlite / postgres / mysql): one row per painted unit of (account, skin), with
  `copy_id` (identity from 1000000), `paint_color` or `paint_shader` (exactly one, CHECK), `painted_at`.
- `inventory.count` still counts every owned unit, painted or not, so ownership checks do not change. The unpainted
  units are `count - painted rows`.
- Copy ids start at 1000000 (`V10_COPY_ID_BASE`, `services/brain/v10_paint.h`). A stack's `sid` is its store index
  (below 1000000), so a `sid` never means two things.
- `db_add_skin` (and the postgres `kn_add_skin`) refuse to take away a painted unit: a sale, listing, trade, quick sell
  or craft takes only unpainted units (`count + delta >= painted`). The refusal says "Only painted copies of that item
  are left".

### Routes

- `POST /items/{id}/paint` (`services/brain/v10_market.c`, `h_item_paint`), body `{item_id, paint_item_id}`, login
  required:
  - `{id}` is a copy id (of the caller, of that item) or the stack's store index (an unpainted unit of the item becomes
    a new copy);
  - the paint must be a type-18 store paint (`v10_paint_of`), else 400 `ITEM_PAINT_INVALID` "That item is not a paint.";
  - an unknown item is 404 `NOT_FOUND`; another account's copy is 400 "You don't own that copy.";
  - the reply is `{copy_id, item_id, paint_item_id, m, count}` (`count`: the paints left).
  - The work is one transaction: `sh_item_paint` (`economy.c`) → `P_ITEM_PAINT` / `run_item_paint` (`db_economy.c`).
    The paint unit is consumed through `db_add_skin` (its audit row, source `paint`).
- `GET /market/inventory` (`v10_market.c`, `reply_inventory`): stacks as `{i, cnt, sid}` (cnt = the unpainted units,
  sid = the store index), then one `{i, sid, m}` row per painted copy (no `cnt`), with `m` = `{paint}` or `{paintShader}`.
  The rows come from the market page's inventory query (`economy.c`, `market_done`), which also feeds the socket.
- The owned list (account data slot 10, `db_owned_skins` in `db_common.c`, every driver): `{ind, cnt}` per skin with
  unpainted units (cnt = those units), then `{ind, cnt: 1, id, m}` per painted copy. For postgres the list replaces
  the SQL function's slot 10.

### The bridge

The v10 bridge (`net-bridge/v10Translate.ts`, `v10Proxy.ts`) keeps the paint of our own loadout's `{i, m}` slots
(`loadoutPaintOf`, `EV.loadoutPaint`) and puts it on our own spawn's add record (`buildPlayerAdd`, weapon slots [12] and
[19]): `{i, m}` instead of the plain index. The native ENT and SKINS packets carry only indices, so the paint has to be
kept on the bridge side.

## Gaps (on purpose, or not yet)

- Painted copies cannot be listed, offered in a trade, quick sold or crafted in this change: a copy id is not a stack, and
  the paths that take a stack refuse it (the stack has no unpainted unit left). The official listing sends the copy id
  as `skin_id`; KrunkNative does not map it to a copy (not tested).
- A copy id in a loadout (`"<copyId>#<index>"`) keeps only the index in the bridge. The paint of a copy is the client's own
  one (its `$account.skins`, section 6), so the local model is painted. Another player's spawn is unpainted: the native
  SKINS packet has no paint field. That needs a `NET_PACKET_EXT` subtype and is not built.
- `paintTargets` (which items accept a paint) is not checked by the brain; the client offers only paintable items.
- The style route (`POST /items/{id}/style`) is still a refusal.

## Not verified

- No live capture of the official owned list. The shape of a copy in `$account.skins` (`id`, `m`, `cnt`) and of the
  market rows (`sid`, `m`) comes from the bundle reading above. A capture needs a painted copy on a production account,
  which this change does not touch. The local playable check runs a guest round against KrunkNative's own server, so it
  does not capture the paint list (see the report).
- Postgres and MySQL: not run. The migrations (0352 on both) and the SQL in `db_economy.c` / `db_common.c` / `economy.c`
  were read against the dialect rules, not executed. A PostgreSQL server answers on 127.0.0.1:5432, but its
  `krunknative` role needs a password that the test run had no access to; no MySQL server listens on 3306. The
  postgres `kn_add_skin` guard was not executed. Its refusal says "Item not owned" where the C path says "Only painted
  copies of that item are left" (a message difference, not a count difference).
- `services/brain/tests/paint_e2e_test.py` (`brain_paint_e2e`) runs on sqlite only.

## Tests

- `services/brain/tests/v10_api_market_test.py` (`paint_scenario`): the refusals (non-paint, unknown item, guest, another
  account's copy), an unpainted unit becoming a copy painted red, a second copy painted shader 0, a repaint of the first
  copy (its paint changes, the other copy keeps its own, the stack keeps its count), the last unpainted unit, no unpainted
  unit left, and a destroy-all that takes none of the three painted units. Run against origin/main's brain, the same
  checks fail (403 NOT_IMPLEMENTED and no copy rows).
- `net-bridge/v10Proxy.test.ts`: a `{i, m}` weapon slot keeps its paint in our spawn's add record, a copy-id slot and a
  plain index keep none. Run against origin/main's bridge sources, it fails (`T.loadoutPaintOf is not a function`).
