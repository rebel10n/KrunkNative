# v10.0.3 social socket inventory

Every frame the Krunker v10.0.3 client sends on, or handles from, its social socket, against
KrunkNative's social socket (`services/brain/social_ws.c`, `social_eb.c`, `game_presence.c`) and the
native client (`client/src/ui/v10_social.c`). Gapi-over-socket (`["gapi", ...]`, 45 tunnel routes) is
not in this table: it is `doc/v10/api-inventory.md` and `social_gapi.c`.

## Method

- Consumer reference: the v10.0.3 bundle `~/jails/kru/src/Krunker_v10.0.3/game.js`, decoded with
  webcrack 2.16 (`unpack`, `deobfuscate`, `unminify`). The installed `isolated-vm` is built for another
  Node ABI, so the string-array decoder ran in `node:vm` (no globals, 10 s timeout). The decoded
  output is `deobfuscated.js`; offsets below are into it. The string literals and event-bus cases are
  readable there; identifiers stay mangled.
- The sockets the bundle constructs (`new WebSocket(...)`, five sites) were checked one by one. Only
  one of them is social. See "Sockets" below.
- KrunkNative side: `services/brain/social_ws.c` (`k_routes`, `k_eb_class`), `social_eb.c` (`sh_eb`
  and the emitters), `game_presence.c` (`presence:game`), and the native client's event-bus switch
  `eb_dispatch` in `client/src/ui/v10_social.c`.

## Sockets the v10.0.3 client opens

| socket | URL | carries | social socket? |
| --- | --- | --- | --- |
| game connection | `//<host>:<port>/ws?gameId=&clientKey=&clientUID=&bpt=&tc=&at=&dm=` (`iîiìiîi`, offset ~3953670) | game packets (msgpack, `ahNum`), and the account labels its handler table dispatches (see "Game connection labels" below) | no: the game server |
| event bus | `${EVENT_BUS_URI}/ws?at=<fresh token>` (offset ~6343979) | friends, presence, direct messages, unread counts, errors | **yes: the only social socket** |
| matchmaking queue | `${gamefrontend}/v1/matchmaking/queue?token=&maps=&regions=` (offset ~3968428) | queue for a match | no |
| gapi factory | `createWebSocket` in the gapi client class (offsets ~959323, ~1016485) | nothing: no caller opens it | no |

KrunkNative's social socket (`/api/ws`, `/social/ws`) is its own: the official client has no
counterpart socket, so its frames map onto the event bus (`["eb", {type, ...}]`, doc/v10/social.md).

## Event bus: frames the v10.0.3 client sends and handles

Direction is from the client. "Consumer" is the v10.0.3 site (decoded offset). "KN handler" is where we
read the frame; "KN emitter" is where we send it. "Native client" is `client/src/ui/v10_social.c`.
The status column is the state before this pass; nothing was missing.

| # | label | dir | v10.0.3 site | KN side (file:line) | native client (file:line) | test (services/brain/tests) | status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `friends:list` | out | send on open, `offset: 0` (6344363); paging, `offset` (6360453) | in: `sh_eb` 765 -> `friends_list` (social_eb.c:276); out: `friends_online` 225 | 576, 583 (send); 830 (recv) | social_v10_test.py:159 | handled before |
| 2 | `presence:ping` | out | every 25 s (6344500) | in: `sh_eb` 757-758 | 3319 | social_v10_test.py:377 | handled before |
| 3 | `msg:send` | out | 6411203 | in: `sh_eb` 766 -> `msg_send` (social_eb.c:394) | 988 | social_v10_test.py:194, v10_social_e2e.py | handled before |
| 4 | `msg:history` | out | 6359246 (`before`, `limit`, `peer`) | in: `sh_eb` 767 -> `msg_history` (571) | 946, 959 | social_v10_test.py:208 | handled before |
| 5 | `msg:read` | out | 6358214 (`peer`) | in: `sh_eb` 768 -> `msg_read` (595) | 847, 940 | social_v10_test.py:227 | handled before |
| 6 | `friends:list` | in | case 6344861 | out: `friends_online` (social_eb.c:225-228, `friends`, `offset`, `hasMore`) | 830 -> `eb_on_friends` | social_v10_test.py:160 | handled before |
| 7 | `msg:recv` | in | case 6347747 | out: social_eb.c:332 (`sent_done`, the recipient copy) | 831 | social_v10_test.py:196 | handled before |
| 8 | `msg:sent` | in | case 6349316 | out: social_eb.c:327 (`sent_done`) | 832 | social_v10_test.py:195 | handled before |
| 9 | `msg:history:result` | in | case 6350959 | out: social_eb.c:562 (`msg_history`) | 834 | social_v10_test.py:209 | handled before |
| 10 | `unread:summary` | in | case 6350027 | out: social_eb.c:617 (`unread_done`, sent after login by `social_eb_after_login`, 622) | 833 | social_v10_test.py:222 | handled before |
| 11 | `presence:pong` | in | `case "presence:pong":` then `default: return` (6353589) | out: social_eb.c:758 (the reply to 2) | not read (ignored, as v10's default) | social_v10_test.py:378 | handled before |
| 12 | `presence:online` | in | case 6353666 | out: social_eb.c:645 (`presence_push`), pushed on sign-in | 835 | social_v10_test.py:225 | handled before |
| 13 | `presence:offline` | in | case 6353702 | out: social_eb.c:645 (`presence_push`), pushed on sign-out or close | 836 | social_v10_test.py:218 | handled before |
| 14 | `presence:game` | in | case 6354216 | out: game_presence.c:77-104 (`presence_frame`, pushed to friends) | 837 | strike_social_test.py:179 | handled before |
| 15 | `error` | in | case 6354652 | out: social_eb.c:102-107 (`eb_error`) | 838 | social_v10_test.py:200 | handled before |

Counts: 15 directional frames (5 out, 10 in), 14 distinct type strings (`friends:list` both ways).
Before this pass: 15 of 15 handled on our side. Native client: 14 of 15 read; `presence:pong` is
ignored, which v10 does too.

### Error codes (`error` frames)

v10.0.3 handles five codes inline, for a pending message bubble (the rest open the error popup with
the message). Our emitters for each:

| code | v10.0.3 | KN emitter (social_eb.c) |
| --- | --- | --- |
| EB_TARGET_OFFLINE | inline (6354881) | 355 (msg:send, offline friend), 477 (invite, KN-only) |
| EB_NOT_FRIEND | inline (6354921) | 373 (msg:send), 501 (invite, KN-only) |
| EB_RATE_LIMITED | inline (6354957) | 375 (msg:send), 525 (invite, KN-only); the limiter (social_ws.c:1022) |
| EB_TEXT_TOO_LONG | inline (6354995) | 409 |
| EB_MSG_INVALID | inline (6355034) | 320, 371, 374 (a chat ban: "You are banned from chat.", feature_bans 'chat'), 401, 408 |
| EB_AUTH_REQUIRED | popup | 762 (not signed in) |
| EB_UNKNOWN_TYPE | popup | 772 (and social_ws.c:1022 for an unknown route) |
| EB_INTERNAL | popup | 270, 548 (database errors) |
| EB_INVITE_INVALID | popup | 497, 521 (KN-only) |

## KrunkNative event-bus types with no 10.0.3 counterpart

These are KrunkNative extensions. The v10.0.3 client never sends them and ignores them (the default
branch of its switch). They stay as they are.

| label | dir | KN side | native client |
| --- | --- | --- | --- |
| `game:invite` | out and in | `sh_eb` 769 -> `game_invite` (social_eb.c:511); the friend gets `game:invite` (social_eb.c:479) | strike_social.c |
| `game:invite:sent` | in | social_eb.c:484 (the sender's copy) | strike_social.c |
| `ranked:queued` | in | `ranked_notice_social_event` (ranked_notice.c:8), fanned out to JSON social sockets (ranked.c:222) | `v10_social.c` 841 (`eb_dispatch`'s `else return`: no notice) |
| `friend:request`, `friend:accepted` | in | no emitter in services/ (the native client's `eb_dispatch` branches 839-840 are dead) | notify_friend |

## Game connection labels (not the social socket)

The game connection's handler table dispatches account and social labels too (deobfuscated offsets
~11420864 to ~11424000, the same block as the game connect at ~11419340). They are game-server
frames and are not on the social socket, so this pass does not cover them. Some share names with
KrunkNative social-socket labels: `a` (ACCOUNT_RESPONSE), `ua` (UPDATE_ACCOUNT), `kml` (ADD_MAIL),
`rml` (OPEN_MAIL), `gmc` (UPDATE_MAIL), `gfrnd` (FRIENDS_LIST_RESPONSE), `nmeR` (ALIAS_RESPONSE),
`upp` (NEW_PASS_RESPONSE), `cln` (CLAN_RESPONSE), `clnL` (CLAN_LEAVE_RESPONSE), `vr`
(REDEEM_VOUCHER_RESPONSE), `recps` (CRAFT_DATA_RESPONSE), `dslr` (DAILY_SPIN_CLAIM_RESPONSE). A
same-named KN label is the KN page's own protocol (the v7-derived social socket), so the names are
not a parity claim.

## Result

| | count |
| --- | --- |
| official social-socket labels (directional frames) | 15 (14 distinct types) |
| handled by KN before this pass | 15 |
| implemented in this pass | 0 |
| empty by design (no KN source, empty reply) | 0 |
| KN-only event-bus types the brain emits (no 10.0.3 counterpart) | 3 (`game:invite`, `game:invite:sent`, `ranked:queued`) |
| native-client branches with no brain emitter | 2 (`friend:request`, `friend:accepted`, `v10_social.c` 839-840) |

Nothing new was needed, so no new label, brain handler, native handler or test was added.
