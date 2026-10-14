# The economy: KR gifts, the market and trading (v7.1.4)

Krunker Box Office's player economy: KR gifts, the market (listings, purchases, quick sell, the
Black Market), trade offers, their history and notifications. Reference: Krunker v7.1.4
`src/social.js` (the hub), `src/config.js`, `src/data/store.js`, `public/social.html`. The v7.1.4
tree's own server (`server/socialSocket.ts`, `DBClient.ts`) is a local stand-in that skips most
rules, so the rules below come from the client code and config; where it is silent the choice is
ours and marked **KN**.

Code: `services/brain/economy.c` (social socket handlers, the `ecfg` rules, the expiry timer),
`services/brain/db_economy.c` (every movement, one transaction each), migration `0111_economy`,
the hub UI in `client/src/ui/social_ui.c` (native and web), the toast in `account_ui.c`.
Tests: `services/brain/tests/economy_test.py`, `economy_hub_e2e.py` (native), `economy_web_e2e.py`
(web).

The store table these rules read is Krunker v10.0.2's (8069 items, `assets/skins.json`): v10's
rarities are v7.1.4's unchanged, and indices 0-6447 keep their items, so no rule below changes;
v10 adds one bit, `tradable` (`SkinInfo.tradable`, never true where the v7 rule already refuses).

## 1. Audit

"Before" is origin/main at f75b57b (the v3.1.1-era social port: `social_data.c` handlers, the
`0104` SQL functions / their C twins in `db_social.c`, the hub in `social_ui.c`).

| Feature | v7.1.4 | Ours before | Ours after |
| --- | --- | --- | --- |
| Gift KR: UI | profile "Send Gift" -> `#giftPop` (amount, message, fee line, "lvl 30 required" / "Login to Gift" / "10 KR minimum" / "Not enough KR") | button rendered, `giftPopup` unhandled (no-op) | v7 popup, `calcGiftFee`, `giftUser`, success popup + page refresh (`io_gir`) |
| Gift KR: rules | `giftMin` 10, `giftMax` 1e7, `giftMinLVl` 30, fee `ceil(amount * 10%)` paid on top | level 30, fee `floor` 10 %, no min / max | v7 rules server-side (integer ceil, see 3) |
| Gift KR: notification | "KR Gift" mail | mail + `gmc` | mail + `gmc` + `econN` toast/popup + `ua` (receiver's header KR) |
| Market: browse / filter / sort | `r market <tab>`, `fil` (text, rarity, weapon, type, season, order), `srt` | text + rarity filters only, order ignored | + weapon, type, season filters, highest / lowest price order |
| Market: list | `li` -> `lir {cnt, funds, listingId}` / `MAX`; fee `floor(price * 10%)` unless `<= marketFeeBypass`; level 15; `saleMax` 1e7; noSale / opensea / blackM / free / itemShop items refused | fee 5 %, no level, no tradability check, MAX 50; client: fee display hard-coded 5 %, "lvl 20 required" text with a level 15 gate | v7 fee / level / limits / tradability server-side; client fee and level from `ecfg`; MAX 40 |
| Market: buy | `pi` -> `br`; level 15; buyable `saleGrace` (5 min) after posting (client-side only); seller gets the full price | no level, no grace server-side | level + grace enforced server-side; "Item sold" mail + `econN` + `ua` to the seller |
| Market: unlist | `uli` -> `ulir SUCCESS / SOLD` | yes | yes (idempotent) |
| Market: quick sell | `qsi(mode, skin, uid, amount)`; KR = `qsKR` / `rarities[r].sell` (5 for `minQuickSell`); amount 1 / all but 1 / all; destroy mode | `qsi(skin, uid, count)`: the client sent the amount choice as a count ("All" sold 2) | v7 frame + the old one; `qsKR` / `minQuickSell` / `noQuickSell`; destroy; junk refused (no junk currency here) |
| Market: item sales page | `{ind, onS, inC, high, low}`, owners, graph `gd [{d, f: KR sum, t}]` | no `onS` / `inC`; `gd.f` was an average (the client averaged it again) | v7 fields; `gd.f` = daily KR sum; v7 range index or days |
| Market: listing page `?p=listing&i=` | the listing's card with Purchase | "No Items Found" | rendered (the market popup's content) |
| Market: profile Listings tab | `uml` -> `ul` | sent, never answered | answered and rendered |
| Market: Black Market | `pbm` -> `bmr {item, kr, f}`: blackM items for `funds` KR if `store.checkReq`; owned items are "done" | `pbm` never answered (spinner); capes always "Access Denied" | server-side `checkReq` (accAge / stats), price, ledger; once per account; client age check |
| Trading: entry | profile "Trade" (both level >= 20), "Must be level 20" | button, no handler | `tradePopup` -> `ri` -> `trD` |
| Trading window | `tradePop`: both inventories, search, add / remove, Trade List n/5 (premium 20), est. value, message, "Submit Offer - N KR", toggle missing items, locked items | missing | v7 window (+ **KN** KR fields, see 2) |
| Offer: create | `ctr(id, [{i,c}], [{i,c}], msg)`; fee = sum of `rarities[r].fee` per item on both sides, premium 0; "N items per user", "No Items Selected" | items only, no fee, max 10 per side, receiver level unchecked, items not reserved (the same item in any number of offers) | v7 rules; sender's items (and KR) escrowed; receiver's items checked; fee charged at submit (not refunded) |
| Offer: list | Incoming / Outgoing / History tabs, "Decline All", fast decline, dates | yes, date always "just now" | real dates, KR shown, live refresh on `econN` |
| Offer: view / accept / decline / remove | `viewOffer` -> Accept / Decline / Remove, confirmations, "You get no items from this trade!" | missing (`viewOffer` unhandled) | v7 popups; accept / decline / remove move or return everything in one transaction |
| Offer: KR | none (KR cannot be added) | none | **KN**: either side may add KR (escrowed / taken at accept) under the gift rules (level 30, `gift` ban, 10 KR minimum, gift fee paid by the giver); `trade_kr_max` 0 turns it off |
| Offer: expiry, counter-offers | none | none | expiry **KN** optional (`trade_ttl_h`, default 0 = never, like v7); no counters (v7 has none) |
| Trade lock | `player_notrade` exists, never read | never read | enforced on gifts, listing, buying, offers, accepts |
| History | trade history tab, sales graphs, mail | trade history | + **KN** KR ledger popup (click the header KR): every movement with reason, counterparty, fee |
| Notifications | mail ("Item sold" -> notification type 4) | gift mail; trade pushes as a menu modal (`pRes`) | mails + `econN` (hub popup when idle, toast in menu / game) |
| Fees, levels, limits | config.js | hard-coded / partly wrong | v7 defaults, configurable, advertised to clients by `ecfg` |
| Cooldowns / rate limits | `saleGrace` only | per-socket token bucket | + **KN** per account and action kind, across sockets and brains (DB-backed) |
| 2FA / verified email | no economy action asks for either | optional `accounts.email.require_verified` gate | unchanged |
| Per-feature bans | none | none (only the email gate) | **KN** `feature_bans` (gift / market / trade), checked inside each transaction |
| Live in-game trade (Trade mode, `windows[36]`, `strd` / `utrd` / `atrd`, `tradeCost` 10) | client only: v7.1.4's game server implements no trade frame | not ported (`hud_ui.c`) | **not done**: needs a game-server trade session + brain settlement; follow-up |
| Premium gift (`prm`, `confPremium`) | paid premium days (store purchase, not KR); no server | not ported | not done (real-money store, out of the KR economy) |

## 2. Rules (defaults = v7.1.4)

| Rule | Value | Config (`accounts.*`) |
| --- | --- | --- |
| Gift | level 30, 10 .. 1e7 KR, fee `ceil(amount * 10 / 100)` on top | `gift_min_level`, `gift_min`, `gift_max`, `gift_fee_percent` |
| List | level 15, 1 .. 1e7 KR, fee `floor(price * 10 / 100)` (0 when `price <= bypass`), 40 open listings | `market_min_level`, `sale_max`, `market_fee_percent`, `market_fee_bypass`, `max_listings` |
| Buy | level 15, not within 300 s of posting, not your own, seller paid in full | `market_min_level`, `sale_grace_s` |
| Quick sell | `qsKR` else `rarities[r].sell` (1, 3, 10, 100, 500, 2500, 10000, 10), 5 for `minQuickSell`; items the game sells for KR (Black Market) only destroy (**KN**: a 100 KR cape is worth 2500 KR at rarity 5) | - |
| Black Market | `funds` KR, `checkReq` (accAge, stats), once per account (**KN**: also after the item is gone) | - |
| Offer | both players level 20; 5 items per side (premium sender 20); fee = sum of `rarities[r].fee` (5, 5, 5, 10, 10, 25, 200, 500) over both sides, 0 for premium; 50 open offers per sender; KR per side 10 .. 1e7 under the gift rules: the giver needs level 30 and no `gift` ban and pays the gift fee on it (the sender at submit, the receiver at accept) | `trade_min_level`, `trade_max_items`, `trade_max_items_premium`, `trade_max_open`, `trade_kr_max`, `trade_ttl_h` (+ the gift keys) |
| Not tradable / listable | `free`, `noSale`, `blackM`, `opensea`, `itemShop == 1` | skins file |
| Rate limits (per account, per minute) | gift 10, list 30, unlist 60, buy 30, quick sell 60, offer 10, accept 30, close 60 | `economy_rate_<kind>` |

Fee rounding: KR are whole numbers; fees are computed in integers. Gift fee rounds **up**
(`(amount * pct + 99) / 100`, v7's `Math.ceil`), the list fee rounds **down** (`price * pct / 100`,
v7's `Math.floor`). v7's affordability check `Math.ceil(amount * 1.1) <= funds` is off by one KR
for some amounts because of floating point (10 KR -> 12); ours is exact (`amount + fee <= funds`)
and the client shows the same number. The trade fee is a sum of whole numbers. Premium = 
`player_premium > 0` (everyone with `free_premium`, the default).

## 3. Integrity

Every client action is one database transaction (`kn_proc` with no `.pg` text: the C code runs
inside BEGIN .. COMMIT on PostgreSQL, SQLite and MySQL alike; any refusal rolls it all back):

- **Locks.** The listing / trade row first (`SELECT .. FOR UPDATE`), then every player row the
  action touches in ascending player id, then inventory rows (the guarded `UPDATE`s). Decline-all
  and expiry lock all their trade rows first, then all the players, sorted. Two actions on the
  same rows queue instead of deadlocking (the crossing-gifts test fires A->B and B->A together).
  SQLite serialises writers (`BEGIN IMMEDIATE`); MySQL runs READ COMMITTED with InnoDB row locks.
- **No negative balances.** `player_funds >= 0` / `inventory.count >= 0` (schema CHECKs) and the
  guarded `UPDATE .. WHERE player_funds + delta >= 0` (refusal "Not enough KR" / "Item not owned").
- **Ledger.** One `kr_transactions` row per KR movement (`source` = reason, `ref`, `counterparty_id`,
  `fee`), one `skin_transactions` row per item movement, in the same transaction. Per account the
  sum of its rows equals its balance (the tests check it). Sources: `gift-out` / `gift-in`,
  `market-fee`, `market-buy` / `market-sale`, `quicksell`, `blackmarket`, `trade-fee`,
  `trade-escrow` / `trade-return`, `trade-in` / `trade-out`.
- **Escrow.** A listed item leaves the inventory until it is sold or unlisted; an offered item and
  offered KR leave the sender's account until the offer is accepted, declined, removed or expires.
  An item therefore can never be in two listings or offers, or listed and offered at once. Offers
  made before 0111 (`trades.escrow = 0`) still settle from the inventories at accept time.
- **Idempotency.** KN clients append `{"op": key}` to every action frame; `economy_requests`
  (account, kind, key) is claimed by an `INSERT .. ON CONFLICT DO NOTHING` inside the transaction,
  so a retried or doubled frame waits for the first and replays its stored result (and notifies
  nobody again). Keys of actions that can only happen once are derived from their target
  (`buy-<listing>`, `acc-<trade>`, `trc-<trade>`, `uli-<listing>`); the others are made when their
  confirmation popup opens. Frames without a key (v7.1.4 clients) get a server key. Keys older
  than 7 days are deleted by the economy timer.
- **Validation.** Amounts are range-checked as doubles before any integer conversion (handlers)
  and again in the transaction; skin ids must exist in the skins file and be tradable; offers are
  normalised to distinct `[skin, count]` pairs; prices / gifts / KR are capped.
- **Rate limits** count the account's committed actions of the kind in the last minute, after its
  players row is locked, so parallel requests of one account see each other.
- **Batches** (decline all, expiry) unwind each offer in its own SAVEPOINT: one that cannot be
  unwound stays open (reported as `failed`) instead of blocking the rest.
- **Known limits.** Deleting an account (no code path does) cascades its trades: delete an
  account only after closing its offers. The escrow trigger exists on PostgreSQL (production);
  MySQL / SQLite deployments must not run an old and a new brain at once.
- **Bans and locks.** Account bans (`bans`) and per-feature bans (`feature_bans`: `gift`, `market`,
  `trade`) are checked inside the transaction, for the counterparty too: a trade-locked or banned
  player receives no gift, their listings do not sell and their offers cannot be accepted. Unlisting and declining / removing offers stay allowed
  (they give the player's own things back). Staff add a ban with
  `INSERT INTO feature_bans (account_id, feature, reason, expires_at) VALUES (...)` and lift it by
  setting `revoked_at`.

## 4. Protocol (social socket)

Frames as in doc/backend.md 7.x; KN additions in **bold**:
`["kr", name, amount, msg, {op}]` -> `["gir", {funds, amount, fee}]`;
`["li", skin, skinid, uid, price, {op}]` -> `["lir", {cnt, funds, listingId, fee} | {response:"MAX"}, uid]`;
`["uli", skin, listing, uid, {op}]` -> `["ulir", "SUCCESS" | "SOLD" | null, uid, err]`;
`["pi", listing, uid, {op}]` -> `["br", {f, i, sid, m, price} | null, uid]`;
`["qsi", mode, skin, uid, amount, {op}]` -> `["qsr", {i, c, f, kr, j, jnk} | null]`;
`["pbm", skin, {op}]` -> `["bmr", {item, kr, f} | null]`; `["uml", id]` -> `["ul", rows]`;
`["ri", name, id]` -> `["trD", name, id, [mine, theirs]]` (**`nt: 1`** marks untradable items);
`["ctr", id, [{i,c}], [{i,c}], msg, {op, **skr, rkr**}]` / `["trA", id, {op}]` /
`["trC", id, fast?, {op}]` -> `["trR", {trid, funds, ...}, id | null, msg]`;
`["trCA", {op}]` -> `["pRes", "SUCCESS", null, "Declined All"]`;
**`["ecfg"]` -> `["ecfg", {rules}]`**, **`["krh", days]` -> `["krh", rows]`**, pushes
**`["econN", "gift" | "sale" | "trade", text]`** and `["ua", ...]`. Refusals are
`["po", null, reason]` (v7's `io_po`).

## 5. Migration 0111_economy

Additive, forward-only, safe on the live database: `kr_transactions.counterparty_id` / `fee` (the
foreign key is `NOT VALID`: no scan of the existing ledger), `trades.sender_kr` / `receiver_kr` /
`expires_at` / `escrow` (constant defaults: no table rewrite), the
`economy_requests` and `feature_bans` tables, and the 6-argument
`kn_add_funds(id, amount, source, ref, counterparty, fee)` overload (the 4-argument
`kn_add_funds` and every 0104 function stay as they are, unreferenced by the economy).

## 6. Deploy

1. Apply `0111_economy` (`kn_brain --migrate` / `kn_migrate`) before or as the new kn_brain starts.
   It only adds columns with constant defaults, two tables, a `NOT VALID` foreign key, a small
   index on `trades` and a trigger: brief locks, no rewrite, safe while the old brain serves.
   KR movements without a counterparty (match rewards, Turf Wars, challenges, map rewards) keep
   writing the pre-0111 ledger row, so they work whichever of the two happens first; the economy
   actions need 0111 and answer "Server error" until it is in.
2. Restart every brain serving the social socket in the same deploy. While an old brain still
   runs, the `trades_escrow_guard` trigger makes its accept / decline / decline-all of an escrowed
   offer fail instead of losing the escrow (the player retries on a new brain).
3. Config: the defaults are now v7.1.4's (list fee 10 % instead of 5 %, 40 listings instead of 50,
   5 / 20 items per side instead of 10, market level 15, 5 minute buy grace). Set the
   `accounts.*` keys of section 2 to keep other values; clients learn them from `ecfg`.
4. Staff: per-feature bans are rows in `feature_bans` (section 3); the KR ledger of an account is
   `SELECT * FROM kr_transactions WHERE account_id = $1 ORDER BY id DESC`.

## 7. Not done

- Live trading inside a match (Trade mode): v7.1.4 has only the client half; it needs a game
  server trade session and a brain settlement op.
- Premium gifting (`prm`): a real-money store item, not KR.
- Junk (`qsi` mode 1, `jnk`): v7.1.4's second currency does not exist here.
