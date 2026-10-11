# deploy/pages

The krunk.io front end on Cloudflare Pages (project `krunknative`, production branch
`slop`, custom domains `krunk.io` + `www.krunk.io`). Only the page is on Pages; game
content and services stay on the central node behind the Cloudflare tunnel:

| host | what | where |
| --- | --- | --- |
| `krunk.io`, `www.krunk.io` | dist/web without assets/ (index/social/editor/viewer, ui/, client.<hash>.*) | Pages |
| `assets.krunk.io` | `/assets/` (game content, ~1 GB) | kru OpenResty :8081 (tunnel) |
| `coordinator.krunk.io` | kn_brain auth + community + matchmaker | kru :8083 (tunnel) |
| `matchmaker.krunk.io` | kn_brain finder (`/servers`) | kru :8082 (tunnel) |
| `play.krunk.io` | net-bridge: `wss://play.krunk.io/native?target=<edge>-<n>.krunk.io:<port>` -> game server TCP | kru :8084 (tunnel, deploy/ansible `kn_bridge`) |
| `<edge>-<n>.krunk.io` | game servers, DNS only | edges (deploy/ansible cf_dns) |

```sh
make pages                     # make web + deploy/pages/deploy.sh
deploy/pages/deploy.sh         # an existing dist/web
```

`stage.py` also writes a top-level `404.html`: without one Pages runs the site as an SPA and
answers every unknown path with `index.html` (200, text/html), so a missing image "loads" HTML
instead of failing. Pages links are real files (`/`, `/social`, `*.html`), nothing needs the fallback.

`deploy.sh` runs `stage.py` (dist/web -> dist/pages) and `wrangler pages deploy` (pinned
version). Credentials: `CLOUDFLARE_API_TOKEN` / `CLOUDFLARE_ACCOUNT_ID`, else
`~/.config/kn-secrets/cloudflare.env`. CI does not deploy Pages (deploy-kru.yml only runs
the ansible play): run `make pages` / `deploy.sh` by hand after a client change. Knobs: `KN_PAGES_PROJECT KN_PAGES_BRANCH KN_ASSET_BASE KN_BRAIN_URL KN_BRIDGE_URL KN_WT_URL KN_GA_ID` (Analytics below).

`stage.py` fills in the `<meta>` tags that `ui/js/loader.js` reads (empty in a plain
build, which keeps working as before from one origin, e.g. `make web-serve`, net-bridge,
kru's OpenResty): `kn-build` (hashed client file names), `kn-asset-base`, `kn-brain`,
`kn-bridge` and `kn-wt` (WebTransport relay; `off` until one is deployed: `KN_WT_URL`).

`kn-bridge` (`KN_BRIDGE_URL`, default `wss://play.krunk.io/native`) is the net-bridge endpoint
the game connects through: Pages cannot carry WebSockets to a TCP game server, so the default
same-origin `wss://krunk.io/native` would only get HTML back. The client appends
`?target=<host>:<port>` of the server the matchmaker picked (client/src/net/net_web.c
`net_web_target_url`). `?ws=<url>` on the page still overrides it: a URL without `target=` is
used as the bridge (e.g. `?ws=ws://localhost:8080/native` for a local `make bridge`), one with
`target=` pins the session to that server.

## Analytics

Google Analytics (gtag.js) is configured by one setting, `KN_GA_ID`: a GA4 measurement id
(`^G-[A-Z0-9]+$`, anything else fails the build), or empty / `off` for none. `deploy.sh`
defaults to `G-6EGYKRF020` (the krunk.io property; `KN_GA_ID=off deploy/pages/deploy.sh`
deploys without it); `make web` defaults to off and opts in the same way
(`make web KN_GA_ID=G-...`, for `make web-serve` / `make bridge`).

The `ui/` sources stay free of analytics: the native HTML engine renders them (and
ignores `<script>`). `ga.py` (run by `stage.py --ga-id`, and by `make web` on dist/web)
puts Google's snippet, with the id filled in, as the first child of `<head>` in every
HTML document of the output (index, social, editor, viewer, any other page; the
`windows/` templates are fragments without `<head>` and are skipped). It first removes a
snippet that is already there, so restaging never doubles it and `off` strips it.

Every page switch of the site is a real navigation (`location.href`, `client/src/app_mode.c`
`web_navigate`; `?offline`, `?sandbox`, `/social`, ...), so each one is its own page load
and `gtag('config', ...)` sends its `page_view`; nothing in the client calls gtag.

`_headers` sets no Content-Security-Policy (nor does the zone), so gtag.js and the collect
requests to `*.google-analytics.com` / `*.analytics.google.com` need no allowance. Adding
a CSP later means allowing `https://www.googletagmanager.com` in `script-src` and those
hosts in `connect-src`.

## Caching

Zone setting Browser Cache TTL = "Respect existing headers" (krunk.io and krunker.party);
it used to force `max-age=14400` onto anything without one (client.js).

- Pages, index/*.html, js/, css/, ui images, windows/: Pages' default
  `public, max-age=0, must-revalidate` + ETag: revalidated on every load, never stale.
- `client.<hash>.{js,wasm,data}`: `immutable` for a year (`_headers`). The hash covers
  all three, so a page always loads a matching client.js + wasm + data; the page that
  names them is always revalidated.
- `assets.krunk.io/assets/version.txt`: `no-store`; a content hash of the asset tree,
  rewritten by the ansible role kn_web on every run (after the rsync, now `--checksum`).
  loader.js reads it before boot, the client appends `?v=<version>` to every asset URL
  (shared/src/utils.c `web_asset_url`).
- `/assets/...?v=...`: `public, max-age=31536000, immutable`, cached at the edge (cache
  rule `kn_assets_cache` on krunk.io: cache eligible, TTLs from the origin). A content
  change changes the version, hence every URL: nothing to purge.
- `/assets/...` without `?v` (no version.txt yet, old pages): `no-cache`, revalidated
  against the ETag (`cf-cache-status: REVALIDATED`, 304 to the browser).
- `krunk.io/assets/*` 302s to the asset host (`_redirects`).

## krunker.party

The old domain: zone rule `kn_krunker_party_to_krunk_io` (dynamic redirect) sends
`krunker.party` and `www.krunker.party` to `https://krunk.io<path>` with 301, query kept.
`coordinator.krunker.party`, `matchmaker.krunker.party` and the `<edge>-<n>.krunker.party`
A records are deliberately not redirected: released native clients have them compiled in,
and neither 301 nor 308 helps there (CORS preflights and WebSocket upgrades do not follow
redirects, game servers are not HTTP). They keep pointing at the same tunnel / IPs until
the old clients are gone.
