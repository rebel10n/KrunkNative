# The account split: krunker.io logins and Box Office logins

The rule: a krunker.io login is used only on an official krunker.io game, and a Box Office
(krunk.io, kn_brain) login only on a Box Office server. Nothing one side's login is (a name, an
email, a password, a token, a session, a row) reaches the other side's requests or screens.

The one predicate (every path asks it; nothing compares backends by hand):

- `client/include/account_split.h`, `client/src/ui/account_split.c`
  - `account_split_refuses(login)`: 1 when the name or email is known to belong to the other backend
    and not to this one (case-blind; a name known on both sides is left to the connected server, by
    the password).
  - `account_split_row_usable(krunker_row)`: an Account Manager row is used only on its own side.
  - `account_split_krunker_identity()`: 1 while a krunker.io account is signed in AND an official
    krunker.io game is the session. This is the only krunker.io identity a request may carry.
  - `account_split_side_of(login)`: the side a name is known to belong to (-1 unknown); a helper.

The card's side is always the connected server's backend (`account_backend()`, backend_ui.h). The
card never takes its side from a row or a typed value.

## What the client knows, and what it cannot

- The client knows a name's side only when it holds that name: the signed-in krunker.io account
  (its name), the signed-in Box Office account, or an Account Manager row (either kind).
- A krunker.io login the client has never signed in or listed is unknown to it, and so is its
  address (the client keeps none). Typed on the Box Office card it reaches kn_brain, which refuses
  it because kn_brain holds no such account (it has no krunker.io accounts to match). The hard
  guarantee on this side is the token rule below.
- The brain cannot know krunker.io accounts, and krunker.io's servers are not in this repository:
  "a Box Office login is never accepted by krunker.io" is enforced here by never sending a Box
  Office token or identity to a krunker.io host (audit rows 27-29), and the reverse is krunker.io's
  own check.

## Audit (client)

Before = the code as of origin/main b0f6e960. Now = this change. "Crossed" = can one side's
login, token or identity act on the other side's request or screen.

| # | Path | file:line (now) | Crossed before | Crossed now | Note |
|---|------|-----------------|----------------|-------------|------|
| 1 | Box Office login card, typed Username -> kn_brain | `account_ui.c:1989`, `:2010`, `:2017` | yes | no | a known krunker.io name is refused before the send (`splitKrunkerIo`) |
| 2 | Box Office login card, typed Email -> kn_brain | `account_ui.c:1989` (mail), `:2017` | yes | no | refused when the address is a known krunker.io name or row |
| 3 | Box Office register card, typed name / email -> kn_brain | `account_ui.c:1989`, `:2000` | yes | no | `splitRegister`; registering a Box Office name a krunker.io account holds is refused |
| 4 | krunker.io login card, typed Username -> krunker.io sign-in | `v10_login.c:470` (split check), `:476` before the sign-in | yes | no | a Box Office name is refused (`splitBoxOffice`), no krunker.io request |
| 5 | krunker.io login card, typed Email -> krunker.io sign-in | `v10_login.c:470`, `:476` | yes | no | same, when the address is a known Box Office name or row |
| 6 | krunker.io card, Email code button | `v10_login.c:589-592` | no (krunker card only) | no | now guarded: `if (!kio()) return` |
| 7 | krunker.io card, register | `v10_login.c:446-455` | no | no | "create it on krunker.io"; no request |
| 8 | Box Office recover card (email) -> kn_brain reset | `v10_login.c:565-575` | no | no | not a sign-in; the brain mails only a Box Office account's address |
| 9 | krunker.io recover card | `v10_login.c:560-563` | no | no | `kio()` only |
| 10 | Account Manager Use of a krunker.io row on a Box Office server | `alt_manager_ui.c:204-219` (`account_split_row_usable`) | yes (card opened with the krunker.io sign-in) | no | refused: "krunker.io accounts are used in an official krunker.io game." |
| 11 | Account Manager Use of a Box Office row in an official game | `alt_manager_ui.c:204-219` | no (the card opened as a Box Office card on an official server; its Login was dropped silently by the brain gate, `account_ui.c:1971`) | no | now refused with a message: "Box Office accounts are used on a Box Office server." |
| 12 | Account Manager row "active" marks | `alt_manager_ui.c:122-123` | yes (a krunker.io sign-in shown as the active account on Box Office; a Box Office session shown active in an official game) | no | each side's row is active only on its own server |
| 13 | Account Manager Add / Remove | `alt_manager_ui.c:256-280`, `:225-252` | no (local vault; Remove signs out only the local session) | no | no request; Remove of a krunker.io row in an official game still refuses (`kn.alt.inGame`) |
| 14 | Live Streams: source choice | `v10_menu.c:611-612` | yes (a signed-in krunker.io account chose api.krunker.io on a Box Office server) | no | now `account_split_krunker_identity()` |
| 15 | Live Streams: krunker.io Bearer | `v10_menu.c:617` | yes (the krunker.io Bearer went to api.krunker.io from Box Office) | no | Bearer only on an official game; the guest read stays |
| 16 | Live Streams: brain read | `v10_menu.c:618` (`backend_http_get`) | no | no | Box Office's own |
| 17 | Ranked route (who answers ranked) | `ranked_route.c:6-9`; `ranked_ui.c:109-111` | no (krunker.io only when `official`) | no | already split |
| 18 | Ranked krunker.io reads / purchase / claim | `ranked_ui.c:246` (`kio_request`), callers `:280`, `:765`, `:998`, `:1270` | no (every caller behind `ranked_active_backend()` == KRUNKER) | no | now also gated by the predicate (defence in depth) |
| 19 | Ranked queue socket | `ranked_ui.c:737` (`start_kio_queue`); `krunker_ranked.c:609-635` | no (behind the official route) | no | now gated by the predicate |
| 20 | `krunker_ranked_request` itself | `krunker_ranked.c:143-167` | yes if called ungated (it attaches the krunker.io Bearer whenever signed in) | no | only callers ever reach it; all are gated (rows 14, 18) |
| 21 | Friends event bus (krunker.io's socket) | `v10_social.c:873`, `:882`, `:889` (kio_connect), `:531`, `:552`, `:3312` | no | no | `kio()` gates the open; the Bearer is `?at=` only in `kio_connect` |
| 22 | Social hub, player lists, friends | `v10_social.c:333`, `:343`; `social_ui.c:3678` | no | no | `kio()` / `account_backend()` gates |
| 23 | Window gapi reads | `v10_gapi.c:171` | no | no | krunker.io's gapi only in an official game |
| 24 | Shop / Eterno reads, purchase | `kio_store.c:64`, `:734`, `ranked_ui.c` (`:345`) | no | no | gated |
| 25 | Clan reads / writes | `clan_ui.c:354`, `:373`, `:438` (krunker), `:432`, `:1250` (Box Office token) | no | no | krunker.io reads gated by `kio_clan()`; the brain's token is "" on krunker.io (`account_ui.c:525`) |
| 26 | Player count, maps, search, Popular Now | `servers_ui.c:3214`, `:3647`, `:4165` | no | no | `account_backend()` gates |
| 27 | Box Office session token -> krunker.io | `account_ui.c:525` (`account_session_token`) | no | no | empty unless Box Office is the backend; the brain's Bearer goes only to the brain (`backend_common.c:50-64`) |
| 28 | Box Office identity in krunker.io requests (krunker_net, seek, game socket) | `krunker_net.c:1898-1964`, `:2233`, `:2373` | no | no | the official game only; the krunk.io session is not sent |
| 29 | Signed-in identity display: header, Account window | `account_ui.c:440-442` (`active_account`), `:259-265` | no | no | backend-aware; `kio_account()` only under KRUNKER |
| 30 | Strike FTUE storage key | `strike_ftue.c:34` | yes (a krunker.io name keyed a Box Office screen's local state) | no | local only; now `account_split_krunker_identity()` |
| 31 | Krunker.io sign-in refresh / profile at startup | `backend_startup.c:124` -> `krunker_account.c:806-815` (refresh to FRVR), `:449` (`/player/me`) | no | no | a Box Office run with the krunker.io account still signed in sends no Bearer to krunker.io (`account_manager_e2e.py` `kio_idle`, passes on the new build) |
| 32 | Email code request (krunker.io) | `v10_login.c:589` | no | no | guarded now (row 6) |
| 33 | user-assets.krunker.io media fetch | `user_assets.c:236` (services/brain) | no | no | anonymous GET, no credential |

## Audit (brain, kn_brain)

| # | Entry point | file:line | Identity | krunker.io token accepted |
|---|-------------|-----------|----------|---------------------------|
| B1 | session routes (`/auth/me`, `/auth/refresh`, `/auth/join`, `/auth/logout`) | `services/brain/auth.c:88-100` (`req_token`), `accounts.c:709-711`, `:519-548` (`token_lookup_done`) | a 32-hex session token, matched by `sha256(token)` in `sessions.token_hash` | no: a JWT matches no row (`accounts.c` `token_lookup_done` answers "Invalid login.") |
| B2 | login / register | `auth.c:163-193`, `accounts.c:682-707` | username or email + password | no: a krunker.io token is not a password |
| B3 | social socket login | `social_ws.c:545`, `:610-632` | name + password, or a session token | no |
| B4 | internal WS (game servers) | `internal_ws.c:351` (`kn_signed_token_verify`, HS256 under `join_secret`), `:726` (`brain.internal_secret`) | a join ticket (HMAC) or the internal secret | no |
| B5 | `/mm/seek-game`, `/mm/host` | `public_routes.c:246-269`, `:398`, `:486` | a session token; a bad one degrades to a guest | no (a guest) |
| B6 | guest routes | `guest.c:98-99` | a 32-hex guest id, no credential | n/a |
| B7 | email verify / reset | `account_security.c:465`, `:797`, `db_security.c:14` | email token or reset code | no |

The brain never calls krunker.io or FRVR with a token (the only krunker.io-host call is the anonymous
`user_assets.c:236`). `services/brain/tests/account_split_test.py` pins the rule. It passes on the
baseline brain too: the brain already refused every krunker.io token, so no brain code changed. The
client's refusals are what the e2e shows failing before (see the test notes in the report).

## Changes (this commit)

- `client/include/account_split.h`, `client/src/ui/account_split.c`: the predicate.
- `client/src/ui/v10_login.c`: `v10_login_fill(name, pass)` (the card's side is the server's, not the row's);
  krunker.io `kio_submit` refuses a Box Office login (`:470`); the email-code handler is the krunker.io card's only (`:589`).
- `client/src/ui/account_ui.c:1989-2017`: the Box Office card refuses a known krunker.io name or email
  before the brain send (login and register).
- `client/src/ui/alt_manager_ui.c:122-123` (row active), `:204-219` (Use refuses the other side).
- `client/src/ui/v10_menu.c:609-617`: the Live Streams source and Bearer use `account_split_krunker_identity()`.
- `client/src/ui/ranked_ui.c:243-246` (`kio_request`), `:280`, `:737` (queue), `:765`, `:998`, `:1270`.
- `client/src/ui/strike_ftue.c:34`: the local storage key follows the predicate.
- `client/include/v10_menu.h`, `client/include/menu_sources.h`: signatures and comments.
- `ui/i18n/kn/en.json`: five `kn.*` strings (`kn.account.splitBoxOffice`, `kn.account.splitKrunkerIo`,
  `kn.account.splitRegister`, `kn.alt.useKrunkerIo`, `kn.alt.useBoxOffice`), by `i18n_lint.py --write-kn`.
- `services/brain/`: no code change (see the brain audit: the brain already refuses every krunker.io token).
- Tests: `client/tests/account_manager_e2e.py` (split refusals, Use on the other side, the idle Bearer check),
  `services/brain/tests/account_split_test.py` (new).

## Limits

- Names and addresses the client does not hold are not recognised on the card (row 1-5 notes); the
  brain's token rule still stops any krunker.io token.
- A name held on both sides is not refused by either card: the connected server decides by the
  password.
- krunker.io's own refusal of Box Office tokens lives on krunker.io's servers, outside this repository.
