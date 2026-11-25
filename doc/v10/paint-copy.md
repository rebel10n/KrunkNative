# Per-copy paint (Krunker v10.0.3)

An owned skin can be painted. The paint belongs to that copy: a painted copy keeps its paint, a second copy of the
same skin does not get it. This file is the official rule (read from the 10.0.3 bundle), what KrunkNative does with it,
and what is not verified.

## Sources and citations

- Readable copy: `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (the bundle with its string table decoded; the identifiers are still
  minified). `file:LINE (byte OFFSET)` below is that file: LINE is the line, OFFSET the byte offset of the line's start.
- Minified original: `~/jails/kru/src/Krunker_v10.0.3/game.js`. Its strings are hex-indexed lookups, so the
  readable anchors are the decoded lines. Byte offset of the paint API definition there: `game.js@2560926`
  (`'setPaint':function(…){…'/items/'…'/paint'…'POST'…}`).
- The bundle is read-only. Nothing here comes from a live capture (see "Not verified").

## The official rule

### 1. The owned entry (the account's `skins`, data slot 10)

`$account.skins` holds one entry per owned unit or stack:

- a stack: `{ind, cnt, st: 1}` (`addSkin` / `setSkin`, stack branches at deob-10.0.3.js:129116-129120, 129149-129153);
- a copy: `{ind, cnt: 1, id, m, st: 0}`, `id` the copy's id, `m` its metadata (copy branches at deob:129099-129105,
  129133-129139).
- `st` is the stack flag: `st === 1` is a stack, any other value a copy. The trade chooser and the item card test it
  (deob:124726 `iIííiíì`, 125012 `iÎiiiïî`, card at 124760-124771): a copy is keyed by its `sid` and `iids`, a stack by
  its `cnt`.

`m` is read through the normaliser `iìiíïîï` (deob:128883-128894: an object is kept, a string is parsed), on load
at deob:129081-129083. The server rows map `{i, c, id, m, st}` to `{ind, cnt, id, m, st}` (deob:28690-28700).
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
`cnt === undefined || cnt > 0` (deob:149209, 149970): a copy has no `cnt`, a stack does. Each row carries `st` (1 a stack,
0 a copy; deob:124726). A copy row carries `sid` (its
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

### 7. Listing, trading, quick selling and crafting a painted copy

The official client keeps a painted copy as its own unit in each of these four paths. It never refuses a painted copy
(no paint refusal exists in the bundle: the error keys of the market and the trade, deob:117347-117359, and the craft
messages have none). What it sends:

- **List.** The listing function takes `skin_id` and sends it on `/market/list` (`iîïíiíí`, deob:122583 byte 5136305;
  the `skin_id` is set at deob:122589 byte 5136499; `listItem`, deob:122591 byte 5136547). The caller passes the
  selected row's `sid` (deob:151563-151564, byte 7050366 and 7050521), so a painted copy lists as its copy id and a
  stack lists with no `skin_id` (the stack's row, deob:149123). The refusal the client shows for a unit that cannot be
  sold is the `noSale` / opensea / free / blackM / itemShop test (deob:122670 byte 5139171), not paint; the message is
  "This item cannot be sold" (deob:151554 byte 7049972).
- **Trade.** A stack row is `{i, c}` when `st === 1` (deob:125011-125012, byte 5305369). Any other row is a copy: the
  offer gets `{i, c: 1, iids: [sid], sid, m}` (deob:125069-125072, byte 5307757). The offer serializer keeps only
  `{i, c, iids}` (deob:125109-125113, byte 5309491), and the trade body goes out at deob:125149 (byte 5310757). The
  trade picker shows the copy's paint (`trade-item-paint`, deob:124685, byte 5279363).
- **Quick sell.** `quickSellItem({item_id, action, amount}, skin_id?)` (deob:147675, byte 6792946). `skin_id` is the
  unit's `instanceId` only for action 0 (one item): `IÏîîiìì = action === 0 && instanceId` (deob:147673, byte
  6792725). The dialog offers only "1 Item" when the count is 1 (deob:147652, byte 6791805). After a copy sale the
  client drops that copy by its id, and a stack sale drops the first stack-count copies (deob:147703 byte 6794196,
  deob:147715 byte 6794786, deob:147722 byte 6795131). The KR of a unit is the rarity's value, whatever its paint
  (the value function at deob:122598 byte 5136822 reads the rarity and the skin's flags only).
- **Craft.** The craft is the socket frame `crft` with the recipe SID (`startCraft`, deob:249930 byte 12514916;
  `sendCraft`, deob:249935 byte 12515107; `CRAFT_REQUEST` is `"crft"`, deob:110986 byte 4642898). The client checks each
  material against its owned rows: `find(ind == X && cnt >= n)` (deob:153201 byte 7143790). A copy row has `cnt: 1`, so
  a copy satisfies a one-unit material, and the check is per row (a two-unit material needs a stack row with two). The
  server side of the craft is not in the bundle.

So a painted copy is tradable, listable, quick-sellable and a craft material; the paint goes with the copy wherever
it goes, and is gone when the copy is consumed.

## Pinned lines (byte offsets)

Byte offsets are the start of the line in `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (`head -n LINE-1 | wc -c`).

| line | byte | what it is |
|---|---|---|
| 128883 | 5580350 | `m` normaliser `iìiíïîï`: an object as is, a string parsed |
| 128914 | 5581541 | `this.skins = iÍííìíí.skins` (account load) |
| 128977 | 5584975 | `this.skins = data[10]` (account load) |
| 129070 | 5588914 | account skins setter: the `{i, c, id, m, st}` rows |
| 129095 | 5589760 | `addSkin` (key) |
| 129099 | 5589938 | `addSkin` copy: `{ind, cnt: 1, id, m, st: 0}` |
| 129116 | 5590530 | `addSkin` stack: `{ind, cnt, st: 1}` |
| 129124 | 5590685 | `setSkin` (key) |
| 129133 | 5591056 | `setSkin` copy: `{ind, cnt: 1, id, m, st: 0}` |
| 129149 | 5591598 | `setSkin` stack: `{ind, cnt, st: 1}` |
| 122013 | 5102715 | picker: the owned count map from `$account.skins` (`cnt ?? 1`, line 122019) |
| 122029 | 5103479 | picker: a paint has `paintColor` or a numeric `paintShader` |
| 122041 | 5103983 | picker: sort, owned first, then by index |
| 122132 | 5107674 | apply: refused without `skinId` |
| 122139 | 5108046 | apply: `items.setPaint(skinId, {item_id, paint_item_id})` |
| 122152 | 5108656 | apply: one unit taken from the local paint stack |
| 122564 | 5135633 | `iiíïïiï()`: `market.getInventory()` (the reload) |
| 124567 | 5273391 | item metadata: painted when `paint` / `paintShader` is set |
| 124726 | 5285099 | `st === 1` (stack test) |
| 125012 | 5305369 | `st === 1` (trade chooser stack test) |
| 149066 | 6901818 | the picker's `skinId: sid` (an inventory row) |
| 149073 | 6902208 | "Painted <item> with <paint>" toast on success |
| 149209 | 6908785 | the chooser's `cnt === undefined \|\| cnt > 0` filter |
| 30863 | 1177086 | `market.getInventory` route |
| 30996 | 1182913 | `items.setPaint` route: `POST /items/{id}/paint`, body JSON |

## What KrunkNative does

KrunkNative keeps every owned unit in `inventory(account_id, skin_id, count)`. The painted units are a second table:

- `skin_copies` (migration 0352, sqlite / postgres / mysql): one row per painted unit of (account, skin), with
  `copy_id` (identity from 1000000), `paint_color` or `paint_shader` (at most one since 0398, CHECK), `painted_at`, and since 0398 `wear` and `serial` (below).
- `inventory.count` still counts every owned unit, painted or not, so ownership checks do not change. The unpainted
  units are `count - painted rows`.
- Copy ids start at 1000000 (`V10_COPY_ID_BASE`, `services/brain/v10_paint.h`). A stack's `sid` is its store index
  (below 1000000), so a `sid` never means two things.
- `db_add_skin` (and the postgres `kn_add_skin`) refuse to take away a painted unit (`count + delta >= painted`). A
  stack's sale, listing, trade or quick sell takes only its unpainted units; the refusal says "Only painted copies of
  that item are left". A painted copy goes by its copy id instead (db_common.c): `copy_take` deletes its skin_copies row
  and takes the unit with `db_add_skin` (so the audit row is written), `copy_give` adds a unit as a new copy with a
  paint. A craft takes the unpainted units first and then the copies (`db_take_units`), so a copy's paint goes with it
  when it is used up. The copy's paint is kept on the listing (`market_listings.paint_*`, migration 0353) and on the
  offer (`trade_copies`, side `s`: the escrowed copy's paint), and each returns with the copy.
- Minted copies (migration 0398, v10.0.7's item instances): a store spin mints each `wearable` item (skins.json
  `wearable`, v10's `itemConfig.wearable`) as a copy with a wear (`skin_copies.wear`, uniform in [0, 1): the
  Flawless < 0.1 / Tested < 0.3 / Trashed < 0.6 / Wrecked tiers, deob-10.0.7.js:16478-16494) and the item's next serial
  (`skin_copies.serial`, counted per item in `item_mints`, so a consumed copy's serial is never reused). A copy needs
  no paint any more (the CHECK is "at most one paint"); a minted copy can be painted later and keeps its wear and serial.
  `m` is `{paint | paintShader, wear, serial}`; the listing (`market_listings.wear / serial`) and the offer
  (`trade_copies.wear / serial`) keep them as they keep the paint (`CopyPaint`, `copy_meta_*` in db_common.c;
  `v10_copy_meta_row` in v10_paint.c). The spin answers the instances (`["spin", ..., usedTicket, instances]` on the
  social socket, `data.instances` on `POST /spins/execute`; deob-10.0.7.js:224680-224697), and the client adds each as
  v10's `addSkin(index, 1, id, m)` and draws its wear line and serial tag on the spin screen.
- Name tags (migration 0401): `skin_copies.name_tag` (1 to 20 characters, CHECK), kept on the listing
  (`market_listings.name_tag`) and the offer (`trade_copies.name_tag`) as the wear and serial are. `m.name` is v10.0.7's
  instance name (the card's `.itemNametag`, deob-10.0.7.js:110688-110693; the HUD's `.weapNametag`, :176440). v10.0.7 has
  no client route that names a copy, so KrunkNative names one through an operator route (below; a KN addition). The
  rule for a name is `copy_name_tag_valid` (db_procs.h): trimmed, no control characters and none of `"'<>\` or the
  backquote. A copy's item style does not travel: the styled items (KrunkCup, 0393) are not tradable, and `copy_take`
  deletes the copy's `item_styles` row.
- In a match: the server sends each worn copy's `{skin, wear, serial, name}` to every client in
  `NET_EXT_KN_SKIN_META` (0x81C1, `accounts_skin_meta_send` in `server/src/accounts.c`, after each SKINS) from the
  owner's copy of that index (the plain-index rule, as for the paint). A client reads it with `skin_meta_of(uid, skin,
  &meta)` / `skin_meta_wear(uid, skin)` (`client/include/skin_meta.h`). v10.0.7 sends the instance in the player's add
  data (`{i, id, m}`, deob-10.0.7.js:24335) and draws the wear map (:173646-173651) and the name tag (:176440).
- The end table's skin reward: v10.0.7's `endGame` gives a skin with the KR reward (deob-10.0.7.js:195123-195160) and the
  end table's reward cell draws it as a `.skinReward` thumbnail that opens its sales (:226765-226770). KrunkNative's
  server sends it before END in `NET_EXT_KN_END_REWARD` (0x81C0, `{uid, kr, skin}`, `accounts_skin_reward`), and the
  reward cell draws it (`hud_ui_end_reward_cell`).

### Routes

- `POST /items/{id}/paint` (`services/brain/v10_market.c`, `h_item_paint`), body `{item_id, paint_item_id}`, login
  required:
  - `{id}` is a copy id (of the caller, of that item) or the stack's store index (an unpainted unit of the item becomes
    a new copy);
  - the paint must be a type-18 store paint (`v10_paint_of`), else 400 `ITEM_PAINT_INVALID` "That item is not a paint.";
  - the item must have `paintTargets` (true or a list of mesh names, `SkinInfo.paintable` in store.c), else 400
    `ITEM_PAINT_INVALID` "That item cannot be painted." (f1eb3de1). The official server's text is not in the bundle;
    the socket's `ip` gets the same text as a `po` refusal;
  - an unknown item is 404 `NOT_FOUND`; another account's copy is 400 "You don't own that copy.";
  - the reply is `{copy_id, item_id, paint_item_id, m, count}` (`count`: the paints left).
  - The work is one transaction: `sh_item_paint` (`economy.c`) → `P_ITEM_PAINT` / `run_item_paint` (`db_economy.c`).
    The paint unit is consumed through `db_add_skin` (its audit row, source `paint`).
- The social socket's `ip` frame (`social_ws.c` table, `sh_item_paint` in `economy.c`; the gapi socket has no `/items/`
  route): `["ip", skinId, item, paint, colour, shader, uid]` -> `["ipr", {copy_id, item_id, paint_item_id, m, count} | null,
  uid]`, or `["po", null, message]` for a refusal (the native paint picker's apply, `paint_picker.c`). Same work and
  refusals as the HTTP route; the email gate counts it as a market write (`account_security.c`, F_MARKET).
- `GET /items/{id}/instances?sort=mint|wear` (`v10_market.c`, `h_item_instances`; v10.0.7's `getInstances`,
  deob-10.0.7.js:27078-27082): the item's copies, owned and listed, as `[{nm, wear, serial}]` (`nm` the owner's
  username), by serial or by wear (copies without a wear left out), at most 100. The item view's `stackable` is false and
  `wearable` true for a wearable item, which shows the item page's Mints and Wear tabs (:109727-109733).
- `POST /mm/admin/items/{copy}/nametag` `{name}` (operator only: `mm_internal_authorized`, the internal secret): sets the
  copy's name tag, or clears it with `name: null` / `""`. Replies `{copy_id, name}`; 400 `NAMETAG_INVALID`, 404
  `ITEMS_INVALID_ID`, 401 without the secret (`P_COPY_NAMETAG` / `run_copy_nametag`, db_economy.c).
- `GET /market/inventory` (`v10_market.c`, `reply_inventory`): stacks as `{i, cnt, sid, st: 1}` (cnt = the unpainted
  units, sid = the store index), then one `{i, sid, m, st: 0}` row per painted copy (no `cnt`), with `m` = `{paint}` or
  `{paintShader}`.
  The rows come from the market page's inventory query (`economy.c`, `market_done`), which also feeds the socket.
- `POST /market/list` with `skin_id` = a copy id lists that copy (the socket "li" frame's third value; `run_market_list`).
  `POST /market/quick-sell` with `skin_id` = a copy id sells that copy as one unit (`action` 0, `amount` 0; the socket
  "qsi" frame's ext `copy`; `run_quick_sell`). The stack's `amount` counts its unpainted units.
- `POST /trades/create`: an offer's copy is `{i, c: 1, iids: [copy id]}`, the official shape (game.js 125069-125113);
  a stack is `{i, c}`. `offer_side` (economy.c) turns each copy into its own `[skin, 1, copy id]` entry;
  `run_trade_create` takes the sender's copies into escrow (their paint kept in `trade_copies`) and checks the
  receiver's copies exist. Accept moves them (the receiver's paint read from its copy row); cancel, decline and expiry
  return the sender's with their paint (`trade_unwind`). The trade rows carry the copy units as `{ind, sid, m}` in
  `sksell` / `skbuy` (`add_copy_units`, v10_market.c).
- `GET /trades/inventory/{id}`: a painted copy is its own row, `{i, c: 1, sid: copy id, m}`; a stack is `{i, c}`.
- `POST /market/unlist` and `POST /market/purchase` give a painted listing back (or to the buyer) as a copy with its
  paint. The copy id is new; the paint is the same.
- The owned list (account data slot 10, `db_owned_skins` in `db_common.c`, every driver): `{ind, cnt, st: 1}` per skin
  with unpainted units (cnt = those units), then `{ind, cnt: 1, id, m, st: 0}` per painted copy. For postgres the
  list replaces the SQL function's slot 10.
- The socket's `market` inventory page (`market_done` / `shape_inventory` in `economy.c`) returns the same rows without
  the SQL's paint columns: a stack `{i, s, c, qs}`, a painted copy `{i, s, c: 1, sid, m, qs}`.

### Loadout copies (v10.0.7 `"<copyId>#<index>"` slots)

v10.0.7 (deob-10.0.7.js line numbers):

- A slot picked from an owned card with an `id` is `"<copyId>#<index>"` (`iîìîiìï`, :24381-24383). The weapon card calls
  `selectSkin(ind, weapon - 1, undefined, id)` (:220896) and `selectSkin` stores `iîìîiìï(id, ind)` (:217812-217830); the
  item cards call `fn("<id>#<ind>")` (:223502) and `selectHat` & co. save the raw value (`iïîîíïí`, :217270-217272,
  :217305-217465). Random returns the picked entry's `"<id>#<ind>"` (`iiïíïíí`, :217797-217812; favourites by
  `isFav`, :214623-214625). Presets (`krk_loadouts`) keep the raw values (:218607). `ENTER_GAME` sends them as
  they are (:215052).
- `iïiíiïí` splits a slot into `skinId` / `skinIndex` (:24362-24380); `iiîiîîî` gives the index of a string or an `{i, m}`
  object (:24322-24334).
- The class preview (:217860-217950): a weapon slot is `{i, m, id}` when it is `"<id>#<ind>"` and the account entry with
  that id has an `m`, else the plain index; a cosmetic's `xMetadata` is the entry's `m` only for `"<id>#<ind>"`; the melee
  seed is `iiiíìiî(slot, m)` (m.seed, else a hash of the id, :24346-24352).
- The Customize tab's slot finds the entry by its id (or, for a plain index, the first entry of the index with an `m`) and
  draws `m.style`'s style preview (:222501-222512).
- `selectCharm` (:217425-217452): both charm slots on one index are fine when both are copies with different ids; the same
  copy twice, or a plain index with no stack of `cnt > 1`, empties the slot and shows "Charm already in use" with
  Override (`overrideCharm(JSON.stringify(value))`) and Ok.

KrunkNative:

- `client/src/ui/ui_loadout.c` keeps each saved slot's copy (`Selected.copy`; the `skins` and `charms` JSON hold the
  `"<id>#<ind>"` string), parses it with `skin_slot_parse` (`client/src/ui/skin_paint.c`), writes it from the owned cards
  (`owned_card`: the weapon card's `selectSkin(ind,weapon,undefined,id)`, the item cards' `fn("<id>#<ind>")`), the
  handlers (`harg_slot`, `ui_handlers.c` `h_select_skin`'s 4th argument), Random, presets and `select_charm` (v10's rule
  and popup). `ui_loadout_look` fills `LoadoutLook.copies` (the copy each routed slot names), and the client sends it as
  `NET_EXT_KN_SKIN_COPY_IDS` (0x81C0, `NetSkinCopyIdsPacket`, 128 bytes: store index and copy id per `SKIN_COPY_SLOT_*`)
  right before every ENT (`client_send_skin_copies`, a server with EXT; the krunker.io link turns it into the "en"
  array's `"<id>#<ind>"` strings, `krunker_link.c` `ent_slot`). The ENT itself keeps the plain indices.
- The local model: the menu preview reads the saved slots (`ui_loadout_worn_copy`), our player in a game the last ENT's
  copies (`ui_loadout_ent_copies`). Paint, style, seed and wear come from that copy's `m` (`ui_loadout_copy_meta` and
  friends; `player_mesh.c` `player_own_copy`); a plain index keeps the plain-index rule (the first copy with a paint /
  style / wear). Another copy of a worn index rebuilds the preview (`class_preview.c`).
- The server (`server/src/net.c`, `accounts.c`): the EXT is kept as `ent_copies`, copied to `worn_copies` at spawn.
  `accounts_load_skins` records every owned entry with an id (`ServerSkinCopy`: paint, style, seed, wear; 512 per
  account). `accounts_worn_copy` gives the copy a slot names when the account still has it and it is a copy of that index;
  else NULL and the plain-index rule applies (a gone copy, another index's copy, an older client that sends no EXT). The
  copy's paint goes on SKINS V2, its seed and wear on `NET_EXT_KN_SKIN_COPIES` V2 (40 bytes: the V1 28 bytes, then
  `float wear[3]`), a cosmetic's style and wear on `NET_EXT_KN_SKIN_STYLES` V2 (104 bytes: the V1 72, then
  `float wear[8]`; a pair with a wear and style 0 is listed too). Readers take both sizes; an older client reads the V1
  prefix.
- Wear on the model (`client/src/skin_fx.c`): the wearMap of a cosmetic (`<type dir><id>_wear`, not for customTextures,
  :173646-173651), of a melee (`melee/melee_<id>_wear`, :174102-174107) and of a weapon skin
  (`weapons/<src>[_<mid>]_wear`, :174184-174189, `skin_fx_weapon_wear_key`) at the copy's `m.wear`, mixed by
  `basic.frag`'s `wear_apply` as v10's `iïìîïiì` (:95713-95730). Another player's levels come from the two V2 packets
  (`skin_counter_wear`, `skin_style_wear_of`). The assets ship no weapon wearMap yet (melee 182-200 and a few cosmetics
  only): a weapon skin's wear is computed and logged (`[wear] ... map=0`) but not drawn.
- The item view (`view_item`, :223653-223690): the account's first entry of the index gives `.itemViewWear` ("<tier>
  (wear.toFixed(4))" and `#serial`), `.itemViewDesc` / `.itemViewDescWear` with the type label (`iìíiiií`,
  :24595-24598), and the viewer URL's `&wear`, `&paint`, `&style`, `&seed` and counter stats (`ui_loadout_paint_query`).
  The owned card's tooltip (`createTip`, :223704-223712) shows a wear and serial only from `data-wear` / `data-serial`,
  which the Loadout cards do not set (only the trade cards, :221824 / :221856 / :221970); `ui_tooltip.c` already does
  the same.

### The bridge

The v10 bridge (`net-bridge/v10Translate.ts`, `v10Proxy.ts`) keeps the paint of our own loadout's `{i, m}` slots
(`loadoutPaintOf`, `EV.loadoutPaint`) and puts it on our own spawn's add record (`buildPlayerAdd`, weapon slots [12] and
[19]): `{i, m}` instead of the plain index. The native ENT carries only indices; the copies ride in
`NET_EXT_KN_SKIN_COPY_IDS` (above). Both bridges (the C `server/bridge/v10_bridge.c` built by `make`, and the TypeScript
`net-bridge/v10Proxy.ts`) read an official client's `"<copyId>#<index>"` slots as v10.0.7's server splits them
(`iïiíiïí`, :24362-24380: `parseInt` of each side of the `#`): the ENT gets the index and
`NET_EXT_KN_SKIN_COPY_IDS` goes right before every ENT with the copy each slot names, as a native client sends it.
The native SKINS packet carries each worn skin's paint from patch 6 (below): the server fills it from the owner's painted
copy of that index (`accounts_skin_paints`, `server/src/accounts.c`), so a native client shows another player's paint.

## Gaps (on purpose, or not yet)

- A copy that is listed, sold, traded or given back gets a new copy id with the same paint. A loadout slot that named the
  old copy id (`"<copyId>#<index>"`) no longer matches the copy; the slot falls back to the index.
- The official craft check is per row (game.js 153201): a copy satisfies a one-unit material, a two-unit material needs
  a stack row with two. KrunkNative counts every owned unit of a material (copies included) and takes the unpainted ones
  first. The server side of the official craft is not in the bundle, so the choice of unit is ours.
- The socket's trade rows (`sksell` / `skbuy` strings in economy.c) list stacks only; the copies are in `copies_s` and
  `copies_r` for the v10 page. The native trade picker keeps each row's copy id and `m` (`trade_ui.c:37`, :188), and
  the List / Quick Sell modal sells a painted copy by its id (`v10_market.c` `sel_sid`, :4705; 3e317c00).
- The listing rows show a painted copy's paint (`m`). The purchase reply's `m` is the bought copy's paint
  (`{paint}` or `{paintShader}`) and `sid` the buyer's copy id (`run_market_buy`, 254af10f).
- The native paint picker (`client/src/ui/paint_picker.c`) is opened from the owned card's paint button in the Loadout
  chooser (`ui_loadout.c` `owned_card`, `paintOpen(ind, sid)`). It lists the owned paints (the owned list, sorted as
  deob:122041) with their counts and applies the picked one over the social socket: the `ip` frame (see Routes), then it
  takes the paint's unit locally (`account_paint_applied`) and the brain's `ua` push replaces the list. The popup is
  v10.0.7's item viewer (deob-10.0.7.js:108483-108814, the iframe src :108675) with the picked paint on the item and
  every store paint listed, owned with its count or locked for preview (e5a46e78, `paint_picker_e2e`). The v10
  button itself sits in the store inventory grid (deob:147316 markup, gated on `paintTargets` and a skinId at
  deob:150226), not in the Loadout chooser; the native button is on the chooser's card, where the owned badge is. A
  krunker.io account gets no button (nothing is sent to krunker.io for a paint, `v10_gapi` refuses writes there).
  The clan banner (`clan_ui.c`, `POST /clans/{id}/banner-color`) is a separate clan route.
- The official client gets each copy a native player wears: the C bridge sends it in the add record as `{i, m}` with
  the paint, wear, seed, serial, name, style and counter (deob-10.0.7.js:176753; 9d9992cc, 30786db4). A native client
  gets the copy each slot names from the server (Loadout copies, above); a
  plain index (or a gone copy) shows the owner's first painted copy of that store index, the plain-index rule
  (deob-10.0.3.js:252090-252095), and its first worn copy's wear. v10.0.7 gives a plain index no `m` at all
  (:217860-217950): KrunkNative keeps the plain-index rule for older saved slots and older clients.
- A counter skin counts from the copy its slot names (deob-10.0.7.js:173031; cfa57653). The count is stored per copy
  since migration 0406 (`skin_stats.copy_id`, a5df8bc8); a plain index reads the first counted entry. The count moves with the copy (deob-10.0.7.js:191932;
  4ed623ad): `copy_take` / `copy_give` carry its `skin_stats` rows through live trades, offers and market list /
  unlist / buy (escrowed in `market_listings.stats` / `trade_copies.stats`, migration 0407), `m` carries the counts,
  and a quick sale or a craft drops them with the copy; a stack's count stays with the seller's stack
  (`brain_economy` copy_counts, `brain_live_trade`).
- A shader paint (`paintShader`) is drawn: `client/src/skin_fx.c:1300-1324` (`client/tests/paint_shader_two_client_e2e.py`,
  not in CTest). The official client gets native players' paint from the bridge's add record (above).
- `paintTargets` is checked by the brain (f1eb3de1, Routes above; `brain_paint_e2e`, `brain_v10_api_market`,
  `brain_economy_sqlite`).

## Not verified

- No live capture of the official owned list. The shape of a copy in `$account.skins` (`id`, `m`, `cnt`) and of the
  market rows (`sid`, `m`) comes from the bundle reading above. A capture needs a painted copy on a production account,
  which this change does not touch. The local playable check runs a guest round against KrunkNative's own server, so it
  does not capture the paint list (see the report).
- Postgres and MySQL: not run. The migrations (0352 and 0353 on both) and the SQL in `db_economy.c` / `db_common.c` / `economy.c`
  were read against the dialect rules, not executed. A PostgreSQL server answers on 127.0.0.1:5432, but its
  `krunknative` role needs a password that the test run had no access to; no MySQL server listens on 3306. The
  postgres `kn_add_skin` guard was not executed. Its refusal says "Item not owned" where the C path says "Only painted
  copies of that item are left" (a message difference, not a count difference).
- `services/brain/tests/paint_e2e_test.py` (`brain_paint_e2e`) runs on sqlite only.
- The official server's answers to a listing, offer, quick sell or craft of a painted copy are not in the bundle, nor a
  capture of them. Section 7 is the client's side only; the server's choice of unit for a painted craft material is ours.

## Tests

- `services/brain/tests/v10_api_market_test.py` (`copy_meta_scenario`): a named, worn copy of 7582 on the market
  inventory, the owned list and the listing (and back on unlist), the instances route by mint and by wear, the view's
  `stackable` / `wearable`, and the name tag route (set, clear, refusals, no secret).
- `server/tests/kill_pools_server_e2e.py` (checks 5 and 6): the end reward before END on every peer, and the worn copy's
  `NET_EXT_KN_SKIN_META` on the other peer and on its owner.
- `client/tests/market_copies_e2e.py`: the copy card's name tag and wear line (inventory and browse), the listing row's
  spans, the copy overlay of the item page, and the Mints / Wear tabs.
- `client/tests/player_rank_flag_e2e.py` (`endreward`): the end table's `.skinReward` cell.
- `services/brain/tests/v10_api_market_test.py` (`paint_trade_scenario`): a painted copy listed by its copy id (the
  listing keeps the paint; the stack lists only its unpainted units, and a further stack listing is refused), the
  listing's `m` on `/market/sales`, the buy (the buyer's copy with the paint), an unlist (the copy back with its paint), a
  trade offer by copy ids (the sender's copy escrowed, the receiver's copy row on `/trades/inventory`, accept moves both
  paints, the completed trade shows the copy unit with its paint), a cancel (the copy back), a quick sell of a copy (one
  unit, KR, the copy gone) and a stack quick sell that sells only its unpainted units. Run against origin/main's brain,
  the market checks fail (the copy id is ignored: the stack is listed and the paint is lost; the offer is refused as
  "Only painted copies of that item are left"). A copy offered twice in one side is refused ("Invalid offer").
- `services/brain/tests/craft_test.py`: recipe 1 with its Wood and Wires as painted copies (one unpainted Wood and one
  Wood copy): crafted, the copies taken with their paint. Origin/main refuses the craft ("Only painted copies..." from
  the stack's take).
- `services/brain/tests/social_ws_test.py` (the `vr` redemption): a stack entry of the owned list is `{ind, cnt, st: 1}`
  (`ua` frame, `data[10]`). On origin/main before this change it fails (`an owned stack carries st 1`). The same test's
  two socket inventory checks (`{i, s, c, qs}` exactly) also failed there: the paint commit had leaked the SQL's `sid`
  and NULL paint columns into the socket rows. `shape_inventory` fixes both.
- `services/brain/tests/v10_api_market_test.py` (`paint_scenario`) also checks `st` on the market rows: the paint stack has
  `st: 1`, a painted copy `st: 0`. It fails on origin/main (`market inventory: a stack row has st 1 ...`).
- `services/brain/tests/v10_api_market_test.py` (`paint_scenario`): the refusals (non-paint, unknown item, guest, another
  account's copy), an unpainted unit becoming a copy painted red, a second copy painted shader 0, a repaint of the first
  copy (its paint changes, the other copy keeps its own, the stack keeps its count), the last unpainted unit, no unpainted
  unit left, and a destroy-all that takes none of the three painted units. Run against origin/main's brain, the same
  checks fail (403 NOT_IMPLEMENTED and no copy rows).
- `net-bridge/v10Proxy.test.ts`: a `{i, m}` weapon slot keeps its paint in our spawn's add record, a copy-id slot and a
  plain index keep none. Run against origin/main's bridge sources, it fails (`T.loadoutPaintOf is not a function`).
- `client/tests/paint_picker_e2e.py`: a scratch kn_brain and headless client_bin (online, token login). The Hat chooser's
  Bullseye Halo (an unpainted unit) and two Red Paints: the card's paint button opens the picker, Paint sends `ip`, the
  brain makes the copy (skin_copies, #dc3c3c), one paint is taken, the popup closes with "Painted Bullseye Halo with Red
  Paint", and the card's badge reads "Painted: Red Paint". An account with no paint gets v10's empty state; a refused Paint
  shows the brain's reason. Before this change the card has no paint button (the first check fails).
- `server/tests/skin_copy_ids_test.c` (`skin_copy_ids_test`): an owner sends `NET_EXT_KN_SKIN_COPY_IDS` naming its second
  Topo copy, an unpainted melee copy and a styled body copy before its ENT; a joiner gets the second copy's paint (SKINS
  V2), its seed and wear (SKIN_COPIES V2, 40 bytes, the V1 prefix unchanged), the named melee copy's lack of wear, the
  body copy's style and the plain hat's first-copy wear (SKIN_STYLES V2, 104 bytes). A gone copy, another index's copy
  and no packet fall back to the plain-index rule; a copy named on another slot of its index is still found.
- `client/tests/skin_paint_test.c`: the wear tiers (`skin_wear_tier_key` / `_color` at each boundary) and the slot
  parser (`"<id>#<ind>"`, a plain index, Random, a falsy id, a hex colour, NaN).
- `client/tests/copy_pick_e2e.py` (`copy_pick_e2e`): alice owns two Topo copies (#ff0000 wear 0.05, #00ff00 wear 0.45)
  and two Flip-Flop copies (wear 0.12, 0.71); her client calls the weapon card's `selectSkin(7819,1,undefined,id)` and
  clicks the second Flip-Flop card of Select Melee. The slots save `"<id>#7819"` and `"<id>#7857"`; bob's client draws
  alice's Topo with color=00ff00 (never ff0000), logs `[wear] weapons/weapon_2_wear level=0.4500` and
  `[wear] melee/melee_182_wear level=0.7100 map=1`, and alice's own model does the same.
