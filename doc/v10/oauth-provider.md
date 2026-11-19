# Login with Box Office, Discord and Twitch links (Krunker v10.0.7)

Krunker v10.0.7 added three account-link features to Settings > Connections: "Login with Krunker" with a
**Connected apps** list (deob-10.0.7.js:27656-27696 the API, :97493-97673 the component, :219174 the mount), a
**Discord** row that can be in a *deletion pending* state (:26499-26523, :97250-97490), and a **Twitch reconnect**
flag on the drops answer (:218880-218924). Box Office runs all three on kn_brain for Box Office accounts:

| Feature | Brain | Client | Tests |
| --- | --- | --- | --- |
| Login with Box Office (OAuth 2.0 provider) | `services/brain/oauth.c` | `client/src/ui/account_links.c` (Connected apps) | `services/brain/tests/oauth_test.py` |
| Discord link | `services/brain/discord.c` | `account_links.c` (Discord row) | `services/brain/tests/account_links_test.py` |
| Twitch tokens + reconnect | `services/brain/account_life.c` | `account_ui.c` (the reconnect popup) | `account_links_test.py` |

Migration `0366_account_links` (postgres / sqlite / mysql) holds the tables. Secrets at rest use
`kn_seal` / `kn_unseal` (`services/common/include/knsvc/crypto.h`): AES-256-GCM with a key derived as
HMAC-SHA256(material, "kn-seal-v1:" + label), the label also bound as associated data, stored as
`v1.<base64url(nonce | ciphertext | tag)>`. The material is `accounts.secrets_key`, else `brain.join_secret`
(`account_links_seal_material`). Changing it makes the stored tokens unreadable: Discord's Join Server then asks
for a relink, and Twitch drops ask for a reconnect. Nothing else is lost.

## Login with Box Office

kn_brain is an OAuth 2.0 authorization server (RFC 6749 authorization code, RFC 7636 PKCE, RFC 7009 revocation)
for Box Office accounts. The player-facing name is "Login with Box Office" / "Box Office account".

### Clients

Apps are registered by staff, never self-service: `POST /mm/admin/oauth/clients` with the internal secret
(`mm_internal_authorized`) and `{name, redirect_uris[], scopes[], developer?, icon_url?, homepage_url?,
privacy_url?, tos_url?, min_level?, confidential?}`. The answer has the `client_id` and, for a confidential
client, its `client_secret`. The secret is shown once and only its SHA-256 is kept. Redirect URIs must be https,
or `http://127.0.0.1` / `http://localhost` for local tools, with no fragment, and they match exactly. Every link
URL must be https. `GET /mm/admin/oauth/clients` lists the clients (no secrets). `POST
/mm/admin/oauth/clients/:id/disable` turns one off, and its tokens stop working at once. `min_level` refuses
accounts below that level ("Requires level N", Krunker's `oauth.levelLocked`).

### Scopes

All of 10.0.7's scope names are catalogued (`oauth.scope.*`). An app can only be registered for, and authorized
with, the ones Box Office serves:

| Scope | Kind | Served by |
| --- | --- | --- |
| `profile` | read | `GET /oauth/userinfo`: id, username, level, clan, premium, badges |
| `stats:read` | read | `GET /oauth/api/stats`: kills, wins, games, deaths, time played, stats, score, level, ratings |
| `inventory:read` | read | `GET /oauth/api/inventory`: KR, Junk, owned skins |
| `friends:read` | read | `GET /oauth/api/friends`: following, followers |
| `inventory:write:currency` | write | the `gift.send` action (KR gift through the game's own `P_GIFT`, its fee and limits) |
| `friends:write` | write | the `follow.add` / `follow.remove` actions |

Not served (refused as an unknown scope): `notifications:read`, `inventory:write:skins`, `inventory:write`,
`trades:*`, `market:*`, `dms:*`, `clan:read`, `maps:read`, `events:subscribe`. Each needs its own route and review
before it is turned on (`served` in `k_scopes`, oauth.c).

### The flow

1. The app sends the player to `GET /oauth/authorize?response_type=code&client_id&redirect_uri&scope&state&
   code_challenge&code_challenge_method=S256`. PKCE S256 is mandatory for every client, public or confidential.
2. The consent page is server-rendered HTML with no script, `X-Frame-Options: DENY` and a strict CSP. A bad client,
   redirect or scope is shown on the page and never redirected to (RFC 6749 4.1.2.1). The player signs in on the
   page with their Box Office username, password and 2FA code. The page then keeps only a signed cookie
   (`kbo_oauth`, HttpOnly, SameSite=Lax, Path=/oauth, 15 minutes, Secure behind https), and the game session the
   sign-in created is revoked at once. Every form carries a CSRF token derived from the cookie.
3. **Authorize** issues a single-use code (10 minutes) and widens the grant to the new scopes. **Cancel** sends
   `error=access_denied`. Both carry `state`. A grant with a write scope needs 2FA turned on for the account, the
   "I understand and accept the risks" box and a fresh 2FA code (`oauth.disclaimer`, `oauth.twofa.desc`).
4. `POST /oauth/token` (form or JSON, client credentials by HTTP Basic or in the body):
   - `grant_type=authorization_code` + `code_verifier`: any attempt uses up the code, including a failed one.
     The answer is `{access_token (kbo_at_…, 1 h), refresh_token (kbo_rt_…, 30 d), token_type, expires_in, scope}`.
   - `grant_type=refresh_token`: the refresh token rotates (the old one stops working), and its scopes narrow to
     what the grant still holds.
5. `POST /oauth/revoke {token, client_id}` always answers 200. Revoking a refresh token also ends every token of
   that grant.

The client's own routes take the game session (`?token=` as in v10_account.c, or a Bearer header) and answer
v10's `{data, error}` envelope. They are `GET /oauth/authorize/info` and `POST /oauth/authorize` (authorizeInfo /
authorize, for an in-client consent), and `GET /account/apps`, `DELETE /account/apps/:id` and `GET
/account/apps/:id/activity` (Settings > Connected apps: the list with seconds timestamps, Disconnect, View
activity). Disconnect deletes the grant, its tokens and codes, and expires its pending actions.

### Actions: an app's write, approved by the player

A write scope never lets an app act alone. The app posts `POST /oauth/api/actions {kind, params}` and gets back
`{id, approve_url, expires_in (15 min), status: "pending"}`. The player opens `approve_url` (`/oauth/approve/:id`,
a no-script page), sees exactly what will run ("Send 50 KR to X", "Follow X") and approves it with a 2FA code, or
denies it. The client can do the same through `GET /oauth/actions/:id` and `POST /oauth/actions/:id/approve
{twofa_code}` / `.../deny`. An approved action runs once, through the same procedure the game uses. A gift is
idempotent on `oauth:<action id>` in `economy_requests`. A second approval finds the action already decided. The
app polls `GET /oauth/api/actions/:id`. Each step adds an `oauth_activity` row (the last 50 per app are kept),
which is what View activity lists.

### Limits

Token, revoke, authorize, sign-in, approve and action creation are rate limited per address
(`oauth.rate_per_min`, 30; the in-memory limiter plus Redis `hot_limit` across brains). 2FA codes go through
`P_TWOFA_LOGIN`, with its replay protection and its 10-failures-in-15-minutes lock. Tokens and codes are stored
as SHA-256 only. Config: `oauth.enabled` (true), `oauth.access_ttl_s` (3600), `oauth.refresh_ttl_s` (30 days),
`oauth.code_ttl_s` (600), `oauth.action_ttl_s` (900), `oauth.consent_ttl_s` (900).

### krunker.io accounts

For a krunker.io account the client shows krunker.io's own Connected apps (`GET /account/apps` on gapi with the
account's Bearer) read-only, with "Disconnect these apps on krunker.io.": Box Office makes no writes to krunker.io.

## Discord

`discord.c` replaces v10_auth.c's refusals. `GET /discord/status` answers `{configured, linked, id, username,
globalName, avatar, inGuild, guildUrl, canJoin}`. `POST /discord/link {returnTo?}` answers `{url}`, Discord's
authorize page (scopes `identify guilds.join`, single-use state, 10 minutes). `returnTo` is kept only when it
starts with the brain's own origin or one of `accounts.discord.return_origins`. `GET /discord/callback` exchanges
the code, reads `/users/@me`, stores the link with sealed tokens and sends the browser to
`returnTo?discord_link=<result>`, or shows a page with the result (the native client has no returnTo). The
results are `linked | cancelled | expired | in_use | unavailable | failed`. `POST /discord/join` adds the member
with the bot token (`PUT /guilds/:guild/members/:user`). It answers `{joined}` (false = already a member), and
refreshes an expired user token first. `DISCORD_NEEDS_RELINK` means Discord refused the user's token;
`DISCORD_JOIN_FAILED` means anything else failed. `POST /discord/unlink` answers `{unlinked}` and revokes the token
at Discord.

**Deletion pending** (10.0.7): while `accounts.delete_requested_at` is set, unlink and link answer 409
`DISCORD_DELETION_PENDING` and the row shows `discord.result.deletion_pending`. The link goes with the account
when the deletion sweep runs (ON DELETE CASCADE).

Discord items (10.0.7, :103481 and :103761): an item with `discord` is noSale, noSpin, limited, noQuickSell and
limT "Discord", tagged `#5865f2` (`tools/ui-import/v10_store.js`, `client/src/ui/ui_loadout.c`
store_tag_color). `assets/skins.json` has no such item yet, so the next store import carries them.

### The missing secrets

The repository has no Discord application, so linking is off until these are set (the row then shows v10's
unavailable state):

| Key | What |
| --- | --- |
| `accounts.discord.client_id` | the Discord application's OAuth2 client id |
| `accounts.discord.client_secret` | its client secret |
| `accounts.discord.redirect_uri` | optional, default `<brain.public_url>/discord/callback`; register it in the Discord app |
| `accounts.discord.bot_token` | a bot in the server with Create Instant Invite (Join Server) |
| `accounts.discord.guild_id` | the server's id |
| `accounts.discord.guild_url` | the invite shown as Open Server |
| `accounts.secrets_key` | recommended: the sealing key, separate from `brain.join_secret` |

`accounts.discord.api_url` / `authorize_url` exist for tests (the fake Discord in account_links_test.py).

## Twitch tokens and the reconnect flag

The Twitch link (account_life.c, 0359) now keeps its access and refresh tokens sealed in `twitch_links`
(`token_enc`, `refresh_enc`, `token_expires_at`, `token_checked_at`, `token_state`). `["getTwitchDrops"]` answers
`["twitchDropsResponse", [], reconnect]` (Box Office has no drop campaigns, so the drops are always empty):

- a link with no stored token (made before 0366) or `token_state = 'expired'` gives `reconnect = true`;
- otherwise, at most hourly or once the token is past its expiry, the token is validated with Twitch
  (`streams.twitch.validate_url`). If Twitch refuses it (401), it is refreshed and resealed. If the refresh is
  refused too, the state becomes `'expired'` and the answer says reconnect. If Twitch is unreachable, there is no
  verdict and no popup.

The client shows 10.0.7's "Your Twitch connection expired" popup and sends `["twitchAuth", true]`. The brain then
lets an already linked account start a new link (`force`), which stores fresh tokens and sets `token_state` back
to `'ok'`.
