# Krunker v10.0.2 clans and Turf Wars (clan wars)

Source and line conventions as in `ranked.md` (`v10:N` = `~/.cache/kn-v10/v10.js`, v7 refs under
`/home/user/jails/kru/src/Krunker_v7.1.4/src/`). Guesses are marked (guess).

## Summary of change vs v7.1.4

- The v7 clan window (`windows[12]`, label "clan", `index.js:7236`) and its `cln` / `clnR` / `vcln`
  socket protocol are kept unchanged (same sub-op numbers 4-7, 9). Roles, `rankVar` clan level
  and the 10 % funds fee are the same.
- New: a 5th clan tab (settings icon) with Svelte components for clan logo (uploaded PNG, KR
  cost, admin approval), banner colour (from a paint item), message of the day, recruitment board
  post, Turf Wars loot distribution and ownership transfer. Most go over HTTPS to
  `gapi.svc.krunker.io /clans/*`; ownership transfer uses socket RPCs.
- New: **Turf Wars** (`territorialWars`), a daily clan-vs-clan kill race on 3 public maps that
  decides which clan "controls" each map, puts that clan's banner into the map and pays a daily
  KR pool. This is v10's only "clan war". The old `mode.clanWar` team flag (v10:170666, v7
  `data/players.js:124`) is still unused by any mode, and the store's "Clan Wars" item category
  (v10:102781) is a leftover label.
- v7's `clanwars` social route answers `{}` in KrunkNative (doc/backend.md:1205); v10 does not use it.

## Clan basics (unchanged from v7 unless noted)

| item | v10 | evidence | v7.1.4 |
| --- | --- | --- | --- |
| roles | Recruit; Soldier (max 25); Captain (max 5, perms 1); Commander (max 1, perms 2) | `v10:18613-18629` | identical `config.js:2636-2652` |
| tabs | home, Members, Stats, Requests, **settings** (5th) | `_7497` `v10:18612` | 4 tabs `config.js:2635` |
| settings tab access | owner (`clan_playerid == account.id`) or role 2 | `v10:213459` | new |
| clan level | `max(1, floor(0.03*sqrt(clan_score)))` (`rankVar`) | `v10:213468-213474` | same `index.js:7434` |
| funds fee | 10 % (`calcFee`) | `v10:213400-213410` | same |
| unlock | FTUE gates the Clans feature at level 20 (`'clans': {unlockLevel: 20}`) | `v10:197067-197070` | new (guess: menu gating only) |
| account data | new array slots 50 `clanLogo`, 51 `clanId`, 53 `clanBannerColor` (v10 account array is re-indexed vs v7) | `v10:112874-112879` | v7 `data/account.js:55-113` has no logo/id/banner |
| clan page | `openClanView(name)` routes to a Svelte clan profile (`getClanProfile(id)` / `getClanProfileByName(name)`: `clan_name, clan_logo, clan_score, creator_name, members[{p, role, su, ve, h}]`) with "Apply" | `v10:215716`, `v10:103600-103760` | v7 opened the social page |

### Socket packets (same as v7 `packets.json`)

| packet | direction | args | evidence |
| --- | --- | --- | --- |
| `vcln` OPEN_CLAN_TAB | C->S | - | `v10:213455`, v7 `packets.json:18` |
| `cln` CLAN | C->S | `(op, value, extra)`: 4 sub-op via `sendReq(4)`; 5 kick `(id)`; 6 role `(id, {r})`; 7 funds `(amount, {wd})`; 9 (v7: logo/settings value); `sendReq(n)` generic create/join/leave/delete | `v10:213338-213400`; v7 `index.js:7245-7350` |
| `clnR` CLAN_REQUEST | C->S | `(playerName, accept 0/1)` | `v10:213352` |
| `cln` CLAN_RESPONSE, `clnL` CLAN_LEAVE_RESPONSE | S->C | as v7 | `v10:97847`, `v10:97863` |
| RPC `clan-transfer-initiate (name)` / `clan-transfer-cancel` / `clan-transfer-respond (accept)` | C->S (`socket.rpc`) | `{error}` or `{data: ownership_transfer}` | `v10:213485`, `v10:213560-213570`; new |
| `warsClan` WARS_CLAN | S->C | `(clan_id, clan_name, clan_logo, clan_banner_color)` or `(null)` | `v10:186342`, `v10:218149-218163`; name only in v7 `packets.json:179` |

`clanData` gains `clan_id`, `ownership_transfer` (pending transfer banner component at the top
of the home tab, `v10:213476-213495`).

## Clan settings tab (new, `v10:95700-96900`, `v10:27618-27726`)

| section | behaviour | endpoint (gapi) |
| --- | --- | --- |
| Clan Logo | 512x512 PNG, max 256 KB, costs 20,000 KR from clan funds; status Approved / Pending Approval; only one pending upload; remove with confirm | POST `/clans/<id>/logo` (multipart `file`), DELETE `/clans/<id>/logo` |
| Banner Color | pick an owned paint item (count shown, "Apply (name)"); string colour or `s<N>` shader id | POST `/clans/<id>/banner-color {paint_item_id}` |
| Message of the Day | text, "No message set" | GET / PUT `{message}` / DELETE `/clans/<id>/motd` |
| Recruitment Board | post `{description, looking_for, min_level, min_kd, region, language}`; "Create Post (1,000 KR)" from clan bank; remove (re-post costs 1,000 KR) | GET / POST / DELETE `/clans/<id>/recruitment` |
| Turf Wars Loot Distribution | `{distribution_mode, clan_pct, top_player_count, top_player_pct, remainder_pct, min_games_required}`, defaults clan_bank / 0 / 20 / 80 / 20 / 1; shows "Last updated by ..." | GET / PUT `/clans/<id>/loot-config` |
| Transfer Ownership | player name, initiate / cancel; target accepts via banner | socket RPCs above |

Loot modes (`v10:96984-96990`, descriptions `v10:96530-96560`):

| mode | label | description | fields used |
| --- | --- | --- | --- |
| `clan_bank` | Clan Bank (default) | All rewards go to the clan treasury. | - |
| `even` | Even Split | split equally among eligible participants | clan_pct, min_games |
| `top_weighted` | Top Weighted | top players get a larger share | clan_pct, top_player_count, top_player_pct, remainder_pct |
| `performance` | Performance (by kills) | proportional to kills | clan_pct, min_games |
| `top_performance` | Top N by Performance | proportional among the top N | clan_pct, top_player_count |

Other clan endpoints (gapi): GET `/clans/recruitment?page&limit&region&language` ->
`{posts, total}` with `{recruitment_id, clan_id, clan_name, clan_logo, description, looking_for,
region, language, loot_mode, loot_clan_pct, loot_top_player_count, loot_top_player_pct}`;
GET `/clans/my-applications`; POST `/clans/<id>/apply`; GET `/clans/<id>/profile`;
GET `/clans/by-name/<name>` (`v10:26750-26840`, `v10:143826-143990`). The raw method list in
`~/.cache/kn-v10/endpoints.txt` mislabels some GETs as PUT; the methods above are from the
`'method'` fields.

## Turf Wars (new)

### Rules (server code in bundle `_63945`, `v10:186257-186456`; "How to Play" text `v10:142223`)

| rule | value | evidence |
| --- | --- | --- |
| intro date / day 0 | `TERRITORY_WARS_INTRO_DATE` "2026-03-17"; `day = max(0, floor((now - 2026-03-17T00:00Z)/86400000))` | `v10:16533`, `v10:186369-186376` |
| eligible maps | map index 0 Burg, 2 Sandstorm, 3 Subzero (`_63938 = [0,2,3]`) | `v10:186193` |
| eligible modes | mode index 0 FFA, 1 TDM (`_63939 = [0,1]`) | `v10:186194` |
| eligible games | `isPubType()` (no host, or host with `mimicPubs`); not a custom map | `v10:186360-186395`, `v10:187102` |
| counted kill | killer is not a bot, has an account and a clan, and the victim's clan differs (a clanless victim counts) | `v10:186398-186425` |
| record | per player `{playerId, clanName, kills, deaths}`; deaths counted only for clan members killed by another clan | same |
| flush | at game end `recordGameEndKills()` -> `game.onRecordTerritorialWarsKills(day, [{clan_name, player_id, map_id, kills, deaths}])` (server hook, posts to backend: guess) | `v10:186427-186456`, `v10:191283` |
| daily winner | clan with most kills on a map controls it next day; resets at 00:00 UTC | How to Play |
| top-20 cap | only the top 20 contributors per map per clan count | How to Play |
| reward pool | each day all kills on a map become a KR pool; top 3 clans per map split it `territorialWarsRewardSplit [0.6, 0.3, 0.1]` ("Split across the top 3 clans (60% / 30% / 10%)"); each clan distributes its cut by its loot config | `v10:16534`, `v10:142688-142700` |

Server-side details (pool size per kill, top-20 enforcement, payout job) are not in the bundle.

KrunkNative note on `mimicPubs` (the only way a hosted game is a pub game): v10 sets it from the game's
create object and forceDat (Zombies and Raid, maps 27 / 18, never a turf map); v10's Host Game UI has
no option for it. KN carries it from a hosted game's request, a Host Game request (`accounts.c`
`acc_host_apply`) or the operator's preset (`gameserver.game.mimicPubs`, `config.c` `gs_game_typed`),
through every round (`game_config_setup`). Deliberate KN decision (coordinator, user-approved): a
player's own Host Game request can set `mimicPubs`, so a player's mimicPubs game counts for Turf Wars
(and for the pub progress hooks). A published map's settings never set it (`game_config_from_map`).

### In-game effect

- On `init` (`v10:219253`) or `warsClan` packet, the client gets the controlling clan
  `{clan_id, clan_name, clan_logo, clan_banner_color}` and shows a one-time text popup
  "Turf Wars" / "Territory controlled by <clan_name>" (`v10:208134`, `v10:218161`).
- Banners: every map node whose `tag` starts with `wars_banner` (`serverNodes`) gets a dynamic
  model asset. Suffix after 11 chars selects the variant (`v10:186195-186235`):

| tag suffix | size | asset | decal position |
| --- | --- | --- | --- |
| `` (plain `wars_banner`) | [1, 3, 1] | 64295 | [0, 0.05, 0] |
| `_jap` | [1, 13.17, 1] | 64301 | [-0.15, 1.68, 0] |
| `_def` | [1, 13.17, 1] | 64303 | [-0.12, 1.65, 0.487] |

  Config: `src:"model", mSize 6, dyn, noCol`; paint = banner colour with mask `<asset>/model_p`;
  decal `clans/<clan_id>` from the clan logo asset, or a 200x200 canvas text of the clan name
  (font 100, centred) when there is no logo. Removed on reset / clan change; triggers
  `updateShadowMap()`. So v10 maps Burg/Sandstorm/Subzero need `wars_banner*` tagged nodes (see
  map-format.md).

### Turf Wars window (`_51011`, `v10:142360-144400`; menu button `menuBtnTurfWars`, icon flag, `v10:147047`)

| tab | content | endpoints (gapi) |
| --- | --- | --- |
| Overview | SVG territory map `./img/territory_map.webp` with per-map polygon (`burg`, `sandstorm`, `subzero` paths, label x/y), raised overlay `./img/turf/<mapId>-raised.png`, controller clan logo/name, today's kills, guaranteed / estimated reward, per-map top-clans list | GET `/territorial-wars/status` -> `[{map_id, map_name, clan_id, clan_name, clan_logo, kills, total_kills_today, guaranteed_reward, estimated_reward, leaderboard:[{position, clan_name, clan_id, clan_logo, kills}]}]` |
| My Clan | clan stats, players per day/map, earnings, payouts; "Join the Turf Wars!" if clanless | GET `/territorial-wars/clan/<id>`, `/clan/<id>/players?day&map_id`, `/clan/<id>/earnings`, `/clan/<id>/payouts?day` |
| Leaderboards | top performing clans (`day`, `map_id`, `clan_name` filters), top grossing clans, top performing players (`map_id`, `player_name`); columns Map / Position / Kills / Comparison | GET `/territorial-wars/leaderboards/top-performing-clans`, `/top-grossing-clans`, `/top-performing-players` |
| Recruitment | recruitment board browse with region/language filters, apply | `/clans/recruitment`, `/clans/<id>/apply` |
| How to Play | static rules text | - |

Player drill-down: GET `/territorial-wars/player/<id>`, `/player/<id>/contributions?day`,
`/player/<id>/earnings`.

## Port notes

- Keep the v7 clan socket protocol; add the settings tab and the gapi `/clans/*` routes to
  kn_brain (logo review, banner from paint inventory, MOTD, recruitment with 1,000 KR
  charge, loot config).
- Clan logo (KrunkNative, `services/brain/clan_logo.c`, migration 0332_clan_logos): the upload is
  POST `/clans/<id>/logo` with `{file: <base64 PNG>}` over the gapi tunnel (v10 sends the multipart
  `file`; the tunnel carries JSON). It runs the profile picture's pipeline (`avatar_image.c`
  decoder, `avatar_review_picture` in `avatar.c`, the same moderation profile and verdict rules).
  There is no separate admin queue: the AI verdict approves or rejects the logo, the same as for
  profile pictures. Approval moves `clan_logo_pending` into `clan_logo`. The approved image is GET
  `/api/clans/<id>/logo` (not v10's user-assets CDN). Deviations: the settings tab uploads as soon
  as a PNG is chosen (no preview step); a pending logo is shown as a status, not a picture;
  20,000 KR is taken from the clan bank only when an approval sets the logo (no fee,
  `clan_funds_ledger` source `clan-logo`), the same as the profile picture, which costs nothing: a
  rejected or failed review charges nothing and refunds nothing. An approval the clan can no longer
  pay for is not set (status error).
- Turf Wars needs: shared `territorialWars` tracker in the game (eligibility, per-player kill
  records, game-end flush to the brain), a daily brain job (winner per map, KR pool split
  60/30/10 then loot-config distribution), the `warsClan` packet and init field, `wars_banner*`
  node handling in map loading, and the Turf Wars window + territory map art.
- Assets referenced but local to the page (`./img/territory_map.webp`, `./img/turf/*`) are not on
  assets.krunker.io paths; they must be fetched or recreated (see ui.md).
