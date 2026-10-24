# v10.0.2 Social hub on the v10 page (v10-social)

Krunker v10's in-game Social hub (`window.openSocial(tab)`, Svelte `_49999` in a `socialModal`
generic popup), its view stack (player profile, clan view, post detail, compose, follow list,
gift, report) and the friends event bus, ported to C (`client/src/ui/v10_social.c`) over kn_brain.
The clan window (13) is `client/src/ui/clan_ui.c` (see clans.md). Line numbers `v10:N` refer to
`~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js`.

## Client

| piece | v10 | KrunkNative |
| --- | --- | --- |
| Social menu item | `_50477`, `#menuBtnSocial`, `counterBadge` = unread total | `v10_social_tick` sets `onclick="openSocial()"` on the item and keeps the badge |
| hub | `_49999` (v10:140481): sidebar tabs Social / Friends / Search | `open_hub` / `render` into `#genericPop.socialModal` |
| Social tab | feeds global / following / hot / giveaways / clan + News (v10:140540), New post (level 10), player search | `sb_social_tab`, gapi `/social/feed?tab=`, `/social/news`, `/social/post` |
| post card / detail / compose | `_35330`, `_35483`, `_49624` | `sb_post`, `sb_post_detail`, `sb_compose`; votes optimistic like v10 |
| Friends tab | `svelte-u33r8v` / `e2g8p2` / `128yn5i` / `jj9ljg`, event bus (v10:139770-140300) | `sb_friends_tab`, `sb_friend_list`, `sb_conversation` |
| Search tab | `svelte-fo44ss`, 300 ms debounce, `/players/search?q&limit=20` | `sb_search_tab` |
| player profile | `_34510` (v10:109702) | `sb_profile` (Stats, Feed, Sales, Badges, Titles, Creations, Match History, Reports) |
| clan view | `_34484` (v10:103555), Request to Join | `sb_clan`, gapi `/clans/by-name/<tag>`, `/clans/<id>/apply` |
| view stack | `.view-stack > .view-stack-overlay > .view-wrapper` scaled by `--ui-scale` | the page's one stack (`v10_view_push` / `v10_view_set` / `v10_view_pop`, client/include/v10_ui.h); `menu_scale_base` publishes `--ui-scale/--ui-width/--ui-height` on `<body>` |
| window 5 Friends tab | `#friendList` rows, Join (`openURL('/?game=..',1)`), Remove Friend (`deleteFollower`) | `v10_social_window_friends_html` |
| in-game DM lines | `[DM from <name>]: <text>`, `[DM] N new messages from <name>` in `#chatList` | `dm_chat_line` (only in game) |

KrunkNative adds `v10s*` element ids (no style) for focus/scroll restoration and tests.

## Transport (deviation)

v10 calls gapi over HTTPS with a FRVR bearer token and opens a second WebSocket for the friends
event bus (`wss://event-bus.svc.krunker.io/ws?at=<token>`). KrunkNative's client already holds the
authenticated social socket, so both ride on it:

* gapi: `["gapi", rid, method, path, body]` -> `["gapi", rid, status, {data} | {error: {code, message}}]`
  (`services/brain/social_gapi.c`, `client/src/ui/v10_gapi.c`). Requests are rate limited per class
  and connection, a refused one answered with a 429 `RATE_LIMITED` (doc/social-rate-limit.md); the
  client takes any 429 as v10.0.3's gapi wrapper does: no data and "Rate limit exceeded. Try again
  later." (deob-10.0.3.js:30051-30057). The bodies were checked route by route against the 10.0.3
  call sites and consumers (doc/v10/api-inventory.md, the shape column).
* event bus: `["eb", {type, ...}]` both ways, v10's objects verbatim (`services/brain/social_eb.c`):
  `friends:list`, `presence:ping/pong/online/offline/game`, `msg:send/sent/recv/read/history`,
  `msg:history:result`, `unread:summary`, `error` (EB_* codes; an unknown type is EB_UNKNOWN_TYPE with
  10.0.3's "Chat client is out of date. Please refresh."). Checked against 10.0.3 frame by frame
  (deob-10.0.3.js:156539-157813): every field it reads matches. KrunkNative's `game:invite` (Strike) is
  not a 10.0.3 type; its refusals carry `of: "game:invite"`, so the client never fails a pending
  message on them. A message's length is counted in UTF-16 units on both sides, as 10.0.3's
  `text.length <= 512` (157704-157705); a comment is cut at 500 UTF-16 units (the reply box's
  maxlength, 120813), not bytes. Friends are mutual follows (v10's
  rule); a follow that makes or breaks a friendship pushes both friends lists. Messages are
  `chat_messages` rows (the hub chat's), online only like v10 (EB_TARGET_OFFLINE). Online = a
  signed-in social socket on any brain (Redis presence, `hot_presence_online`).
* gift / report / sales / listings use the existing socket ops (`kr`, `rpt`, `pst`, `uml`).
* the frame-by-frame check against v10.0.3 (every event-bus frame sent and handled, with the sites
  on both sides): `doc/v10/social-socket-inventory.md`.
* chat bans: a `feature_bans` row with feature `chat` in force (revoked_at NULL, expires_at NULL or
  later) refuses the account's `msg:send` with `EB_MSG_INVALID` ("You are banned from chat."), an
  inline code, so the bubble fails as v10.0.3 expects, and its social chat line `ict` with `chf` false.
  Staff write that row with `POST /moderation/chat {player_name, ban}` (`v10_mods.c`, moderation_log);
  `GET /moderation/chat/:name` reads `chat_banned`, which the v10.0.3 consumer uses for the staff
  button label (deob-10.0.3.js:126002-126004, 254172-254207).

## The krunker.io account (official games)

While krunker.io is the active account backend (`backend_ui.h`: an official krunker.io game) the hub,
the profile, the clan view and the Clans window show the krunker.io account's real data, fetched as
v10 fetches it:

* gapi reads go to krunker.io's gapi (`v10_gapi.c` -> `krunker_account_api(KRUNKER_SVC_API)`, v10's
  `_10449 = new _10077(GAPI_URI)`, v10:27600) with the account's fresh Bearer token, answers unwrapped
  as `_10067` (v10:26094). Only GETs of `/players/`, `/social/`, `/clans/`, `/territorial-wars/` are
  sent; the profile's Sales tab uses v10's `getSales(name, rangeIndex)` / `getListings(name)`.
* the friends event bus is v10's own socket (`client/src/net/krunker_eb.c`:
  `<EVENT_BUS_URI>/ws?at=<fresh token>`, natively through the client's proxy with Origin
  https://krunker.io, the browser's WebSocket on the web, none on Windows) with v10's reconnect
  backoff. Friends list, presence, history, direct messages and read receipts work as in v10.
* the Clans window's `vcln` / `["cln", 3]` go over the official game socket (account bridge);
  its settings tab reads `/clans/<id>/motd | loot-config | recruitment` over gapi.
* not ported (they change the krunker.io account or spend its KR): follow / unfollow, likes,
  comments, posts, Gift KR, Report, Trade, clan apply, every Clans window write, Turf Wars apply /
  takedown, window 5's Remove Friend. Their buttons are hidden as for a v10 guest, the handlers do
  nothing and `v10_gapi` does not send them. The Turf Wars popup's reads stay kn_brain's.
* tests: `server/tests/krunker_social_e2e.py`.

## Giveaways (0112_social_v10)

`POST /social/post {type: 9, text, funds}`: level 20, at least 10 KR, the author pays funds +
ceil(10 %). The draw is not in the bundle: KrunkNative draws one entrant (liked the post and follows
the author) uniformly at random when it ends (`accounts.giveaway_hours`, default 24; checked every
`accounts.giveaway_check_ms`), pays the prize and mails the winner; without entrants the prize goes
back to the author. News: `accounts.news` (HTML strings).

## Known differences from v10

* No profile pictures, clan logos or emoji skins are hosted: avatars use v10's fallback picture,
  the clan view its shield placeholder, and the DM emoji panel never has candidates (`:` is inserted).
* Profile: raid stats, match history, badges / titles tabs and ranked rank chip show v10's empty
  states; the sales chart is a bar strip instead of v10's canvas line chart; listings show the skin
  index instead of its name / thumbnail; Trade dispatches `openTrade` (the economy's flow).
* Post types 5 (unboxed item) and 8 (match result) are rendered by the brain only if such posts exist
  (KrunkNative creates none yet).
* Join in the conversation header calls `v10_party_join_game` (v10-party); without it, nothing.
* Load earlier messages is the button only: 10.0.3 also loads them when the list is scrolled within
  80 px of the top (deob-10.0.3.js:157784-157790).
* Reports: `rp_text` is sent raw on the gapi path (10.0.3 puts it in a text node, deob-10.0.3.js:126950)
  and the client escapes it; `rp_date` is ISO UTC. The socket's `["grp"]` -> `["rps"]` keeps the
  HTML-escaped text.
