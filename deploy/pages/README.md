# deploy/pages

The krunk.io front end and the game content's hosting. The page (dist/web without assets/) is
on **GitHub Pages**; the game content is on two interchangeable **asset origins**, the page picks
one at run time (kn-config.json, below); services stay on the central node:

| host | what | where |
| --- | --- | --- |
| `krunk.io`, `www.krunk.io` | dist/web without assets/ (index/social/editor/viewer, ui/, client.<hash>.*, kn-config.json) | GitHub Pages (`rebel10n/KrunkNative`, deploy-kru.yml job `pages`); before the cutover the Cloudflare Pages project `krunknative` |
| `krunknative-assets.pages.dev` | asset origin `cdn`: `/assets/` (the game content, 19.3k files, 1.9 GB) | Cloudflare Pages project `krunknative-assets`, its pages.dev URL only (assets-cdn.yml) |
| `assets.krunk.io` | asset origin `own`: `/assets/`, and `/updates/` for the launcher | kru OpenResty :8081 (tunnel; later abulxt's edges, deploy/ansible role kn_edge_origin) |
| `coordinator.krunk.io` | kn_brain auth + community + matchmaker | kru :8083 (tunnel) |
| `matchmaker.krunk.io` | kn_brain finder (`/servers`) | kru :8082 (tunnel) |
| `play.krunk.io` | net-bridge: `wss://play.krunk.io/native?target=<edge>-<n>.krunk.io:<port>` -> game server TCP | kru :8084 (tunnel, deploy/ansible `kn_bridge`) |
| `<edge>-<n>.krunk.io` | game servers, DNS only | edges (deploy/ansible cf_dns) |

Names this setup owns: the DNS records `krunk.io` and `www.krunk.io` (CNAME to
`rebel10n.github.io`, DNS only) and the Cloudflare Pages projects `krunknative` (old front end)
and `krunknative-assets`. It only *uses* `assets.krunk.io` (the own origin): its DNS and server
belong to the edge-origin work (deploy/ansible role kn_edge_origin), which must keep what the page
relies on there (CORS `*`, `/assets/version.txt` no-store, `?v=` caching, byte ranges).

```sh
make web && make pages-github  # stage dist/web for GitHub Pages into dist/gh-pages (CI publishes it)
make pages                     # make web + deploy/pages/deploy.sh: the old Cloudflare Pages front end
make assets-cdn                # mirror the own origin and deploy it to the asset CDN
deploy/pages/check.sh https://krunk.io/                       # a published site + its asset origins
deploy/pages/check.sh --assets https://krunknative-assets.pages.dev/
```

The site's settings are in `site.sh` (each one an environment variable; CI passes the repository
variables of the same name, empty = the default): `KN_ASSET_ORIGIN KN_ASSET_FALLBACK
KN_ASSET_OWN_URL KN_ASSET_CDN_URL KN_BRAIN_URL KN_BRIDGE_URL KN_RTC_URL KN_WT_URL KN_GA_ID`
(Analytics below), plus `KN_PAGES_CNAME` (github.sh) and `KN_PAGES_PROJECT KN_PAGES_BRANCH`
(deploy.sh). Cloudflare credentials: `CLOUDFLARE_API_TOKEN` / `CLOUDFLARE_ACCOUNT_ID`, else
`~/.config/kn-secrets/cloudflare.env`.

`stage.py` (run by `github.sh` and `deploy.sh`) copies dist/web without assets/, renames the client
to `client.<hash>.{js,wasm,data}`, writes `kn-config.json` and a top-level `404.html` (without one
Cloudflare Pages runs the site as an SPA and answers every unknown path with `index.html`, so a
missing image "loads" HTML; GitHub Pages serves it with status 404), and fills in the `<meta>`
tags that `ui/js/loader.js` reads (empty in a plain build, which keeps working as before from one
origin, e.g. `make web-serve`, net-bridge, kru's OpenResty): `kn-build` (hashed client file
names), `kn-asset-base`, `kn-config`, `kn-brain`, `kn-bridge`, `kn-rtc` and `kn-wt` (WebTransport
relay; `off` until one is deployed: `KN_WT_URL`).

## GitHub Pages

deploy-kru.yml builds dist/web, runs central.yml (services first, so a new page never talks to old
ones), stages the site with `github.sh` (actions/upload-pages-artifact) and its job `pages`
publishes it (actions/deploy-pages; `pages: write`, `id-token: write`, environment
`github-pages`). Both only run when the repository variable `GH_PAGES` is `1` and Pages is enabled
with `build_type=workflow` (the old Build job failed every push because it was not: "Get Pages site
failed ... Not Found"). `pages.yml` (dispatch only) republishes the last deployed build with the
current variables, without building: that is how a settings change (e.g. the asset origin) goes
out. Build's `web` job stages the site once as a check.

- The repository is public, so Pages is free. Limits: 1 GB site (ours: ~31 MB, 38 MB with the
  previous client kept), 100 MB a file (largest: client.wasm, 6.4 MB), 100 GB/month soft
  bandwidth. stage.py fails a stage over the limits.
- No custom headers. Every file is served with `Cache-Control: max-age=600`; nothing needs more:
  the web build has no threads (no SharedArrayBuffer), so no COOP/COEP; `.wasm` is served as
  `application/wasm` (streaming compile; check.sh checks it); there is no service worker, and the
  PWA manifest's `start_url`/`scope` are `./`, right at the custom domain's root. `_headers` /
  `_redirects` are Cloudflare-only and not written for GitHub (`krunk.io/assets/*` no longer
  redirects to the asset host: nothing current asks the page origin for /assets/).
- Because index.html may stay cached for those 10 minutes, `github.sh` also ships the
  `client.<hash>.*` files of the build the live site serves (`--keep-from`, `KN_PAGES_LIVE_URL`,
  default `https://krunk.io/`): a cached page still finds its client after a deploy.
- The custom domain is a repository setting (`gh api -X PUT repos/rebel10n/KrunkNative/pages -f
  cname=krunk.io`): a workflow deployment ignores the `CNAME` file stage.py writes (kept as the
  record of the intended name, `KN_PAGES_CNAME`). GitHub's certificate needs the names DNS only
  (not proxied); HTTPS can be enforced once it is issued.
- Absolute links of the page (`/social.html?...`) only work at the domain root, so the
  `https://rebel10n.github.io/KrunkNative/` address is good for checking the game (it loads with
  relative URLs) but not for every link; it redirects to krunk.io once the custom domain is set.

## Asset origins

The page loads the game content from one of two origins, both serving `/assets/<path>` with
`assets/version.txt` (the `?v=` of every asset URL) and CORS for any page origin:

- `own`: `KN_ASSET_OWN_URL`, default `https://assets.krunk.io/`: kru's OpenResty today, abulxt's
  edges after the edge-origin move (a DNS change of `assets.krunk.io`, nothing here). Also the
  native launcher's updates host.
- `cdn`: `KN_ASSET_CDN_URL`, the Cloudflare Pages asset project's `https://<sub>.pages.dev/`
  (Asset CDN below); empty = not offered.

`stage.py` writes them to `kn-config.json` next to index.html, with the one in use
(`KN_ASSET_ORIGIN`, default `own`) and the one a failed download is retried on
(`KN_ASSET_FALLBACK`, default none; shared/src/utils.c retries `ensure_asset_file` and the
prefetches there, so a file missing from one origin, or an origin that is down, still loads):

```json
{"asset_origin": "cdn", "asset_fallback": "own",
 "asset_origins": {"cdn": "https://krunknative-assets.pages.dev/", "own": "https://assets.krunk.io/"}}
```

loader.js reads it before boot (no-cache; it wins over the `kn-asset-base` meta, which names the
same origin for older loaders), so **switching is a repository variable plus a page republish, no
client build** (GitHub serves the new file within its 10-minute cache):

```sh
# to the Cloudflare Pages CDN (own stays the fallback)
gh variable set KN_ASSET_ORIGIN --body cdn && gh variable set KN_ASSET_FALLBACK --body own && gh workflow run pages.yml
# back to our own origin (assets.krunk.io), the CDN as the fallback
gh variable set KN_ASSET_ORIGIN --body own && gh variable set KN_ASSET_FALLBACK --body cdn && gh workflow run pages.yml
# own origin on another name (e.g. a CNAME of our own to abulxt's edges): add the DNS record first
gh variable set KN_ASSET_OWN_URL --body https://<name>.krunk.io/ && gh workflow run pages.yml
```

`deploy/pages/check.sh <site>` afterwards checks every origin kn-config.json names.

Per tab, `?assets=<name>` picks one of the configured origins (`?assets=cdn`, `?assets=own`; kept
in sessionStorage for the tab's later page loads, `?assets=default` forgets it), e.g. to try the
CDN on the live site before switching everyone. A full URL (`?assets=http://127.0.0.1:9000/`) is
only accepted on a localhost page, so a link cannot point players at someone else's content.

## Asset CDN

The Cloudflare Pages project `krunknative-assets`, on its pages.dev URL only (no custom domain):
`assets-cdn.sh` / assets-cdn.yml mirror the own origin and deploy it.

- What: exactly what the native client gets, the launcher's content index (updates manifest
  `assets.manifest`: every file of the web tree but the ones the packages ship, as
  sha256-addressed objects, verified on download) plus those packaged files, fetched by path, plus
  the origin's `assets/version.txt`. `assets_cdn.py` hard-links the objects into the tree
  (wrangler skips symlinks); assets-cdn.yml caches them, so only new content is downloaded.
- When: after every Deploy kru, every 6 hours and by hand (`gh workflow run assets-cdn.yml [-f
  force=true]`); a run stops after one request when the CDN already has the origin's
  `version.txt`. Gated by the repository variable `KN_ASSET_CDN=1` (the project must exist).
- Upload: `wrangler pages deploy` hashes every file, asks Cloudflare which hashes it lacks
  (`/pages/assets/check-missing`) and uploads only those (identical files once), so a content
  change uploads just the changed files; the full tree is only hashed, not resent.
- Headers (`_headers`): `Access-Control-Allow-Origin: *` (public files, no credentials), exposed
  `Content-Length, Content-Range, ETag`, `Cache-Control: public, max-age=86400` for `/assets/*`
  (a day, not a year: `_headers` cannot tell `?v=` URLs from bare ones), `no-store` for
  `assets/version.txt`. Pages serves byte ranges (206) and ETags itself.
- Limits (Free plan: 20,000 files a deployment, 25 MiB a file, 100 `_headers` rules). Our tree
  (2026-10-13, asset list e9ce9c01455eec39): 19,192 indexed files + 113 packaged + version.txt =
  **19,306 files** (694 to spare), **1.92 GB**, largest file 7.4 MB (`user-assets/md23677/mod.zip`),
  none over 5 MiB but that one: everything fits. The biggest groups are textures/weapons (6,376) and
  textures/previews (5,977). `assets_cdn.py` fails the run instead of deploying a partial tree
  when the content outgrows that. Then: `KN_ASSET_CDN_EXCLUDE="textures/previews/"` (space-
  separated prefixes) leaves directories out, which the page's `asset_fallback` (own) serves (the
  client retries every failed download there; `<img>` previews in the UI are not retried); or the
  Workers Paid plan (100,000 files, project setting `PAGES_WRANGLER_MAJOR_VERSION=4`); or move
  the page to `own`.
- The launcher's `/updates/` (manifests, 18,356 content objects, client packages of 27-33 MB) is
  not on the CDN: it would not fit next to the tree (37k files, packages over 25 MiB).

## Native client and launcher

They stay on `assets.krunk.io`: the launcher's default updates URL
(`KN_UPDATE_DEFAULT_URL`, launcher/include/kn_update.h) is compiled into released builds and the
client's update links are checked against that host (client/src/ui/update_ui.c), and the
updates store does not fit on Pages (above). Their switch is the same name's DNS: when
`assets.krunk.io` moves to abulxt's edges (or any origin of ours), the launcher follows with no
release, and `KRUNKNATIVE_UPDATES_URL` / `--updates-url` points a single install elsewhere.

## Cutover

From Cloudflare Pages to GitHub Pages, once the change with this README is on main. Each step is
verified before the next; the old front end keeps deploying (`KN_CF_FRONTEND` unset) until the
last step, so the DNS can go back at any time.

1. Enable Pages and the deploy: `gh api -X POST repos/rebel10n/KrunkNative/pages -f build_type=workflow`,
   `gh variable set GH_PAGES --body 1`, rerun Deploy kru (`gh workflow run deploy-kru.yml`).
   Verify: `deploy/pages/check.sh https://rebel10n.github.io/KrunkNative/` and the game at
   `https://rebel10n.github.io/KrunkNative/?offline=1&map=kanji`.
   Back: `gh variable delete GH_PAGES`.
2. Create the asset project: `npx wrangler@4.149.0 pages project create krunknative-assets
   --production-branch main` (note the pages.dev subdomain it prints), then
   `gh variable set KN_ASSET_CDN_URL --body https://<sub>.pages.dev/`, `gh variable set KN_ASSET_CDN --body 1`,
   `gh workflow run assets-cdn.yml -f force=true` (first upload: 1.9 GB).
   Verify: `deploy/pages/check.sh --assets https://<sub>.pages.dev/` (version, CORS, 206, headers).
3. Offer it, then switch to it: `gh workflow run pages.yml`, try
   `https://rebel10n.github.io/KrunkNative/?offline=1&map=kanji&assets=cdn`, then the switch of
   "Asset origins". Back: the switch back.
4. DNS: with `rebel10n.github.io` serving the site, set the custom domain
   (`gh api -X PUT repos/rebel10n/KrunkNative/pages -f cname=krunk.io`), check GitHub answers for
   it (`deploy/pages/check.sh https://krunk.io/ --resolve krunk.io:443:185.199.108.153 -k`), then
   point `krunk.io` and `www.krunk.io` at `rebel10n.github.io`, DNS only. The apex: on Cloudflare
   a CNAME (flattened to GitHub's addresses, one record to change and to change back); on a DNS
   host without CNAME flattening / ALIAS (FreeDNS) GitHub's A records `185.199.108.153`,
   `185.199.109.153`, `185.199.110.153`, `185.199.111.153` and AAAA `2606:50c0:8000::153`,
   `2606:50c0:8001::153`, `2606:50c0:8002::153`, `2606:50c0:8003::153`; `www` is a CNAME to
   `rebel10n.github.io` on both. Back: the saved records (proxied CNAMEs to `krunknative.pages.dev`;
   the Pages project still has both domains).
5. HTTPS: wait for GitHub's certificate (`gh api repos/rebel10n/KrunkNative/pages --jq
   .https_certificate.state` = `approved`; minutes, up to an hour; https://krunk.io shows a
   certificate error until then), then `gh api -X PUT repos/rebel10n/KrunkNative/pages -F
   https_enforced=true`. Verify: `deploy/pages/check.sh https://krunk.io/`.
6. Retire the old front end after a quiet period: `gh variable set KN_CF_FRONTEND --body 0`, remove
   `krunk.io` / `www.krunk.io` from the project `krunknative` (check the DNS records are still the
   GitHub ones afterwards), later delete the project and drop `https://krunknative.pages.dev` from
   `kn_web_origins` (deploy/ansible/group_vars/all.yml).

`kn-bridge` (`KN_BRIDGE_URL`, default `wss://play.krunk.io/native`) is the net-bridge endpoint
the game connects through: Pages cannot carry WebSockets to a TCP game server, so the default
same-origin `wss://krunk.io/native` would only get HTML back. The client appends
`?target=<host>:<port>` of the server the matchmaker picked (client/src/net/net_web.c
`net_web_target_url`). `?ws=<url>` on the page still overrides it: a URL without `target=` is
used as the bridge (e.g. `?ws=ws://localhost:8080/native` for a local `make bridge`), one with
`target=` pins the session to that server.

## Analytics

Google Analytics (gtag.js) is configured by one setting, `KN_GA_ID`: a GA4 measurement id
(`^G-[A-Z0-9]+$`, anything else fails the build), or empty / `off` for none. `site.sh`
defaults to `G-6EGYKRF020` for both front ends (the krunk.io property; `KN_GA_ID=off
deploy/pages/github.sh` stages without it); `make web` defaults to off and opts in the same way
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

- GitHub Pages: every file `max-age=600` (not configurable). index/*.html, js/, css/ and
  kn-config.json can therefore be up to 10 minutes old in a browser (loader.js fetches
  kn-config.json with `no-cache`); the hashed client files never change, and the previous
  build's stay for one deploy (GitHub Pages above).
- The old Cloudflare Pages front end: Pages' default `public, max-age=0, must-revalidate` + ETag
  for the pages, `immutable` for a year for `client.<hash>.*` (`_headers`), `krunk.io/assets/*`
  302s to the asset origin (`_redirects`); zone setting Browser Cache TTL = "Respect existing
  headers" (krunk.io and krunker.party).
- `client.<hash>.{js,wasm,data}`: the hash covers all three, so a page always loads a matching
  client.js + wasm + data.
- `<origin>/assets/version.txt`: `no-store`; a content hash of the asset tree, rewritten by the
  ansible role kn_web on every run (after the rsync, now `--checksum`) and copied by the CDN
  mirror. loader.js reads it (from the chosen origin) before boot, the client appends
  `?v=<version>` to every asset URL (shared/src/utils.c `web_asset_url_at`).
- own origin, `/assets/...?v=...`: `public, max-age=31536000, immutable`, cached at Cloudflare's
  edge while assets.krunk.io is proxied (cache rule `kn_assets_cache`); without `?v`: `no-cache`,
  revalidated against the ETag. A content change changes the version, hence every URL.
- CDN origin, `/assets/*`: `public, max-age=86400` (Asset CDN above), Pages' own edge cache.

## krunker.party

The old domain: zone rule `kn_krunker_party_to_krunk_io` (dynamic redirect) sends
`krunker.party` and `www.krunker.party` to `https://krunk.io<path>` with 301, query kept.
`coordinator.krunker.party`, `matchmaker.krunker.party` and the `<edge>-<n>.krunker.party`
A records are deliberately not redirected: released native clients have them compiled in,
and neither 301 nor 308 helps there (CORS preflights and WebSocket upgrades do not follow
redirects, game servers are not HTTP). They keep pointing at the same tunnel / IPs until
the old clients are gone.

## Cache purge

`deploy.sh` (the old Cloudflare front end) ends by purging Cloudflare's edge cache for the page
hosts (`KN_PURGE_HOSTS`, default `krunk.io www.krunk.io`, zone `KN_PURGE_ZONE`) so every client
loads the new build on its next page load. CI (deploy-kru.yml) runs the same script while
`KN_CF_FRONTEND` is not 0; once the hosts are DNS only (GitHub Pages) there is nothing of
theirs in Cloudflare's cache and the purge is a no-op. The
hashed `client.<hash>.*` files and `?v=` assets change URL with each build and are not purged.
The token needs Zone Read + Cache Purge on the zone; `KN_PURGE_HOSTS=` skips the purge.
