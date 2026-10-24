# Live Trade (Trade mode, Trade Plaza): v10.0.3

The in-game, player-to-player trade of Krunker v10.0.3's Trade mode (`trade`, the Trade Plaza
lobbies). Not the trade **offers** of the Market / hub (doc/v7/economy.md, `/trades/*`): a live
trade is two players in one lobby, one window, both accept, everything changes hands at once.

Reference: `deob-10.0.3.js` (`/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`). The
bundle is the client only; the game server that runs the session is not in it, so the server
rules below are the client's contract plus KrunkNative's own choices, marked **KN**.

## 1. What 10.0.3 does

### Mode

`deob-10.0.3.js:119697-119711`: `{id: "trade", name: "Trade", sendR: 5, endStats: [sid, name,
score], noClientKick, canTrade, friendly, noHPbars, viewDis 500, nameDist 60, gameTime 0,
maxNoMov 180000, noMinimap}`. Every field but `maxNoMov` was already in `modes.c`. `maxNoMov` has
no reader in the client (the audit's "dead", gameplay-sim.md); it is the server's no-movement
kick for the mode (the client's own idle kick is off: `noClientKick`, `:261095`). **KN**: the
server's idle kick uses it as the mode's in-game timeout, 180 s (section 3).

`mode.canTrade` readers (all client):

| Where | What |
| --- | --- |
| `:192464-192474` | the trade prompt: every 500 ms, not while a waitTimer runs, each other active player with a `level` (an account) within 13.2 units of us (eye to eye, y + 5.5) gets `changeInteract(me, player, 5, null, 1)` |
| `:197504-197521`, `:197562-197567` | changeInteract stores `lastInteractData = {id: player.sid, type: 5}`; the prompt reads "Press [interact] to Trade with [name]" (`fakeName`, else the alias for premium, else the name; `#9eeb56`), "Tap to Trade with" on mobile |
| `:260120-260126` | the interact key sends `int` [5, sid] |
| `:250341` | the player list's "Trade Player" button (`store` icon): `requestTrade(id)` -> `strd` [id] (`:254074-254084`) |
| `:253813` | `showWindow(n)` for any other window cancels the trade (`windows[36].cancel()`) |
| `:256686`, `:258806` | the round switch and the death screen close the window |
| `:257012` | game start fetches the market prices (`/webhooks/general/items/prices`, `:148053`) for the window's Est. Value |

### Packets

C -> S (`:110976-111115`; `iîiìiîi.send`):

| Name | Key | Args | Sent by |
| --- | --- | --- | --- |
| SEND_TRADE_REQUEST | `strd` | id | player list Trade button (`:254081`) |
| INTERACT | `int` | 5, sid | interact key near a player (`:260125`) |
| INIT_TRADE | `trd` | sid, 1 | the chat line's [Accept] (`acceptRequest`, `:253613-253615`) |
| TRADE_UPDATE | `utrd` | skin, action, copyId \|\| 0 | action 1 add own item (`:251295`), 2 request the other's (`:251280`), 0 remove own (`:251388`) |
| TRADE_CURRENCY_UPDATE | `utrdc` | currency, action, amount | (1, 1, n) add own KR (`:251446`), (1, 3, n) request KR (`:251458`), (c, 2, 0) remove (`:251466`) |
| TRADE_ACCEPT | `atrd` | - | the Accept Offer box (`:251411`) |
| TRADE_DECLINE | `dtrd` | - | the Decline Offer box (`:251425`) |
| TRADE_CANCEL | `ctrd` | - | the window's close (`:251524`), another window (`:253813`) |
| TRADE_CHAT | `tct` | text (maxlength 70) | Enter in `#tradeChatInput` (`:260031-260036`) |

S -> C (`:111116-`; handlers registered at `:261708`):

| Name | Key | Args | Handler |
| --- | --- | --- | --- |
| TRADE_RESPONSE | `trd` | msg, sid, type, invs | `:253238-253267`: type 0 a request from sid (chat "NAME wants to trade [Accept]", `:259629`); 1 open the window with sid, invs = [mine, theirs] (`showWindow(37)`, pointer lock released); 2 canceled (window shut); 3 sid accepted (READY, green header); 4 `process(n)`: "Loading n"; 5 `updateButtons` (both READY cleared). msg, when set, is a chat line |
| UPDATE_TRADE_RESPONSE | `utrd` | ownerId, add, skin, copyId | `:253308-253315`: add / remove one unit on the owner's side, then `updateButtons` |
| UPDATE_TRADE_CURRENCY_RESPONSE | `utrdc` | ownerId, action, currency, amount | `:253316-253323`: 1 add (amounts add up), 2 remove; then `updateButtons` |
| TRADE_A_RESPONSE | `atrd` | error, funds, removed[], added[], meta{} | `:253269-253307`: error -> "failed" (window shut, red chat line); else funds, removed / added skins ("skin" or "skin:copyId", meta by copy id), "Trade Successful" (`modes.trade.success`) |
| ADD_TRADE_CHAT | `tch` | name, msg, type, color | `:259539-259561`: `#tradeChatList` (35 lines kept); type 2 a coloured system line (`#fc03ec` default) |

Currencies (`:251468-251499`): 1 KR ("Krunkies"), 2 JNK (Junk), 3 RP (Ranked Points). The window
only ever sends KR.

### Window (`windows[36]`, `showWindow(37)`, `:251107-251527`)

Header `windows.trade.header` "Trading", width 1146. Left: "Your Inventory" (`#invList0`, 225 px,
`trd_inv_0_<skin>[_<copy>]` cards: thumbnail, name, `xN`, rarity border, a `lock` overlay on
untradable items, `hideNonTrade` hides them), a search box, the KR Add row, "You n/5" (20 premium,
`#tradeTotal0`, `#tradeReady0`), `#tradeList0`, "Est. Value N KR" (`#offerVal0`), the
"Accept Offer - 10 KR" checkbox (`#tSubOffer`). Right: "NAME's Inventory" (clicks request),
search, KR Request row, "NAME n/5", `#tradeList1`, Est. Value, "Decline Offer" (`#tDecOffer`).
Below: `#tradeChatHolder` (`#tradeChatList`, `#tradeChatInput` "Enter Message", 70 chars).
Untradable = `free || noSale || blackM || opensea || itemShop == 1` (`:251130`); KN uses the item
table's `tradable` bit (doc/v7/economy.md). A selected unit dims a copy's card / lowers a stack's
`xN`; the totals and Est. Value (`Math.round(price * count)`) follow every change.

### Config (`:22627`, `:22952-22955`)

`interactTradeTimer` 3000 (server: one trade request per 3 s), `tradeCost` 10 (the label's
"10 KR"), `tradeMaxOut` 5, `tradeMaxOutP` 20 (premium; the side's owner), `tradeMinLvl` 10
(`errors.exchange.tradeLevelTooLow` "Both players must be level 10 or higher to trade.").

## 2. The server and the brain (what the bundle does not show)

The client's contract above says nothing about who checks what; KrunkNative decides (**KN**):

| Step | Rule |
| --- | --- |
| Request (`strd`, `int` [5, sid]) | Trade mode only (`canTrade`), not on the end screen; both players signed in with an account at level >= 10 (`tradeMinLvl`), neither already trading; the interact request also needs both alive and within 13.2 + 6 units (the prompt's reach plus slack for the server's older positions); one request per 3 s per requester (`interactTradeTimer`). The target gets `trd` type 0, the requester the line "Trade request sent to NAME". A request can be accepted for 30 s. |
| Accept (`trd` [sid, 1]) | the request exists and is fresh, both still eligible. The session opens: the brain's `trade_inventories` op loads both inventories (15 s timeout), then both get `trd` type 1 with [mine, theirs] (v10 rows `{i, c[, id, m, nt]}`). |
| `utrd` add / request / remove | one unit per packet; the unit must be in the side owner's snapshot and not all offered already, not `nt` (untradable), and the side must hold fewer than 5 units (20 when its owner is premium). Every change goes to both (`utrd`), clears both READY marks and stops a countdown. |
| `utrdc` | KR only (Junk / RP have names in v10 but no window controls: refused). Add / request accumulate; remove takes the side's KR off. A side's KR + the 10 KR `tradeCost` must fit its balance; at most 1e7 (`giftMax`). |
| `atrd` | not with both sides empty ("Your trade offer is empty or invalid."), not without the side's KR + 10 KR. Both READY: "Loading 3", "Loading 2", "Loading 1" (one a second, `trd` type 4), then the settle. A change during the countdown cancels it. |
| `dtrd` / `ctrd` | end the session until the settle starts (the guide: "you can cancel any time before both sides accept"; the countdown can still be canceled). `dtrd` (the window's box) shuts both windows; `ctrd` comes from the window's close or another window, so only the other side gets `trd` type 2 ("NAME canceled the trade"). A leave ends it the same way ("NAME left the trade"). |
| `tct` | relayed to both as `tch` [name, msg], sanitized like chat, 70 code points, one line per 0.4 s. |
| Settle | the brain's `live_trade` op, a **reliable** frame (brain.c resends it after a reconnect) with the session's key `lt:<32 hex>`. Success: each side gets `atrd` [null, funds, removed, added, meta] and the server's ENT ownership bits follow the brain's counts; a refusal: both get `atrd` [error]. Nothing can change while it runs. |
| Mode / round | leaving Trade mode or the end screen cancels every session that is not settling. `maxNoMov` 180 s is the mode's in-game idle kick (`idle_kick_seconds` elsewhere); every trade packet counts as activity. |

The brain (`services/brain/economy.c` `econ_live_trade*`, `db_economy.c` `run_live_trade_open` / `run_live_trade`,
migration `0357_live_trades`) is the authority on what anyone owns:

- `trade_inventories {a, b}` -> `{a: {inv, funds, premium, level, notrade}, b}`: the trade picker's rows
  (`inventory_items`: unpainted units per skin, one row per painted copy) in v10's shape, `nt: 1` on items whose
  `tradable` bit is off.
- `live_trade {key, a: {id, items, kr}, b: {id, items, kr}, cost}`: economy.c checks every skin exists and is
  tradable, merges stacks, refuses duplicate copies; then **one transaction** (`kn_proc` C body: BEGIN .. COMMIT on
  every driver, any refusal rolls it all back) claims the key in `economy_requests` (a resent frame replays the
  stored result), locks both `players` rows in ascending id, re-checks trade locks, account bans, `trade` feature
  bans (both sides), `gift` bans for KR givers, both levels, the 5 / 20 unit limits, each side's KR + `tradeCost`,
  inserts the `live_trades` row (`req_key` UNIQUE: a second guard against a double settle), then moves the stacks
  (`db_add_skin`: a missing unit refuses everything, "Item not owned"), the painted copies (`copy_take` /
  `copy_give`: the paint goes along under a new copy id) and the KR (`live-trade-out` / `-in`), and charges each
  side `live-trade-fee` 10 KR. KR moves 1:1: v10 shows the 10 KR cost only (**KN**: no gift fee on it, unlike the
  trade offers). Both accounts' sockets get `ua` (header KR, owned list). Ledger: one `kr_transactions` /
  `skin_transactions` row per movement (ref `live:<id>`), `live_trade_items` one row per stack / copy.
- Two settles racing for the same unit serialize on the inventory row: one wins, the other is refused.

## 3. KrunkNative

| Piece | Where |
| --- | --- |
| Wire bodies, v10 constants | `shared/include/live_trade.h`; subtypes `NET_EXT_TRADE_*` 0x0b00-0x0b07 (C -> S), 0x0b10-0x0b15 (S -> C) in `shared/include/net_ext.h` |
| The prompt | `GAME_INTERACT_TRADE` 5 (`shared/include/shared.h`); client `trade_ui_interact_target` / `ui_interact.c` `scan_with_trade` (priority 1, after the map's prompts); the key sends `int` [5, sid] |
| Server session | `server/src/trade_session.c` (the state machine, pure), `server/src/trade.c` (requests, packets, brain ops, tick), hooks in `server/src/net.c` (EXT dispatch, interact type 5, leave, idle) and `server/src/accounts.c` (replies) |
| Brain | `econ_live_trade_inventories` / `econ_live_trade` (economy.c), `P_LIVE_TRADE_OPEN` / `P_LIVE_TRADE` (db_economy.c), `0357_live_trades` (all three dialects) |
| Client window | `client/src/ui/trade_ui.c` (windows[36] markup, handlers `windows[36].*`, `acceptRequest`, `requestTrade`), `ui_windows.c` `gen_37`, `menu_ui.c` (another window / the close cancels), `hud_ui.c` (the player list's Trade Player button, the request line) |
| Official lobbies | `client/src/net/krunker_link_trade.c` (`trd` `utrd` `utrdc` `atrd` `tch`, `ch` type 3; out `strd` `trd` `utrd` `utrdc` `atrd` `dtrd` `ctrd` `tct`) |
| Tests | `server/tests/trade_test.c` (the state machine + the server flow over the sandbox, the brain faked; `make test-net`), `services/brain/tests/live_trade_test.py` (the two ops on a scratch database; ctest `brain_live_trade`), `server/tests/live_trade_e2e.py` (server_bin + kn_brain + two raw clients; `make live-trade-e2e`); `--render-scenario` with a `"trade"` state draws the window headless |

Deliberate differences: the window redraws a side's list from its state instead of v10's in-place DOM edits (same
markup); the request line is the chat's own row with the [Accept] span appended, `acceptRequest(sid, this)` (C sets "[Accepted]"); the chat and KR inputs
send on Enter through their own `onkeydown` (v10's page-wide key handler); a spray card shows its first frame
(no `sprayAnm` keyframes); the search matches name, creator and weapon (v10 also keyW, drops, limT). Premium's
"Reduced Trade Fees & Cooldowns" (`:19349`) has no live-trade numbers in the bundle: not applied.
