# Junk wallet (Krunker v10.0.2 parity)

Krunker v10.0.2 gives every account a second currency, Junk, kept to two decimal places. KrunkNative
has a wallet for it now (user, 2026-10-18: "build a junk system. implement everything and match parity
with v10"), which reverses the older "no junk wallet" rule in doc/v7/decisions.md ("Reward Junk").
This file is the contract: the balance, the C API the rest of kn_brain uses, the internal ops the game
servers call, the account JSON, and every place that earns or spends Junk. Crafting (the Junkyard's
recipes) is a separate owner and reads this file for the API it uses.

Line references `v10:N` are to `~/.cache/kn-v10/v10.js` (read only). `v7:path:line` refers to
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

Every reply's `junk` is the balance after the call, as a decimal string. A successful credit or spend
that the game server did not ask for in a reply (a `junk_credit` from a game) also pushes the new
balance to the account's social sockets: `["uj", "12.34"]` (v10's UPDATE_FUNDS, junk part; `uf` is the
profile activity frame and stays).

Game servers send `junk_credit` with `reason` one of `trigger` (Reward Junk) and `airdrop` (Junk
Package); the ops above accept any reason of at most 32 characters.

## 4. Account JSON

- Brain account object (`db_account_json`, the output of `accounts_internal_call` `get` / `login` /
  `token` and every social login): `"junk": "12.34"`, a decimal string with two places.
- The social socket's account info (`social_account_info`, the info object of the `a` frame and of
  `ua`): `junk` is copied with the other keys. The client reads it into `AccountInfo.junk` (the same
  field the navbar and the Account window already show, `client/src/ui/v10_menu.c`,
  `account_ui.c`). Missing or non-numeric reads as 0.00.
- The client never computes a balance. It only displays the brain's value.

## 5. Where Junk is earned

| Source | v10 rule | KN |
|---|---|---|
| Reward Junk trigger (action 59, `triggers.js:1392-1404`, `v10:163451-163455`) | `(!isSandbox && isPubType() && account)` and `v = parseFloat(trigConstTx || 0)`, `!!v && !(v >= 100)` -> `sendUserJunk("trigger", player, v)`. devOnly in the editor. | Credits `llround(v*100)` hundredths with reason `trigger`. Only for a player with a brain account in a public, non-sandbox game (triggers_actions.c `reward_junk`). Bots never. |
| Junk Package (airdrop, `airDropPackages.js:122-130`, `v10:178614-178676`) | Rarity 0..6 = 0.05, 0.1, 0.2, 0.3, 0.5, 1, 2.5 Junk. `send("am", [thief ? "Junk Thief" : "Junk Claimed", amount + " Junk", 0, 1])`, then `sendUserJunk("airdrop", claimer, amount)`. | The same amounts (shared/src/airdrop.c). The popup goes to every human claimer, guests included (v10 gives guests an account object). The credit needs a brain account; a guest has none, so a guest gets the popup only. |
| Battle Pass junk cards (`bpclm`, `v10:` claimItemRes junk, `jnkdata`) | Each claimed card pays its `jnk`. | Credited with reason `battlepass` when the card is claimed (db_season.c `claim_cards`). |
| Quick sell, junk mode (`qsi` mode 1, v10 `canJunk`) | Junk per item = the rarity's `scrap` (Contraband 5, Unobtainable 50; the item's `minQuickSell` gives 0.01 when it has a scrap value; `noQuickSell` and skins without a scrap value cannot). v10's per-skin `qsJunk` is not in the data bundle. | Credited with reason `quicksell` (economy.c, db_economy.c `run_quick_sell`). |
| Challenge rewards (`chall`) | v7 / v10 challenge data carries no Junk in KN's catalogue. | Nothing. |
| Shop purchases with a Junk reward (`currency` "jnk", `_38968`, `v10:113972-113976`) | A store item that grants Junk (the amount is hundredths). | Nothing: KN's store sells no Junk packs. |
| Map rotation junk and the other v10 `item_flow` sources | Not in KN's maps or market. | Nothing. |

Shared rule for every source: a credit is written when the amount is positive; a credit below half a
hundredth (0.005) rounds to nothing. The chat line below follows a credit that the brain accepted.

## 6. Where Junk is spent

v10 spends Junk in its market (a listing priced in Junk, `currency_id` 2), its black market, and its
shop purchases priced in Junk (`currency_id` 2, `_38982`, `v10:114013-114028`), the black market and
lottery entries (`lottery_entry`, `v10:125525-125540`), and in crafting (the Junkyard). KN's store sells
no Junk-priced item, its market has no Junk currency and KN runs no lottery, so the only KN spend is:

- `junk_spend` (C and the `junk_spend` op) with reason `craft` (the craft owner) or any other reason
  an owner adds. A spend that is not affordable returns `JUNK_INSUFFICIENT` and writes nothing.

A Junk-priced market listing, a Junk black-market item or a lottery entry would call `junk_spend`
with its own reason; they are not built.

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

- `services/brain/tests/junk_test.py` (ctest `brain_junk`, PostgreSQL and SQLite): credit, spend,
  insufficient (no write), the ledger sum, a credit push to the social socket, the account JSON
  string, bad amounts, and a Battle Pass junk card.
- `shared/tests/junk_test.c` (ctest `shared_junk`): the amount helpers, and a scripted account (a
  GameHooks stub) through the Reward Junk gates (public, non-sandbox, human, account, amount < 100)
  and the airdrop packages' amounts and popups.
- `client/tests` (ctest `client_junk_counter`): the navbar's text and the account info parse.
