# Social socket rate limits

The social socket (`/social/ws`, `/api/ws`; `services/brain/social_ws.c`) carries every
inbound request of the Box Office client: the social hub, the friends event bus, the clan
window, the market, the account pages. This page is the one place where its limits and its
answers are written down. The code keeps the same numbers (`k_rate` in `social_ws.c`); the
test `services/brain/tests/social_ratelimit_test.py` fails when the two disagree.

## One bucket per class and connection

Each connection has one token bucket per message class (`SocialSession.rate[]`, one
implementation). A frame takes one token from the bucket of its class when it arrives. A bucket
starts full (its burst), refills at its rate and never holds more than its burst. A frame
that finds no token is refused.

Only the anti-hack reply `po` is exempt: it is the client's answer to our `pi`, costs nothing
and has no answer of its own. Every other inbound frame, including an unknown label or a frame
without a label, is charged to a class (unknown and malformed frames to `notice`).

Limits (per connection, per class):

| class | per s | burst | frames |
| --- | --- | --- | --- |
| `auth` | 0.5 | 10 | `a` (login, register, logout), `2fa`, `a2FA`, `a2FAc`, `d2FA`, `d2FAc`, `cemail`, `emailv`, `emailS`, `rpr`, `rpc`, `rps`, `upp`, `las` |
| `read` | 50 | 200 | gapi GET (any path), `eb` `friends:list` and `msg:history`, and the socket reads `r`, `sp`, `gfs`, `gfg`, `pst`, `guf`, `grp`, `gps`, `chtb`, `ctd`, `getS`, `ttl`, `dsp`, `krh`, `spinH`, `sst`, `chg`, `mdl`, `bpdl`, `ri`, `ecfg`, `st`, `kml`, `fil`, `uml`, `wo` |
| `write` | 20 | 100 | gapi POST, PUT, DELETE, `eb` `msg:send`, `msg:read` and `game:invite`, and the socket writes `f`, `ps`, `com`, `lps`, `dps`, `ict`, `kr`, `cln`, `clnR`, `clnRA`, `rpt`, `flg`, `ver`, `crt`, `sbdg`, `nme`, `uploadS`, `uploadM`, `uploadMo`, `uploadA`, `edMp`, `ulb`, `fvM`, `ftC`, `delC`, `li`, `uli`, `pi`, `qsi`, `ctr`, `trA`, `trC`, `trCA`, `srt`, `pbm`, `spin`, `fkr`, `clm`, `bdg`, `cbdg`, `bpclm`, `bpclmAll`, `xstbp`, `xstbpl`, `dsl`, `rml`, `raml`, `rqdxp` (the v10 2x XP claim), `wob`, `wor` (the wild offer purchase and refresh), `vr` (the voucher redeem); a route the table does not name is a write |
| `live` | 2 | 20 | `eb` `presence:ping` |
| `notice` | 5 | 20 | `sfkr`, `lfkr`, `fkrf`, `unbx`, `vcln`; any label that is no route; an `eb` type the table does not name; a frame without a label |

The burst of `write` (100) is larger than any sequence a person can produce, so a human never
meets the limit; it bounds a script's flood. The queue (below) keeps a connection's requests in
order, so a flood cannot run ahead of the work it has already started.

## Every inbound frame is answered

No inbound frame is dropped without an answer. Which answer a frame gets:

| frame | passes its bucket | refused by its bucket |
| --- | --- | --- |
| gapi request `["gapi", rid, method, path, body]` | its handler's `["gapi", rid, status, ...]` answer (404 `NOT_FOUND` for an unknown path) | `["gapi", rid, 429, {"error": {"code": "RATE_LIMITED", "message": "Rate limited, retry in N ms.", "retry_ms": N}}]`, at once or in order |
| eb request `["eb", {type, ...}]` with a known type | its handler's answer; `presence:ping` gets `presence:pong` | `["eb", {"type": "error", "code": "EB_RATE_LIMITED", "message": "Rate limited, retry in N ms.", "retry_ms": N, "of": "<type>"}]`, at once or in order; `msg:read` gets the window notice instead |
| eb request with an unknown type | `["eb", {"type": "error", "code": "EB_UNKNOWN_TYPE", "message": "Unknown event-bus type.", "of": "<type>"}]` | the window notice (eb error, `EB_RATE_LIMITED`, `of` the type) |
| `a` (auth request) | the auth answer | `["a", "Rate limited, retry in N ms.", null, null, null, null]`, the auth error shape |
| other socket request with a known label | its handler's answer (`["po", null, msg]` on an error) | `["po", null, "Rate limited, retry in N ms."]`, the page's error popup |
| label that is no route | `["po", null, "Unknown request."]` | the window notice: `["po", null, "Rate limited, retry in N ms."]` |
| frame without a label (not an array, no string first element, or not JSON) | `["po", null, "Malformed request."]` | the window notice (`notice` class) |
| `["po", ...]` (anti-hack reply) | no answer, by design | not limited |

`retry_ms` is the time until the bucket holds its next token. The client must wait that long
before it sends again; it must not resend in a loop.

Each refused frame with a reply slot, and each passing unknown or malformed frame, gets its own
answer. Unknown and malformed frames are charged to the `notice` class, so a flood of them gets at
most one burst of "Unknown request." / "Malformed request." replies and then one notice per window
(below).

A passing fire-and-forget frame (`sfkr`, `lfkr`, `fkrf`, `unbx`, `vcln`, `msg:read`) is accepted
and has no reply: that is its label's contract (the client never waits for it). It is not dropped.

### Fire-and-forget frames: one notice per refusal window

A refused frame with no reply slot (`msg:read`, and the `notice` class: fire-and-forget labels,
unknown labels, frames without a label) gets one notice per refusal window instead of one answer per
frame. A window opens with the first such frame that is refused, and it is closed by the next frame
of its class that passes (a passing unknown label is answered "Unknown request.", and it closes the
window like any passing frame). Refused frames inside an open window get no
answer of their own, because the notice that opened the window already said when to retry. A
flood of refused fire-and-forget frames therefore yields one notice; after the retry time the
next frame of the class passes and closes the window, and a later flood opens a new one.

This is the one place where a refused frame is not answered individually. It is deliberate:
the frames carry nothing the client waits for, so one notice per window is the explicit answer
the client needs, and a notice per frame would only flood the client back.

## Coalesced state pushes (server to client)

A push that carries the whole current value of one thing replaces the same push still waiting
for the socket. Each socket keeps a map from key to the newest frame. A key is sent once, in its
first position, `OUT_FLUSH_MS` (100 ms) after the first push of the burst, or at once when the map
holds `OUT_MAX` (128) keys.

| key | frame |
| --- | --- |
| `presence:<id>` | `["eb", {"type": "presence:online" \| "presence:offline", "id"}]` |
| `game:<id>` | `["eb", {"type": "presence:game", "id", ...}]` |
| `friends` | `["eb", {"type": "friends:list", ...}]` pushed (not a reply) |
| `gmc` | `["gmc", unreadMail]` pushed |
| `ua` | `["ua", data, info]` |

Other pushes are sent at once: a chat-list change (`chtb`, one row per sender, so not a full value),
a challenge result (`chg`, `chgC`), a badge (`nbdgr`), a battle pass update (`bppd`). Replies are never
coalesced: each request gets its own answer.

Outbound pushes are not charged to the connection's buckets. The client cannot slow them down,
and a key already bounds their rate: one frame per key per flush.

## Inbound state requests

Inbound requests are not coalesced. A repeated `presence:ping` is answered each time (the
`live` class lets a client ping far more often than the 25 s the page uses), and a repeated
`msg:read` is one database update per frame, bounded by the `write` class.

## Connection state and the queue

* A request holds its socket until its database work is done (`social_hold`): a frame that arrives
  meanwhile waits in the socket's queue (`QUEUE_MAX` 128), and the queue runs in order.
* A passing frame that finds the queue full is refused with `retry_ms` 250 (`QUEUE_RETRY_MS`). It
  is answered as above, never dropped.
* Unknown and malformed frames are answered in the same queue order as refusals.
* This table covers the router. A handler that returns without an answer for a request it does not
  accept (for example a guest's request that needs a login, or a 2FA code with no login waiting) is
  not changed by it; those are open items for the socket's owners.

## Limits elsewhere

These sit behind the socket limit and are unchanged: the per-account write limits of the hub
(`write_ok` in `social_gapi.c`, `clan_v10.c`, `studios.c`: posts, votes, comments, follows, clan
applications, shared across brains through `hot_limit`), the login and register limits of
`auth.c` per address, and the chat's own limit of 30 messages per sender and minute
(`EB_RATE_PER_MIN`). A request the socket lets through may still be refused by one of them, with
its usual error.

## Client

* `client/src/ui/v10_gapi.c`: a 429 is an ordinary gapi error; its message is shown. No retry.
* `client/src/ui/v10_social.c`: `EB_RATE_LIMITED` with `of` = `msg:send` fails the oldest pending
  message, as before. With another `of`, it clears the loading flag of that list (`friends:list`
  or `msg:history`) and shows the message, so nothing waits forever. Neither retries by itself.

## Tests

`services/brain/tests/social_ratelimit_test.py` (sqlite and postgres): the table in the code and
in this page agree, every route label has a class; a flood of fire-and-forget frames yields one
notice, the next frame after `retry_ms` passes, and the next flood yields one notice again; an
unknown-label flood gets its burst answered "Unknown request." and one notice, and after the retry
time an unknown label is answered again; frames without a label (an object, an empty array, a
non-string label, text that is not JSON) are each answered "Malformed request."; an unknown eb type
gets EB_UNKNOWN_TYPE; a gapi burst answers every rid once (200 or 429); a ping flood answers every
ping; `po` frames are never limited; a login flood answers every `a`; a friend's presence flapping
within one flush window reaches the friend once, with the newest value.
