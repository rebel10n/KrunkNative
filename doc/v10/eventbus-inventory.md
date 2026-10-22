# Event bus inventory: Krunker v10.0.3 client vs KrunkNative

The friends and presence socket the v10 client opens with `ws?at=<token>` on the event-bus host
(`~/jails/kru/src/Krunker_v10.0.3/game.js`, decrypted and webcrack-deobfuscated for reading).
The client's `onopen` / `onmessage` / `onclose` and its send helper are the only uses of that socket
in the bundle: the five outbound labels below are the only callers of the send helper, and the
inbound switch is the one `switch (msg.type)` in `onmessage`. The 10.0.2 bundle (`~/.cache/kn-v10/v10.js`)
has the same switch and the same send sites, so the label set is unchanged from 10.0.2.

The social socket owner's companion, `doc/v10/social-socket-inventory.md`, lists the same frames with
the 10.0.3 byte offsets and the error codes. This file is the label inventory (direction, consumer,
handler, test) and adds the `requestBefore` finding below.

Where the bus is answered:

* **Box Office account**: `services/brain/social_eb.c` over the social socket, as `["eb", {...}]`
  frames (`social_ws.c:792` routes `eb` to `sh_eb`). Native client: `client/src/ui/v10_social.c`.
* **krunker.io account** (`backend_ui.h`): the client opens krunker.io's own bus
  (`client/src/net/krunker_eb.c`, `KRUNKER_SVC_EVENT_BUS`). The brain is not in that path; the
  client is the only consumer there, with the same `eb_dispatch`.

## Counts

* Outbound (client to bus): 5 labels, all answered by the brain.
* Inbound (bus to client): 10 labels (9 with an effect; `presence:pong` is a no-op in 10.0.3 and
  the native client), all sent by the brain and consumed by the native client.
* Labels the 10.0.3 client sends or expects that the brain or the native client lacks: **0**.
* Brain payload that 10.0.3 reads incorrectly: 1 (`msg:history:result.requestBefore`, fixed here).

## Inventory

Direction is from the client's point of view. "10.0.3 consumer" is the case or send site in the
bundle. "Our handler" is the brain (`services/brain/social_eb.c` unless noted) and the native client
(`client/src/ui/v10_social.c`). "Test" is the existing assertion (`services/brain/tests/`).

| # | label | direction | 10.0.3 consumer / sender | brain handler | native client | test |
|---|-------|-----------|--------------------------|---------------|---------------|------|
| 1 | `friends:list` `{offset}` | client to bus | sent on open (offset 0) and for each further page (offset = next) | `sh_eb` :779 to `friends_list` :276 | send `v10_social.c:576`, `:583` | `social_v10_test.py:159`, `:374-393` (paging) |
| 2 | `presence:ping` | client to bus | every 25 s (`0x61a8` ms) | `sh_eb` :771 | send `v10_social.c:3317-3319` (`EB_PING_MS`) | `social_v10_test.py:401` |
| 3 | `msg:send` `{to, text}` | client to bus | the composer's send | `sh_eb` :780 to `msg_send` :397 | send `v10_social.c:988-991` (`conv_send`) | `social_v10_test.py:194-204`, `:250-252` (chat ban) |
| 4 | `msg:history` `{peer, before, limit}` | client to bus | `before: 0` (first page), `before: oldest id` (earlier) | `sh_eb` :781 to `msg_history` :582 | send `v10_social.c:946-950` (`conv_open`), `:959-963` (`conv_load_earlier`) | `social_v10_test.py:208-222` |
| 5 | `msg:read` `{peer}` | client to bus | on opening a conversation and on new messages in it | `sh_eb` :782 to `msg_read` :609 | send `v10_social.c:847`, `:940` | `social_v10_test.py:234-238` |
| 6 | `friends:list` `{friends[], offset, hasMore}` | bus to client | `case 'friends:list'`: offset 0 replaces the list, else appends; `hasMore`; seeds conversations and unread | `friends_online` :200-237 (`eb_obj` :225); pushed on mutual follow by `refresh_list_of` :293 | `eb_on_friends` `v10_social.c:586-626`; dispatch `:830` | `social_v10_test.py:159`, `:235-238`, `:374-393`, unfollow checks |
| 7 | `msg:recv` `{id, from, text, ts}` | bus to client | `case 'msg:recv'`: dedupe by `id`, append, unread + 1 | `sent_done` :316-340 (`eb_obj("msg:recv")` :332) | `eb_on_recv` `v10_social.c:652-673`; dispatch `:831` | `social_v10_test.py:196-197` |
| 8 | `msg:sent` `{id, to, ts}` | bus to client | `case "msg:sent"`: marks the oldest pending bubble for `to` delivered | `sent_done` :327 | `eb_on_sent` `v10_social.c:675-687`; dispatch `:832` | `social_v10_test.py:195-197` |
| 9 | `unread:summary` `{counts: {<id>: {count, name}}}` | bus to client | `case "unread:summary"`: replaces unread counts, DM line per sender | `unread_done` :621-634 (`eb_obj` :631); on login `social_eb_after_login` :636, from `social_ws.c:570` | `eb_on_unread_summary` `v10_social.c:689-716`; dispatch `:833` | `social_v10_test.py:229-231` |
| 10 | `msg:history:result` `{peer, requestBefore, messages[{id, from, text, ts}], hasMore}` | bus to client | `case "msg:history:result"`: pending key `"<peer>:<requestBefore>"` (the request's `before`); messages oldest first | `history_done` :554-580 (`eb_obj` :571) | `eb_on_history` `v10_social.c:718-742`; dispatch `:834` | `social_v10_test.py:208-222` (fixed here) |
| 11 | `presence:pong` | bus to client | `case 'presence:pong': default: return` (no effect) | `sh_eb` :771-772 (also when signed out) | ignored, `v10_social.c:841` | `social_v10_test.py:401`, `:404` |
| 12 | `presence:online` `{id}` | bus to client | `case "presence:online"`: friend `online = true` | `presence_push` :655 via `social_eb_presence` :696, from `social_ws.c:174` | `eb_on_presence` `v10_social.c:744-753`; dispatch `:835` | `social_v10_test.py:232-233` |
| 13 | `presence:offline` `{id}` | bus to client | same case: `online = false`, clears `gameInstanceId` and `gamePasswordProtected` | `presence_push` (offline path, `social_ws.c:186`) | `eb_on_presence`; dispatch `:836` | `social_v10_test.py:225-226` |
| 14 | `presence:game` `{id, gameInstanceId \| null, gamePasswordProtected}` | bus to client | `case 'presence:game'`: sets or clears the game, password flag only when in a game | `game_presence.c:77-107` (`presence_frame`, pushed to friends on join / leave / node gone) | `eb_on_game` `v10_social.c:755-765`; dispatch `:837` | `strike_social_test.py:179-195`, `server/tests/v10_party_e2e.py` |
| 15 | `error` `{code, message}` | bus to client | `case "error"`: the EB_* message codes mark the oldest pending bubble failed; others show the banner (`errors.eventBus.*`) | `eb_error` :102-107; codes at :412 (`EB_TEXT_TOO_LONG`), :376 (`EB_NOT_FRIEND`), :378 (`EB_RATE_LIMITED`), :355 / :484 (`EB_TARGET_OFFLINE`), :320 / :374 / :401 / :408 (`EB_MSG_INVALID`), :270 / :557 (`EB_INTERNAL`), :776 (`EB_AUTH_REQUIRED`), :786 (`EB_UNKNOWN_TYPE`) | `eb_on_error` `v10_social.c:767-797`; dispatch `:838` | `social_v10_test.py:201-204`, `:407-408`, `:417`, `:252` |

Labels 1 to 15 cover every frame in both directions. Labels 6 to 10 and 14 carry the fields the
10.0.3 handlers read (`lastId`, `lastText`, `lastFromMe`, `lastTs`, `unread`, `counts[].count`,
`counts[].name`, `gameInstanceId`, `gamePasswordProtected`).

## Not on the bus in 10.0.3

Checked against every send site and every `case` of the bus switch (the bundle has one `new WebSocket`
for the bus, and its socket variable is referenced only in the send helper, `onopen` and `onclose`):

* **Friend requests**: no label. A follow is `POST /players/<name>/follow` (gapi/HTTP); a mutual
  follow reaches the bus only as `friends:list` (row 6) and `presence:*`.
* **Party**: no label. The party is `presence:game` (row 14) and the game socket.
* **Invites**: no label. Studio invites are HTTP (`GET /studios/invites`, `/studios/<id>/invites/<id>`).
  Krunker v10 has no game invite.

## KrunkNative-only labels (not in 10.0.3)

* `game:invite` (client to bus), `game:invite:sent` and `game:invite` (bus to client): Krunker
  Strike's invite (`social_eb.c` `game_invite` :518, `invite_online` :479; consumer
  `client/src/ui/strike_social.c`). The 10.0.3 client ignores them (`default` path).
* `friend:request` / `friend:accepted`: the native client still handles them
  (`v10_social.c:837-840`, `notify_friend`), but the brain no longer sends them (no `friend:request`
  in `services/brain/`). The follow notice is the Inbox's Followers entry (`social_eb.c`
  `follower_mail`). The comment at `v10_social.c:635` is stale; the handler is dead code and is left as is.

## Changes in this pass

* `services/brain/social_eb.c` `msg:history:result.requestBefore` now echoes the request's `before`
  as sent: `0` stays `0`, `null` stays `null`, an absent `before` gives no key. Before, every reply
  sent `null` for `before` 0 or absent, so the 10.0.3 handler's key `"<peer>:0"` (its first-page
  request, `before: 0`) did not match its reply's `"<peer>:null"`, and the pending request was never
  resolved. The 10.0.3 client sends `before: 0` on its first page, so this is the one payload defect
  found.
  Assumption: the official server echoes the request value (the handler's key construction makes
  no other choice work for `before: 0`).
* Tests: `services/brain/tests/social_v10_test.py:210-218`. Both new checks failed against the
  unmodified brain and pass after the fix.
* Native client: no change. The client never reads `requestBefore` (it keys by conversation).
