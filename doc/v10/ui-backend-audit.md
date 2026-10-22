# UI backend audit: which server each screen reads

Krunker Box Office's native UI, screen by screen: the request each screen makes, its host, and which
backend answers it on an official krunker.io game (the official type) and on the Box Office type
(kn_brain, krunk.io). Rule: on the official type every screen reads krunker.io, with no Box Office data
and nothing invented; a screen with no krunker.io source shows v10's own empty state. Box Office's
behaviour is unchanged by these rows.

The gate: `backend_http_get` / `backend_http_request` refuse every brain request while krunker.io is the
active backend (`backend_set_brain_gate`, account_ui.c `brain_gated`). A `v10_get` path with no gapi route
(krunker_kio_routes.c `krunker_kio_gapi_path`) is never sent, so its answer is status -1.

Status: **ok** = the official type reads krunker.io (or no request is made and the screen is static), and
the Box Office type reads the brain. **mismatch (fixed)** = was wrong, fixed here. **mismatch (open)** = still
wrong; the reason is in the row. **no source** = krunker.io has no route for it; the screen shows v10's empty
state (or none).

Hosts: `gapi` = gapi.svc.krunker.io (v10 `_10449`, KRUNKER_SVC_API, the `/api` base of the stubs);
`api` = api.krunker.io (KRUNKER_SVC_DATA / webhooks / streams); `mm` = matchmaker; `social` = social.krunker.io;
`brain` = kn_brain (the Box Office account's server).

## Screens

| # | screen | file | request | host | official (krunker.io) | Box Office | status | evidence |
|---|---|---|---|---|---|---|---|---|
| 1 | Turf Wars Overview (territories, today's clans) | client/src/ui/turf_ui.c `gen_overview` | GET /api/territorial-wars/status | gapi `/territorial-wars/status` | krunker.io gapi (was: "Failed to load turf wars data") | brain | mismatch (fixed) | kio_turf_e2e 1; krunker_kio_routes_test 6b |
| 2 | Turf Wars My Clan (own clan's stats) | turf_ui.c `gen_my_clan`, `my_clan_key` | GET /api/clans/by-name/<tag>, then /api/territorial-wars/clan/<id>[/players, /earnings, /payouts] | gapi `/clans/by-name`, `/territorial-wars/clan/<id>` | krunker.io gapi; the clan id from by-name (v10 clanId) | brain (tag) | mismatch (fixed) | kio_turf_e2e 3 |
| 3 | Turf Wars clan view (another clan, a clan name click) | turf_ui.c `clan_stats` | GET /api/territorial-wars/clan/<id>[...] | gapi | krunker.io gapi | brain | mismatch (fixed) | kio_turf_e2e (route) |
| 4 | Turf Wars player view | turf_ui.c `gen_player_view` | GET /api/territorial-wars/player/<id>[/contributions, /earnings] | gapi `/territorial-wars/player/` | krunker.io gapi (route mapped) | brain | mismatch (fixed) | krunker_kio_routes_test 6b |
| 5 | Turf Wars Leaderboards (clans / grossing / players) | turf_ui.c `gen_boards` | GET /api/territorial-wars/leaderboards/* | gapi | krunker.io gapi | brain | mismatch (fixed) | kio_turf_e2e 2 |
| 6 | Turf Wars Recruitment (posts) | turf_ui.c `gen_recruit` | GET /api/clans/recruitment?page&limit&region&language | gapi `/clans/recruitment` | krunker.io gapi | brain | mismatch (fixed) | krunker_kio_routes_test 6b |
| 7 | Turf Wars "applied" clans (my-applications) | turf_ui.c `gen_recruit` | GET /api/clans/my-applications?token= | none | no route in v10: not sent, the list is empty | brain | no source | v10.js has no my-applications route |
| 8 | Turf Wars Apply / recruitment edits | turf_ui.c `turf_ui_handle`, clan_ui.c | POST /api/clans/<id>/apply, recruitment writes | brain | not sent (writes not ported for krunker.io, kept) | brain | no source | turf_ui.c "krunker.io account: applying ... not ported" |
| 9 | Market Browse (listings) | client/src/ui/v10_market.c `kio_browse_load` / `kio_browse_done` | GET /market/listings | gapi | krunker.io gapi (was: "No listings available") | economy socket (brain) | mismatch (fixed) | kio_market_e2e 1 |
| 10 | Market Stats (per-skin circulation, on sale, low / high) | v10_market.c `stats_load`, `kio_stats_done` | GET /market/stats | gapi | krunker.io gapi (v10 rows: ski, inC, onS, low, high) | economy socket (brain) | mismatch (fixed) | kio_market_e2e 2. The freeze on Stats is another owner's; view_stats is untouched |
| 11 | Market Inventory | v10_market.c `request_page("market","inventory")` | economy frame (socket) | brain socket | v10 has GET /market/inventory (gapi) but no KN row mapping yet: empty list | brain | mismatch (open) | v10.js 26862 |
| 12 | Market Sales / History | v10_market.c | economy frames | brain socket | v10 has GET /market/sales, /market/history (gapi): not mapped yet, empty | brain | mismatch (open) | v10.js 26867, 26948 |
| 13 | Market item detail (listings, sales, owners, statistics) | v10_market.c `detail_listings_load`, `detail_stats_load` | economy frames ["r","market",..], ["st", ...] | brain socket | v10 has GET /items/<i>/view, /market/items/<i>/sales(-statistics) (gapi): not mapped yet, empty | brain | mismatch (open) | v10.js 26887-26895, 26957 |
| 14 | Market Black Market | v10_market.c | economy frames | brain socket | v10 has GET /market/black-market (gapi): not mapped yet | brain | mismatch (open) | v10.js 26924 |
| 15 | Market trades, list, buy, unlist, quick-sell | v10_market.c `kio_refused` | POST writes | brain socket | read-only notice, nothing sent (kio_store.h) | brain | no source | writes are not ported for krunker.io |
| 16 | Store home: bundle grid (bundleCols) | store_ui.c `v10_store_home` | none (bundles are the game's ALL_BUNDLE_DATA packet) | none | empty, as v10 is without a bundle | empty: the brain has no bundles | no source | v10.js ALL_BUNDLE_DATA -> `_69845`; brain has no bundle table |
| 17 | Store Eterno Shop (limited displays, products) | store_ui.c, junk_shop_ui.c, kio_store.c | GET /api/shop/displays?tag=limited | gapi `/shop/displays` (kio_store, KRUNKER_SVC_API) | krunker.io's display (kio_store_display_html) | brain display: no products in shop_products (the migration seeds none), so v10's "No offers right now" | no source (Box Office: no products); ok (official) | kio-store-e2e 2-4 |
| 18 | Store Prize Wheels | store_ui.c `v10_store_wheels` | none (wheel tables from the client's store data) | none | static client data; spins read-only on krunker.io | same static data | ok | kio-store-e2e 3 (spin read-only) |
| 19 | Store Purchase KR / Redeem Keys / Premium | store_ui.c, kio_store.c | POST purchase, voucher | none sent on krunker.io | read-only notice | brain | ok | kio-store-e2e 3 |
| 20 | Store Ranked shop | ranked_ui.c, kio_store.c `kio_store_ranked_shop` | GET ranked/shop with Bearer | api `/ranked/shop` | krunker.io's ranked shop | brain `/mm/ranked/shop` | ok | kio-store-e2e 3, ranked-krunker-e2e |
| 21 | Battle Pass | battlepass_ui.c | krunker.io battle pass read | api | krunker.io progress | brain | ok | kio-spin-battlepass-e2e |
| 22 | Daily Spin | v10_daily_spin.c | GET /daily-spin/progress | api (KRUNKER_SVC_DATA) | krunker.io progress; the claim is the game socket's, not sent | brain | ok | kio-spin-battlepass-e2e |
| 23 | Ranked (menu, queue, profile, leaderboard, history) | ranked_ui.c, krunker_ranked.c | /ranked/*, /leaderboards/rankings/*, queue socket | api, gapi, matchmaker | krunker.io with the account's Bearer | brain `/mm/ranked/*` | ok | ranked-krunker-e2e |
| 24 | Leaderboards (players, clans, raids, KrunkCup, ranked) | v10_competitive.c `v10_get` | GET /api/leaderboards/* (mapped to gapi /leaderboards/*) | gapi | krunker.io gapi | brain | ok | krunker_kio_routes_test (leaderboard paths) |
| 25 | Servers window (lobbies, maps, ping) | servers_ui.c, krunker_lobbies.c | /mm/game-list, /mm/ping-list, /api/player-count | mm, social | krunker.io lobbies (KRUNKER.IO OFFICIAL LOBBIES) | brain `/mm/*` | ok | krunker-official-e2e |
| 26 | Games window (maps, mods, search) | social_ui.c | /maps, /mods, /search (webhooks) | api `/webhooks/general/*` | krunker.io webhooks (krunker_kio_webhook_url) | brain `/api/maps`, `/api/search` | ok | krunker_kio_routes_test |
| 27 | Main menu streams (Top Streams) | v10_menu.c | GET /streams/top?cb=2 | api | krunker.io streams (Bearer when signed in) | brain `/streams/top` | ok | v10_menu.c 601-623 |
| 28 | Menu news and events | v10_news.c | GET /social/news, /creators/events | gapi | krunker.io gapi | brain | ok | v10_news.c 265-272, 896-897 |
| 29 | Social hub (feed, players, follow, clans, profile) | v10_social.c | GET /social/feed, /players/*, /clans/* (gapi) | gapi | krunker.io gapi (reads); writes not sent | brain socket | ok | v10_gapi.c kio_readable |
| 30 | Account window, header, Junk, level | account_ui.c `active_account` | the game's "a" / "ua" frames | game socket | krunker.io's account (kio_build) | brain account | ok | krunker-account-e2e, account-unify-e2e |
| 31 | Clan window (MOTD, loot config, recruitment, logo) | clan_ui.c | GET /clans/%d/motd, /api/clans/%d/loot-config, /clans/%d/recruitment (backend_http_get) | brain | gated: nothing sent; v10 has GET /clans/<id>/motd, /loot-config, /recruitment (gapi) | brain | mismatch (open) | v10.js 26763-26806 |
| 32 | Clan Turf banner, loot config and transfer writes | clan_ui.c | POST / PUT / DELETE on the clan | brain | not sent (writes not ported) | brain | no source | clan_ui.c |
| 33 | Community maps (published map data) | community_maps.c | GET /api/map-data?name= | brain | gated: not sent, the map falls back to the bundled copy | brain | no source | backend_http_get |
| 34 | Avatar | avatar_ui.c | GET/POST /api/account/avatar | brain | gated: not sent; no krunker.io avatar read in this client | brain | no source | avatar_ui.c 137-180 |
| 35 | Lottery (raffle) | v10_misc_popups.c | GET /api/lottery/<id> | brain | not sent: the answer is -1 and the popup shows its load failure | brain (lottery.c) | mismatch (open) | v10_misc_popups.c 521 |
| 36 | Guest Unclaimed Rewards | v10_rewards.c | GET guest profile (backend_http_get) | brain | gated: nothing shown | brain guest ledger | no source | v10_rewards.c 177 |
| 37 | Studios | v10_studios.c | gapi /studios/* | gapi | not sent: /studios/ is not in kio_readable | brain | mismatch (open) | v10_gapi.c kio_readable |
| 38 | Krunker Strike menus (economy, missions, feedback) | strike_*.c | /v1/* on the Strike brain | strike brain | no krunker.io Strike: not sent | strike brain | no source | strike_econ.c |
| 39 | Web push subscription | web_push.c | /api/push/* | brain | not sent (browser only) | brain | no source | web_push.c 18-20 |

## Counts

39 screens: **ok 13** (rows 18-30), **mismatch (fixed) 8** (rows 1-6, 9, 10), **mismatch (open) 7** (rows 11-14,
31, 35, 37), **no source 11** (rows 7, 8, 15, 16, 17, 32-34, 36, 38, 39; row 17 is ok on the official type and
no source on Box Office, as its Box Office side has no products).

## Not traced here

Junkyard, Trading guide, Catalog item pages, the in-game HUD windows (endscreen, scoreboard, chat) and the
Account Manager were not walked row by row; they are listed here as not traced so no status is claimed for
them.
