# Battle Pass (v10.0.3): element to data to request

Krunker Box Office's Battle Pass window (windows[5], menu window 6; `client/src/ui/battlepass_ui.c`) on the
two server types. The official krunker.io server shows krunker.io's pass; the Box Office server shows kn_brain's.
Every line below cites the decoded v10.0.3 bundle `~/.cache/kn-v103/deob-10.0.3.js` (`L` = its line; the bundle
is `/home/user/jails/kru/src/Krunker_v10.0.3/game.js` with names decoded) and, where a literal exists once in the
minified file, its byte offset in `game.js` (`B`). v10.0.2 has the same structure at other lines (only offsets
moved), which the port was written against.

## Where the pass comes from

**Official krunker.io.** The pass is krunker.io's own frames on its game socket, and that socket is open only in an
official game:

- v10.0.3's io client `iîiìiîi` (L111381) queues a frame sent while its socket is not open (L111507-111525), and
  it connects only when joining a game (L261682, inside the join path).
- The menu has no krunker.io socket, and `GET /player/me` (L30247) carries no pass field.
- So the window asks (`bpdl`) when the official session is open (the account's answer `a` triggers it, see
  `battlepass_ui_account_answered`). Until then it says `Loading Battle Pass` (v10's own text, L248039).
- Native path: `krunker_net_send_account_frame` (client/src/net/krunker_net.c) sends `bpdl` over the open game
  socket; the answers (`bpdl`, `bppd`) reach `kio_frame` -> `battlepass_ui_frame` (client/src/ui/account_ui.c).

**Box Office.** kn_brain's social socket: `bpdl` -> `sh_bp_data` (services/brain/season.c) reads the active
`battlepasses` row of `accounts.current_season` (services/brain/db_season.c:64; `SEASON_CURRENT` 7,
shared/include/season.h:25; the only seeded row is season 7, services/db/migrations/*/0108_seasons.sql).

## Frames (v10.0.3 socket labels and their handlers)

| Frame | Direction | Label (L) | Handler (L) | Payload / meaning |
|---|---|---|---|---|
| `bpdl` | window -> server | `bpdl` (L111084, B `"bpdl"` not literal in game.js) | `DATA_LOADED` (L111344) -> `windows[5].dataLoaded` (L255599-255600, L247815) | the pass object (`id, season, name, bpp_perlevel, itemcount, tier1price, tier1name, skipprice, startdate, enddate, itemdata, leveldata, krdata, jnkdata`); `null` = no pass |
| `bppd` | server -> window | `SYNC_BP_PROG` (L111346; B 5857771) | L255602-255606: `account.BP = progress`, then `syncBPProg` (L247806) | `{bpp, tier, items_claimed}` |
| `bpclm` | window -> server | L111085 | `claimItem` / `freeclaimItem` (L248109-248117) | `[id]` |
| `bpclmAll` | window -> server | `BP_CLAIM_ALL` (L111100) | `claimAllItems` (L248119-248121) | none |
| `bpclmr` | server -> window | `CLAIM_ITEM_RESPONSE` (L111148) | `claimItemRes` (L248052) | the claimed items |
| `xstbp` | window -> server | L111088 | `selectBPTier` (L248246-248257; `window.selectBPTier` L243315-243326) | `[tier, lang, null, null, channel, giftName?]`. A real purchase (Xsolla, L248256-248257) |
| `xstbpl` | window -> server | L111087 | `skipLevels` (L243372-243386; L248150) | `[levels, lang, null, null, channel]`. A real purchase |
| `purbp` | server -> window | `PURCHASE_RESPONSE_2` (L111146) | `purchaseRes` (L248266) | error or the tier |
| `purbpl` | server -> window | `PURCHASE_LEVEL_RESPONSE` (L111147) | `purchaseLvlRes` (L248160) | error, levels, bpp |
| `a` | server -> client | `ACCOUNT_RESPONSE` (L111206) | the account's own data: `account.BP.bpp` = data slot 41 (L129008-129009) | the account (not the pass) |

## Element table

Columns: the element (v10.0.3 markup, L), its data field, the request that fills it, what the official
krunker.io server sends, what Box Office sends, and what KN shows on the official server.

| # | Element (v10.0.3) | Source line | Data field | Request | Official krunker.io | Box Office (kn_brain) | KN on the official server |
|---|---|---|---|---|---|---|---|
| 1 | Header "Battle Pass" | L247763 (`header`), L247764 (`label: battlepass`) | none | none | none | none | "Battle Pass" (KN constant; v7's `windows.battlepass.header` is missing from en.js) |
| 2 | Home / Challenges tabs (`.bigMenTabs`) | L247842-247850 | `tabIndex` | none | none | none | local |
| 3 | Season name (`.bpSeasonH`) | L247880-247882 | `data.name` | `bpdl` | `bpdl` payload | `battlepasses.name` | shown |
| 4 | "Season ends in:" countdown (`#bpCountdownTimer`) | L247884 (B `bpCountdownTimer` 822723, 974683); timer L248359 | `data.enddate` | `bpdl` | `bpdl` payload | `battlepasses.enddate` | shown, counts down; empty when no end date (L247883) |
| 5 | "BP XP: x / need" bar (`.bpXPBar`) | L247890-247894 | `account.BP.bpp` (level table below) | `bppd` | `bppd` `bpp` | progress row | shown |
| 6 | "Level n/N" / "Battlepass Complete" | L247887-247888; level function L162007 (per-level table L161987) | level(bpp) from `leveldata` / `bpp_perlevel` / `itemcount` | derived | derived | derived | shared `bp_level` (shared/src/season.c) |
| 7 | "Available BPP" bar + cap tooltip | L247869-247870 (cap = (floor((now − startdate)/86400000) + 1) × 500, avail = max(cap − bpp, 0)); L247896-247899 | `data.startdate`, `bpp` | derived | derived | derived | shown (`bp_cap_total`, `bp_available`) |
| 8 | Free row and VIP row, reward cards (`.bpCard`, `.bpCardSlot`) | L247913, L247983, L247990-247997 | `itemdata` quartets `id,tier,col,sk`; `krdata` / `jnkdata` as `id:value` | `bpdl` | `bpdl` `itemdata` / `krdata` / `jnkdata` | the same fields | shown for items with a reward (`bp_item_has_reward`); a skin shows its image, a KR or junk reward its amount |
| 9 | VIP state: "VIP Tier Purchased" or "Purchase VIP Tier" (`.bpBtn.claim`) | L248005 | `account.BP.tier` | `bppd` | `bppd` `tier` | progress row | shown |
| 10 | Purchase VIP Tier -> popup "Purchase VIP Battle Pass", "Unlock vip rewards", price button | popup L248316-248338; price L248325 (`this.data["tier1price"]`), button L248336 (`$${tier1price}`) | `tier1price` | none (popup only) | USD: "$4.99" | KR: "N KR" | "$" + tier1price (USD); no creator code (v10.0.3 has none) |
| 11 | Price button in the purchase popup (`selectBPTier(this, 1)`) | L248246-248257 (Steam L248254; Xsolla L248256-248257) | tier | `xstbp` | a real purchase (Xsolla/Steam): refused, no frame; "Purchase Error" (v10's `purchaseError`, L243651) | `xstbp` [1, null] -> the brain, KR | REFUSED (no frame). Shows "Purchase Error" in the popup |
| 12 | Gift VIP Tier (`giftPop`), "Gift Battlepass" form, SEND GIFT | L248006, L248340-248351 (`selectBPGift`), form L243353-243370, `xstbp` with the name L243326 | recipient name | `xstbp` [1, lang, null, null, channel, name] | a real purchase: refused, "Purchase Error" | `xstbp` with the name -> the brain | REFUSED (the form closes with "Purchase Error") |
| 13 | Skip levels (`.bpBtn.skip`) -> popup: "Skip Battle Pass level", -/+, price button `#bp-skip-levels` | L248006-248007; popup L248211-248229; `increaseSkip` L248123-248133 (max L248127); price constant L19421 (B `levelSkipPrice` 2229584 = L22679) | `itemcount`, `battlepassLvl` | `xstbpl` [levels, lang, null, null, channel] (L243386, L248150) | "$0.99" per level, total `(0.99 × n).toFixed(2)` (L248129-248132); the buy is refused | `xstbpl` [levels] -> the brain, KR | popup prices "$0.99" per level; the buy is REFUSED ("Purchase Error") |
| 14 | Reward card CLAIM (`.bpClaimB`, `claimItem`) | L248109-248117 (`bpclm`), L248119-248121 (`bpclmAll`), answer L248052 (`claimItemRes`) | card `id` | `bpclm` [id] / `bpclmAll` | a claim changes the krunker.io account: not sent; the message "Claiming a reward is not sent to krunker.io from Krunker Box Office." | `bpclm` / `bpclmAll` -> the brain | REFUSED (message popup; the card stays claimable) |
| 15 | Claimed mark (`.bpClaimed`) and `items_claimed` | L247992; L248103 | `BP.items_claimed` | `bppd` | `bppd` `items_claimed` | progress row | shown |
| 16 | "No Battle Pass Found" | L248039 (`dataStatus == 2 && BP`) | `bpdl` null | `bpdl` | `bpdl` null (the season has no pass) | no active `battlepasses` row for the season | shown only for an answer with no pass |
| 17 | "Loading Battle Pass" | L248039 (`dataStatus` 0 or 1) | none | `bpdl` sent | not yet answered: no game session open | not yet answered | shown; never a failure text (KN removed "Could not load the Battle Pass") |
| 18 | Challenges tab (daily and weekly cards, `.chalCard`) | L248029 (text), L247825 (`genDailyCard`) | `account.activeChallenges` (`a` slot 38); `chg` / `uchp` | `a`, `chg`, `uchp` | the account's list | the brain's list | the account's list (`account_challenges`) |
| 19 | End-of-game pass section (`#endBPLayout`, "Unlock VIP") | L236530-236558 (`tmpBPData`) | `tmpBPData` (`bpp`, `cbp`, `re`) | the game server's end data in v10 | `bpEnd` (brain) | not held by the KN official game server: shows nothing (a gap) |
| 20 | Sidebar item ("Battlepass", level) | L162042-164095 (`$battlepassData`) | `account.BP` | `bpdl` / `bppd` | same | same | out of this window's scope (v10_rewards.c `bp_item_update`) |

## Gaps and open points

- **Official pass outside a game.** The menu has no krunker.io socket in v10.0.3 (L261682 is the only
  `connect`), so the pass shows `Loading Battle Pass` until an official game session opens. Once it is open, the
  account's answer triggers `bpdl` and the pass appears (tested: `client/tests/kio_battlepass_e2e.py`). A live
  check of the menu-time behaviour on krunker.io was not made (no production requests).
- **Official purchases and claims are not sent.** They spend real money (Xsolla / Steam) or change the krunker.io
  account. The window refuses them with v10's own purchase error or a message, at once.
- **Box Office "Battle Pass not found".** kn_brain answers `bpdl` with the season's row. The seeded row (migration
  0108) is season 7 and `SEASON_CURRENT` is 7, but v10.0.3 is season 10 (doc/v10/changelog-10.0.3.md: "Season 10
  Battlepass with 80 levels"). So a Box Office server with no season-10 row answers `null`, and the window says
  "No Battle Pass Found". Seeding a season-10 row is a content decision and is not done here.
- **End screen.** The official server's end-of-game pass section (row 19) has no source in KN's official client.
- The v10.0.3 window's `bpCountdownTimer` / `bpEndsIn` timers (L248359) match the port's countdown.
