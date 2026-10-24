# Junk wallet (Krunker v10.0.2 parity)

Krunker v10.0.2 gives every account a second currency, Junk, kept to two decimal places. KrunkNative
has a wallet for it now (user, 2026-10-18: "build a junk system. implement everything and match parity
with v10"), which reverses the older "no junk wallet" rule in doc/v7/decisions.md ("Reward Junk").
This file is the contract: the balance, the C API the rest of kn_brain uses, the internal ops the game
servers call, the account JSON, and every place that earns or spends Junk. Crafting (the Junkyard's
recipes) is a separate owner and reads this file for the API it uses.

Line references `v10:N` are to `~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js` (read only). `v7:path:line` refers to
`/home/user/jails/kru/src/Krunker_v7.1.4/src/`. v7.1.4 keeps the same economy code; v10 only moved
some of it. Where v10 and v7.1.4 differ, v10 wins.

## 1. Balance and storage

- One balance per account in **hundredths** (a 64-bit integer, `players.player_junk`, migration
  `0347_junk_wallet`, PostgreSQL / SQLite / MySQL). 12.34 Junk is `1234`. Never a float in the
  database, the wire between servers, or the ledger.
- The balance never goes below 0 (`CHECK (player_junk >= 0)`, and the guarded UPDATE of
  `junk_spend`).
- Every change writes one row of `junk_transactions` in the same transaction: `account_id`, `amount`
  (signed hundredths), `source` (the reason, at most 32 characters), `ref` (optional) and `at`. The sum
  of an account's rows equals its balance, as for `kr_transactions`.
- v10 keeps Junk in `player_jnk` (the account's `junk` field, `Account.getData()` slot 37, v10:112863).
  The KN name is `junk` in the account object (below); the database column is `player_junk`.

## 2. The C API (services/brain/db_junk.c, declared in db_procs.h)

All three run inside the caller's transaction (`Dbx *x`), like `db_add_funds_ex`. Amounts are
hundredths. `reason` is the ledger's `source`.

| Name (exact) | Signature | Result |
|---|---|---|
| `junk_balance` | `int64_t junk_balance(Dbx *x, int64_t account_id)` | the balance in hundredths (0 for no row; check `dbx_failed(x)` for a failure) |
| `junk_spend` | `int junk_spend(Dbx *x, int64_t account_id, int64_t amount_hundredths, const char *reason, int64_t *balance)` | `JUNK_OK` (0): spent; `JUNK_INSUFFICIENT` (1): the balance is below the amount, nothing written, not an error; `-1`: refused or failed (the transaction is rolled back with `dbx_refuse`, e.g. "Account not found."); `*balance` (optional) is the balance after the call |
| `junk_credit` | `int junk_credit(Dbx *x, int64_t account_id, int64_t amount_hundredths, const char *reason, int64_t *balance)` | `0`: credited; `-1`: refused or failed (no account, amount out of range `1 .. 100000000`); `*balance` (optional) is the balance after the call |

`junk_spend` never goes negative and never partially spends. A caller that wants "ok or insufficient"
branches on `JUNK_OK` / `JUNK_INSUFFICIENT`; the refusal path is for the errors only. The balance is
read under the row lock the UPDATE takes, so two spends at one balance cannot both succeed.

Amount helpers (shared/include/junk.h, shared/src/junk.c): `junk_hundredths(double)` rounds a Junk
amount to the nearest hundredth (`llround(x * 100)`), `junk_format(hundredths, buf, n)` writes
`"12.34"` (two places, always), and `junk_parse(const char *)` reads a decimal string back, rounding
to hundredths. Nothing else formats Junk.

## 3. Internal ops (game servers, `acct` frames on /mm/internal/ws)

Accounts ops (services/brain/accounts.c, doc/backend.md section 5) with `args.id` = the account id:

| Op | Args | Reply `data` | Errors |
|---|---|---|---|
| `junk_balance` | `{id}` | `{junk: "12.34"}` | "Account not found." |
| `junk_credit` | `{id, amount, reason}` (amount in hundredths) | `{junk: "12.34", credited: 5}` | "Account not found.", "Invalid amount." |
| `junk_spend` | `{id, amount, reason}` | `{junk: "12.29"}` | "Not enough Junk", "Account not found." |

Every reply's `junk` is the balance after the call, as a decimal string. Every successful credit or
spend also pushes the new balance to the account's social sockets: `["uj", "12.34"]` (v10's
UPDATE_FUNDS, junk part; `uf` is the profile activity frame and stays), so a game's credit reaches the
navbar while the player is signed in.

Game servers send `junk_credit` with `reason` one of `trigger` (Reward Junk) and `airdrop` (Junk
Package); the ops above accept any reason of at most 32 characters.

## 4. Account JSON

- Brain account object (`db_account_json`, the output of `accounts_internal_call` `get` / `login` /
  `token` and every social login): `"junk": "12.34"`, a decimal string with two places.
- The social socket's account info (`social_account_info`, the info object of the `a` frame and of
  `ua`): `junk` is copied with the other keys. The client reads it into `AccountInfo.junk` (the same
  field the navbar and the Account window already show, `client/src/ui/v10_menu.c`,
  `account_ui.c`). An info without the key keeps the value it had; non-numeric text reads as 0.00.
- The client never computes a balance. It only displays the brain's value.

## 5. Where Junk is earned

| Source | v10 rule | KN |
|---|---|---|
| Reward Junk trigger (action 59, `triggers.js:1392-1404`, `v10:163451-163455`) | `(!isSandbox && isPubType() && account)` and `v = parseFloat(trigConstTx || 0)`, `!!v && !(v >= 100)` -> `sendUserJunk("trigger", player, v)`. devOnly in the editor. | Credits `llround(v*100)` hundredths with reason `trigger`. Only for a player with a brain account in a public, non-sandbox game (triggers_actions.c `reward_junk`). Bots never. |
| Junk Package (airdrop, `airDropPackages.js:122-130`, `v10:178614-178676`) | Rarity 0..6 = 0.05, 0.1, 0.2, 0.3, 0.5, 1, 2.5 Junk. `send("am", [thief ? "Junk Thief" : "Junk Claimed", amount + " Junk", 0, 1])`, then `sendUserJunk("airdrop", claimer, amount)`. | The same amounts (shared/src/airdrop.c). The popup goes to every human claimer, guests included (v10 gives guests an account object). The credit needs a brain account; a guest has none, so a guest gets the popup only. |
| Battle Pass junk cards (`bpclm`, `v10:` claimItemRes junk, `jnkdata`) | Each claimed card pays its `jnk`. | Credited with reason `battlepass` when the card is claimed (db_season.c `claim_cards`); the account is pushed again (`ua`, which carries the junk). KN's Season 7 pass has an empty `jnkdata` (no junk card yet), so nothing pays until a pass has one. |
| Quick sell, junk mode (`qsi` mode 1, v10 `canJunk`; `deob-10.0.3.js:122598-122638`, rarities `116262-116339`) | Junk per item = the rarity's `scrap` (Contraband 5, Unobtainable 50; the item's `minQuickSell` gives 0.01 when it has a scrap value; `noQuickSell`, `noSale && blackM`, `opensea` and skins without a scrap value cannot). v10's per-skin `qsJunk` is not in the data bundle. The window's "Get Junk" (`qs-btn-junk`, action 1) sends `quickSellItem({item_id, action: 1, amount})` and sets the account's junk from the reply's `junk` (`deob-10.0.3.js:147675-147736`). | Built: `economy.c` `quick_sell_junk` (the rule, hundredths), `sh_quick_sell` mode 1, `db_economy.c` `run_quick_sell` (`junk_credit`, reason `quicksell`, in the sale's transaction). The socket reply `qsr` carries `j` (the balance after, Junk) and `jnk` (the Junk given); `POST /market/quick-sell` answers `{funds, junk, count, kr, jnk, sold}` with `junk` the balance (its KR and destroy answers carry the balance too, no longer 0). The inventory rows carry `qj` (Junk per unit, hundredths, only when positive) and `nqv` (an item v10 neither sells nor destroys: every mode is refused). The client's Quick Sell window (`v10_market.c`) shows the Junk row, total and button and sets the navbar from `j` (`account_ui_set_junk`); the brain's `ua` push follows. A Black Market item keeps its Junk (v10 does), though it never quick-sells for KR here. |
| Challenge rewards (`chall`) | v7 / v10 challenge data carries no Junk in KN's catalogue. | Nothing. |
| Shop purchases with a Junk reward (`currency` "jnk", `_38968`, `v10:113972-113976`) | A store item that grants Junk (the amount is hundredths). | Nothing: KN's store sells no Junk packs. |
| Map rotation junk and the other v10 `item_flow` sources | Not in KN's maps or market. | Nothing. |

Shared rule for every source: a credit is written when the amount is positive; a credit below half a
hundredth (0.005) rounds to nothing. A negative Reward Junk amount is not credited: v7 would subtract it,
but the wallet never goes below 0 (KN deviation, triggers_actions.c `reward_junk`). The chat line below
follows a credit that the brain accepted.

## 6. Where Junk is spent

v10 spends Junk in its market (a listing priced in Junk, `currency_id` 2), in its shop purchases priced
in Junk (`currency_id` 2, `_38982`, `v10:114013-114028`), in its lottery entries (`lottery_entry`,
`v10:125525-125540`: a currency cost with `cost_ref` 2), and in crafting (the Junkyard). The black
market is priced in KR (`funds`, `v10:136000` on), not Junk, so KN's black market (KR, doc/v7/economy.md)
has no Junk spend. KN's spends:

- `lottery` (reason `lottery`): a lottery entry whose cost is currency 2 (services/brain/db_lottery.c,
  migration 0348_lottery). A bundle costs `cost_quantity` hundredths and gives `num_entries` entries. The
  three seeded lotteries (the captured v10 API: 1, 2, 3) cost skins only, so Junk entries wait for a
  currency cost that staff add.
- `shop` (reason `shop`): a Junk-priced shop product (services/brain/db_shop.c, shop.c, migration
  0349_shop_junk): the price times the quantity, spent through `junk_spend`, and its skins granted in the
  same transaction (`POST /api/shop/purchase`). The product table starts empty: v10's live "limited"
  display (public, read as a guest) had no Junk-priced product on 2026-10-19, so nothing is sold until
  staff publish one.
- `craft` (the craft owner) or any other reason an owner adds.

A spend that is not affordable returns `JUNK_INSUFFICIENT` and writes nothing: the lottery entry or the
purchase is refused "Not enough Junk".

A Junk-priced market listing and a Junk black-market item are not built (v10 has neither a Junk listing
price in this catalogue nor a Junk black-market price).

## 7. What the player sees

- Navbar: `menuJNKCount` shows the balance, `%.2f` (v10:`_8988(junk.toFixed(2))`, `v10:23916`).
- Account window: the stats' "Junk" row (`pst_num`), the Junkyard's header display
  (`junkyard-display`, `store_ui.c`), both the balance.
- The "You found [x.xx] Junk" chat line (v10 `io_foundJunk`, `v10:208256`, purple amount, white
  "You"): the finder only, sent when the brain accepts the credit. The text is `kn.chat.foundJunk`
  in `ui/i18n/kn/en.json`.
- "Junk Claimed" / "Junk Thief" popup (v10 medal, `ADD_MEDAL`): the title, to any human claimer.
  The sub-line "0.05 Junk" is not on the popup packet (the packet has one text field); the chat line
  carries the amount, as for KR Packages.

## 8. Failure rules

- Ops from game servers are not reliable frames (`brain_post(frame, 0)`): a credit whose link
  drops before the brain answers is lost, never duplicated (the give_skin rule). The chat line
  then does not print.
- Every credit and spend is one transaction with one ledger row. A rolled-back proc writes neither.
- A refused credit (no account) prints nothing and changes nothing.

## 9. Tests

- `services/brain/tests/junk_test.py` (ctest `brain_junk`, `brain_junk_sqlite`, `brain_junk_postgres`):
  credit, spend, "Not enough Junk" (no write), the ledger sum, the `uj` push to the social socket, the
  account JSON and login info text, bad amounts, unknown accounts, and six parallel spends at one balance.
  The Battle Pass junk card is not in this test (KN's seeded pass has none).
- `shared/tests/junk_test.c` (`make test`, `junk_test`): the amount helpers (hundredths, the two-place
  text, parseFloat's reading, non-finite values).
- `shared/tests/triggers_test.c` (`test_reward_junk`): a scripted account (the test's account hook) and
  a scripted wallet (`GameHooks.junk_credit`): Reward Junk 0.01 / 2.5 / 99.99 credits, 100, 0, -1, 'abc',
  '0x10', a guest, a bot and the sandbox do not, and no chat line leaves the game.
- `shared/tests/zombies_test.c` (the Junk Packages): the popup to every human claimer, the credit only
  for an account (0.05 / 2.50 / 0.05 hundredths), nothing for a guest or a bot, and no chat line.
- `server/tests/pickup_chat_e2e.py` (`make pickup-chat-e2e`): a standalone server has no brain: the
  pickups are destroyed and print no line.
- `services/brain/tests/economy_test.py` (ctest `brain_economy`, `brain_economy_sqlite`, ...): the Junk quick sell
  on the social socket: a Legendary's mode 1 is refused (no scrap), a Contraband's inventory row says `qj` 500,
  selling both units gives 10.00 Junk (`j` the balance, `jnk` the gain), no KR, one `quicksell` ledger row, the
  ledger sums to the balance; a noSale Black Market item (Nuke Tamer) is refused for Junk and for destroy.
- `services/brain/tests/v10_api_market_test.py` (ctest `brain_v10_api_market`): `POST /market/quick-sell`
  action 1: an Uncommon is refused (`MARKET_QUICK_SELL_NOT_SELLABLE`), two Contrabands give `{junk: 10.25,
  jnk: 10, count: 0}` with the KR unchanged, one ledger row of 1000, and a second sale of the gone item
  credits nothing.
- `services/brain/tests/lottery_test.py` (ctest `brain_lottery`, `brain_lottery_sqlite`): the guest view of
  lottery 3 (the closed event, "lottery not found"), the refusals (a token, zero bundles, an unknown cost,
  "Not enough Junk" with nothing written), a skin, a Junk and an RP entry, the view with a token, and the
  timer's draw of a window moved into the past: the seeded pools' 10 prizes, each a skin of its pool, one
  grant per prize, drawn once.
- `services/brain/tests/shop_test.py` (ctest `brain_shop`, `brain_shop_sqlite`): the display in v10's
  shape (Junk as hundredths in a "virtual" price), a purchase without a token, "Not enough Junk" with
  nothing written, the spend (reason `shop`) and the skin grants, the per-account limit, an inactive
  product, a bad quantity and the ledger sum.
- `client/tests/junk_shop_e2e.py`: a headless client signed in to the brain: the Eterno Shop's Junk card
  (the screenshot), BUY (2.50 Junk spent, two skins granted, one purchase row), the Anniversary raffle's
  ticket entry (one entry, the ticket spent).
- `server/tests/junk_wallet_e2e.py`: a headless client on the v10 page, signed in to a brain account
  with 12.34 Junk: the navbar's Junk counter reads "12.34" (the login info), then a scripted game
  server's credit of 0.05 makes it read "12.39" through the `uj` push, without signing in again.

Not covered by a test: the server printing "You found [x] Junk" after the brain's reply (the reply
routing in server/src/accounts.c `acc_handle_junk_reply`), and the Battle Pass junk card.
