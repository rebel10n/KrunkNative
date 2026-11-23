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
| 7 | Turf Wars "applied" clans (my-applications) | turf_ui.c `gen_recruit` | GET /api/clans/my-applications (krunker_kio_gapi_path: /clans/my-applications, no token); Box Office ?token= | gapi `/clans/my-applications` | krunker.io gapi with the Bearer while the account has no clan; its clan_ids read Applied (was: not sent, v10.0.2 had no route; v10.0.7 has getMyApplications) | brain | mismatch (fixed) | kio_clan_e2e B; krunker_kio_routes_test 6b; deob-10.0.7.js:26907, 145543-145550 |
| 8 | Turf Wars Recruitment Apply and staff Take Down | turf_ui.c `turf_ui_handle` (`on_kio_apply`, `on_kio_takedown`) | POST /clans/<id>/apply (no body), DELETE /clans/<id>/recruitment | gapi | krunker.io gapi with the Bearer (krunker_kio_gapi_write); Apply adds the clan to the applied set, Take Down drops the post from the board, an error shows on the board, as v10 (was: not sent) | brain | mismatch (fixed) | kio_clan_e2e B; krunker_kio_routes_test 6c; deob-10.0.7.js:26903-26912, 145520-145534, 145742-145762 |
| 9 | Market Browse (listings) | client/src/ui/v10_market.c `kio_browse_load` / `kio_browse_done` | GET /market/listings | gapi | krunker.io gapi (was: "No listings available") | economy socket (brain) | mismatch (fixed) | kio_market_e2e 1 |
| 10 | Market Stats (per-skin circulation, on sale, low / high) | v10_market.c `stats_load`, `kio_stats_done` | GET /market/stats | gapi | krunker.io gapi (v10 rows: ski, inC, onS, low, high) | economy socket (brain) | mismatch (fixed) | kio_market_e2e 2. The freeze on Stats is another owner's; view_stats is untouched |
| 11 | Market Inventory | v10_market.c `inventory_load` / `kio_inventory_done`; kio_market.c `kio_market_inventory_rows` | GET /market/inventory | gapi | krunker.io gapi: rows {i, st, sid, cnt, m}, a row with cnt 0 dropped (deob-10.0.7.js:134361), each keeping its official sid for the writes; Quick Sell values from the item (market_qs_value) (was: the brain's page, an empty list) | economy socket (brain) | mismatch (fixed) | kio_market_e2e (My Inventory); kio_market_test; deob-10.0.7.js:26949, 109077-109084, 134356-134368 |
| 12 | Market Sales / History | v10_market.c `sales_load` / `kio_sales_done`, `history_load` / `kio_history_done` | GET market/sales, GET /market/history?sort=date_desc&limit=50 | gapi | krunker.io gapi; history rows {type, date, skinindex, amount, other_party_name, status} (kio_market.c) (was: empty) | economy socket (brain) | mismatch (fixed) | kio_market_e2e (History); kio_market_test; v10.js 26867, 26948 |
| 13 | Market item detail (listings, sales, owners, statistics) | v10_market.c `kio_view_load` / `kio_view_done`, `detail_stats_load` / `kio_sales_stats_done` | GET /items/<i>/view (stats, owners, listings), /market/items/<i>/sales-statistics?daysback=, /items/<i>/sales?range= | gapi | krunker.io gapi (was: loading, then the load failure) | economy socket (brain) | mismatch (fixed) | kio_market_e2e (Listing Details, item page); kio_market_test; v10.js 26895, 26957, 26963 |
| 14 | Market Black Market | v10_market.c `bm_load`, `bm_items_done` / `bm_rank_done`, MODAL_BM | GET /market/black-market (owned list), /market/black-market/rank (peak_mmr); POST market/black-market/purchase | gapi | krunker.io gapi: the owned list and the peak rank on the tab's mount (since feddb562), the buy {item_index} (was: refused, "not in KrunkNative yet") | brain | mismatch (fixed) | kio_market_e2e (Black Market); deob-10.0.7.js:109167-109178, 137741-137751 |
| 15 | Market trades, list, buy, unlist, quick-sell | v10_market.c `kio_act_send` / `kio_act_answer`, `kio_quick_sell`, `kio_trade_action`, `kio_create_trade` | POST /market/purchase {market_id}, /market/list {item_id, price, skin_id}, /market/unlist {market_id}, /market/quick-sell {item_id, action, amount[, skin_id]}, /trades/create, /trades/<id>/accept, /trades/<id>/cancel, /trades/decline-all; reads GET /market/sales, /trades/history, /trades/player-search, /trades/inventory/<id> | gapi | krunker.io gapi with the Bearer (kio_store_write, as the Black Market buy and the lottery entry); the answers applied as v10.0.7 does (its notes, the lists reloaded, an error code shown as its errors.* text: kio_market_error_key) (was: listed as refused) | brain | mismatch (fixed) | kio_market_e2e (the popup's writes); kio_market_test; deob-10.0.7.js:26980-27000, 27160-27185, 109104-109108, 132698-132706, 134157, 134319, 137199-137269, 111517 |
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
| 31 | Clan window reads (MOTD, loot config, recruitment post) | clan_ui.c `fetch_motd`, `fetch_loot`, `fetch_recruit` | GET /clans/<id>/motd, /clans/<id>/loot-config, /clans/<id>/recruitment (v10_gapi; the id is clanData.clan_id from the game socket's "cln" 3, as v10's) | gapi | krunker.io gapi with the Bearer: the home tab's loot display and the settings tab's sections (was: the brain's routes, gated: nothing shown) | brain (loot config and recruitment: kn_brain HTTP ?token=; MOTD: the gapi tunnel) | mismatch (fixed) | kio_clan_e2e A; deob-10.0.7.js:26852, 26861, 26894, 96410-96430, 96680-96700, 96875-96891 |
| 32 | Clan window gapi writes (MOTD, loot config, banner colour, logo removal, recruitment post) | clan_ui.c `motd_send`, `save_loot`, `banner_apply`, `logoRemoveConfirmed`, `recruit_save`, `recruit_remove` | PUT / DELETE /clans/<id>/motd {message}, PUT /clans/<id>/loot-config {six fields}, POST /clans/<id>/banner-color {paint_item_id}, DELETE /clans/<id>/logo, POST / DELETE /clans/<id>/recruitment | gapi (krunker_kio_gapi_write: these paths only) | krunker.io gapi with the Bearer; answers applied as v10 (a 204 or a 4xx without an error is a success; MOTD and loot "last updated" from the answer, the banner colour the answer's, a logo removal asks for the clan again) (was: not sent) | brain (gapi tunnel; loot config and recruitment: kn_brain HTTP) | mismatch (fixed) | kio_clan_e2e A; krunker_kio_routes_test 6c; deob-10.0.7.js:26838-26905, 95963, 96195-96200, 96374-96402, 96654-96670, 27820-27885 |
| 33 | Community maps (published map data) | community_maps.c | GET /api/map-data?name= | brain | gated: not sent, the map falls back to the bundled copy | brain | no source | backend_http_get |
| 34 | Avatar | avatar_ui.c | GET/POST /api/account/avatar | brain | gated: not sent; no krunker.io avatar read in this client | brain | no source | avatar_ui.c 137-180 |
| 35 | Lottery (raffle) | v10_misc_popups.c | GET /api/lottery/<id> (krunker_kio_gapi_path: /lottery/<id>), POST lottery/<id>/participate | gapi | krunker.io gapi with the Bearer (was: not sent, the popup showed its load failure) | brain (lottery.c) | mismatch (fixed) | kio_market_e2e (lottery); krunker_kio_routes_test 6b; v10.js 26566-26571 |
| 36 | Guest Unclaimed Rewards | v10_rewards.c | GET guest profile (backend_http_get) | brain | gated: nothing shown | brain guest ledger | no source | v10_rewards.c 177 |
| 37 | Studios | v10_studios.c | gapi /studios/* | gapi | krunker.io gapi: the GET reads (/studios/ in kio_readable); writes not sent (was: nothing sent) | brain | mismatch (fixed) | kio_market_e2e (Studios); v10.js 27371-27480 |
| 38 | Krunker Strike menus (economy, missions, feedback) | strike_*.c | /v1/* on the Strike brain | strike brain | no krunker.io Strike: not sent | strike brain | no source | strike_econ.c |
| 39 | Web push subscription | web_push.c | /api/push/* | brain | not sent (browser only) | brain | no source | web_push.c 18-20 |
| 40 | Clan window game-socket writes and the logo upload | clan_ui.c `clan_kio_write` | create / join / leave / kick / role / funds / requests / Discord ("cln" 1-9, "clnR", "clnRA"), ownership transfer (rpc clan-transfer-initiate / -cancel / -respond), POST /clans/<id>/logo (multipart) | game socket; gapi | not sent: the official game socket's account bridge passes only the reads (krunker_net_send_account_frame), and the krunker.io request path carries JSON text, not v10's multipart FormData; the paint applied by a banner write is not taken off the krunker.io account's local inventory (account_remove_skin is the brain account's) | brain | mismatch (open) | deob-10.0.7.js:96104 (FormData), 220556-220642 (transfer rpcs) |

## Counts

40 screens: **ok 13** (rows 18-30), **mismatch (fixed) 19** (rows 1-15, 31, 32, 35, 37), **mismatch (open) 1**
(row 40), **no source 7** (rows 16, 17, 33, 34, 36, 38, 39; row 17 is ok on the official type and no source on
Box Office, as its Box Office side has no products).

## Not traced here

Junkyard, Trading guide, Catalog item pages, the in-game HUD windows (endscreen, scoreboard, chat) and the
Account Manager were not walked row by row; they are listed here as not traced so no status is claimed for
them.
