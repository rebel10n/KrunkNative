# deploy/pages

The krunk.io front end and the game content's hosting. The page (dist/web without assets/) is
on **GitHub Pages**; the game content is on the **asset CDN** (two Cloudflare Pages projects, split by
directory, `deploy/asset-shards.json`) with kru's **download host** as the source and the fallback;
the page picks its origin at run time (kn-config.json, below); services stay on the central node:

| host | what | where |
| --- | --- | --- |
| `krunk.io`, `www.krunk.io` | dist/web without assets/ (index/social/editor/viewer, ui/, client.<hash>.*, kn-config.json) | GitHub Pages (`rebel10n/KrunkNative`, deploy-kru.yml job `pages`, custom domain `krunk.io`, HTTPS enforced) |
| `assets.krunk.io` | asset origin `own`: `/assets/` but the directories of project b (10.9k files, 2.2 GB); `_redirects`: those 301 to `assets1.`, `/updates/*` 302 to `dl.` | Cloudflare Pages project `krunknative-assets-a` (custom domain; pages.dev: `krunknative-assets-a.pages.dev`, origin `a-dev`). Before the cutover (runbook): kru through the abulxt edges |
| `assets1.krunk.io` | `/assets/` of project b's directories (`textures/previews/`, `hats/`, `melee/`, `faces/`, `body/`, `sprays/`, `back/`: 11.2k files, 0.5 GB) | Cloudflare Pages project `krunknative-assets-b` (origin `b-dev` on pages.dev) |
| `dl.krunk.io` | asset origin `dl`: the whole `/assets/` tree and `/updates/` (the launcher's store); the CDN's source, the clients' fallback | kru: host nginx :443 (Let's Encrypt) -> OpenResty :8087 (deploy/ansible role kn_dl_origin) |
| `krunknative-assets.pages.dev` | the old single project (origin `cdn`), frozen at the last tree that fit: not deployed any more, not used | Cloudflare Pages project `krunknative-assets` (left alone) |
| `coordinator.krunk.io` | kn_brain auth + community + matchmaker | kru :8083 (abulxt edges) |
| `matchmaker.krunk.io` | kn_brain finder (`/servers`) | kru :8082 (abulxt edges) |
| `play.krunk.io` | net-bridge: `wss://play.krunk.io/native?target=<edge>-<n>.krunk.io:<port>` -> game server TCP | kru :8084 (abulxt edges, deploy/ansible `kn_bridge`) |
| `<edge>-<n>.krunk.io` | game servers, DNS only | edges (A records by hand at Spaceship; deploy/ansible `kn_dns_check` checks them) |

DNS: the krunk.io zone is on Spaceship (since 2026-10-13); every record is set by hand (or
through Spaceship's API by the runbook), nothing in CI writes DNS. Names this setup owns: the DNS
records `krunk.io` (GitHub's A records), `www.krunk.io` (CNAME to `rebel10n.github.io`),
`assets.krunk.io` / `assets1.krunk.io` (CNAMEs to `krunknative-assets-a` / `-b.pages.dev`, the
projects' custom domains) and `dl.krunk.io` (A record, kru's public IP), and the Pages projects
`krunknative-assets-a` / `-b`. What the page relies on from every asset origin: CORS `*`,
`/assets/version.txt` no-store, `?v=` caching, byte ranges.

```sh
make web && make pages-github  # stage dist/web for GitHub Pages into dist/gh-pages (CI publishes it)
make assets-cdn                # mirror dl.krunk.io and deploy it to both Pages projects of the asset CDN
deploy/pages/assets-cdn.sh --dry-run                          # the files per project, no upload
deploy/pages/check.sh https://krunk.io/                       # a published site + its asset origins
deploy/pages/check.sh --assets https://krunknative-assets-a.pages.dev/
```

The site's settings are in `site.sh` (each one an environment variable; CI passes the repository
variables of the same name, empty = the default): `KN_ASSET_ORIGIN KN_ASSET_FALLBACK
KN_ASSET_OWN_URL KN_ASSET_CDN_URL KN_ASSET_DL_URL KN_ASSET_SHARDS_FILE KN_BRAIN_URL KN_BRIDGE_URL KN_RTC_URL KN_WT_URL KN_GA_ID`
(Analytics below), plus `KN_PAGES_CNAME` (github.sh). Cloudflare credentials (the asset CDN): `CLOUDFLARE_API_TOKEN` / `CLOUDFLARE_ACCOUNT_ID`, else
`~/.config/kn-secrets/cloudflare.env`.

`stage.py` (run by `github.sh`) copies dist/web without assets/, renames the client
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

The page loads the game content from one origin and retries a failed download on a second one;
every origin serves `/assets/<path>` with `assets/version.txt` (the `?v=` of every asset URL) and
CORS for any page origin:

- `own`: `KN_ASSET_OWN_URL`, default `https://assets.krunk.io/`: the asset CDN's default Pages
  project (krunknative-assets-a) once the split is live; before the cutover, kru through the
  abulxt edges (the whole tree).
- `dl`: `KN_ASSET_DL_URL`, default deploy/asset-shards.json's `download`, `https://dl.krunk.io/`:
  kru's download host, the whole tree (the fallback).
- `a-dev`, `b-dev`: the Pages projects on their pages.dev names (deploy/asset-shards.json), for
  trying the CDN before its domains carry traffic; `a`, `b`: their custom domains, once the file
  says `"live": true`.
- `cdn`: `KN_ASSET_CDN_URL`, the old single Pages project (`krunknative-assets.pages.dev`, frozen);
  empty = not offered. Unset it with the cutover.

`stage.py` writes them to `kn-config.json` next to index.html, with the one in use
(`KN_ASSET_ORIGIN`, default `own`), the one a failed download is retried on
(`KN_ASSET_FALLBACK`, default none; shared/src/utils.c retries `ensure_asset_file` and the
prefetches there, so a file missing from one origin, or an origin that is down, still loads) and
the **split** (`asset_split`, from deploy/asset-shards.json): on the default project's URL, the
other project's directories load straight from its host (`Module.krunkNativeAssetBaseFor` in
loader.js; also the URLs the C UI writes into `<img>` / `<video>`, which are never retried):

```json
{"asset_origin": "own", "asset_fallback": "dl",
 "asset_origins": {"own": "https://assets.krunk.io/", "dl": "https://dl.krunk.io/",
                   "a": "https://assets.krunk.io/", "b": "https://assets1.krunk.io/",
                   "a-dev": "https://krunknative-assets-a.pages.dev/", "b-dev": "https://krunknative-assets-b.pages.dev/"},
 "asset_split": [{"base": "https://assets.krunk.io/", "routes": [["textures/previews/", "https://assets1.krunk.io/"], ...]},
                 {"base": "https://krunknative-assets-a.pages.dev/", "routes": [["textures/previews/", "https://krunknative-assets-b.pages.dev/"], ...]}]}
```

(the first split only once the file is live). loader.js reads it before boot (no-cache; it wins
over the `kn-asset-base` meta, which names the same origin for older loaders), so **switching is a
repository variable plus a page republish, no client build** (GitHub serves the new file within
its 10-minute cache):

```sh
# the asset CDN with kru as the fallback (the setting after the cutover)
gh variable set KN_ASSET_ORIGIN --body own && gh variable set KN_ASSET_FALLBACK --body dl && gh workflow run pages.yml
# everything from kru (the CDN in trouble)
gh variable set KN_ASSET_ORIGIN --body dl && gh variable set KN_ASSET_FALLBACK --body own && gh workflow run pages.yml
```

`deploy/pages/check.sh <site>` afterwards checks every origin kn-config.json names.

Per tab, `?assets=<name>` picks one of the configured origins (`?assets=a-dev` tries the split
CDN on its pages.dev names, `?assets=dl`; kept in sessionStorage for the tab's later page loads,
`?assets=default` forgets it), e.g. to try the CDN on the live site before switching everyone. A
full URL (`?assets=http://127.0.0.1:9000/`) is only accepted on a localhost page, so a link cannot
point players at someone else's content.

## Asset CDN

Two Cloudflare Pages projects, the tree split between them by directory (**`deploy/asset-shards.json`**,
one file for the deploy, the page and the launcher manifest; `deploy/pages/asset_shards.py` reads
it): `krunknative-assets-a` as `assets.krunk.io` (everything the other project does not take) and
`krunknative-assets-b` as `assets1.krunk.io` (`textures/previews/`, `hats/`, `melee/`, `faces/`,
`body/`, `sprays/`, `back/`). `assets-cdn.sh` / assets-cdn.yml mirror kru's download host
(`dl.krunk.io`) and deploy each part to its project.

- Why two: Pages takes at most 20,000 files a deployment (Free plan; 25 MiB a file, 100
  `_headers` rules, 100 dynamic `_redirects`). The tree outgrew one project with the v10 content
  (2026-10-14, asset list 442d6fc77b68be68: 22,039 indexed files + 115 packaged + version.txt =
  **22,155 files**, 2.7 GB; largest file 22.8 MB, `videos/tutorial/basics.mp4`). By directory:
  textures/previews 7,422, textures/weapons 6,802 (5,015 of them skins), textures/hats 883,
  textures/melee 753, textures/faces 689, textures/body 595, textures/sprays 563, models/hats 536,
  sound 513, user-assets 397, models/weapons 371, models/body 332, textures/back 330, textures/
  (files) 276, models/melee 207, the rest under 200 each. `textures/` alone (19,059) is over the
  limit, so its subdirectories are split: **a 10,920 files (2.2 GB), b 11,236 (0.5 GB)**, both
  under the file's `max_files` 16,000 with ~5k to grow. A path goes to the project with the
  longest matching prefix; `exclude` prefixes go to neither (kru only).
- Old clients (cached pages, launchers and clients without the split) ask `assets.krunk.io` for
  everything: project a's `_redirects` (generated) send b's directories there with a 301
  (`/assets/textures/previews/* https://assets1.krunk.io/assets/textures/previews/:splat 301`),
  `/updates/*` (the launcher's store: manifests, objects, packages over 25 MiB) and the excluded
  prefixes to `https://dl.krunk.io/` with a 302. New clients go to the right project directly:
  the page by kn-config.json's split, the launcher and the native on-demand fetch by the
  manifest's `assets.mirrors` (`skip` / `only` prefixes, written by kn-updates.py from the same
  file), with kru (`dl`, the objects) as the fallback everywhere.
- What: exactly what the native client gets, the launcher's content index (updates manifest
  `assets.manifest`: every file of the web tree but the ones the packages ship, as
  sha256-addressed objects, verified on download) plus those packaged files, fetched by path, plus
  the origin's `assets/version.txt` in both projects. `assets_cdn.py` hard-links the objects into
  the trees (wrangler skips symlinks); assets-cdn.yml caches them, so only new content is downloaded.
- When: after every Deploy kru, every 6 hours and by hand (`gh workflow run assets-cdn.yml [-f
  force=true]`); a run stops after one request per project when both already have the source's
  `version.txt`. Project b is deployed first, a last (its version.txt is what loader.js and the
  next run read). Gated by the repository variable `KN_ASSET_CDN=1` (the projects must exist).
  After a deploy the workflow checks both pages.dev names (version, CORS, a byte range) and a's
  redirects (a 301 for a b path, a 302 for `/updates/`).
- Limits: `assets_cdn.py` prints the files per project (and per directory with `--dry-run`); over
  the file's `max_files` it warns, over 20,000 it fails the run (naming the biggest directories)
  instead of deploying a partial tree. Then move a directory to the other project (or add a
  third) in deploy/asset-shards.json. Check without downloading or uploading anything:
  `deploy/pages/assets-cdn.sh --dry-run`; `make deploy-test` runs the split's tests.
- Upload: `wrangler pages deploy` hashes every file, asks Cloudflare which hashes it lacks
  (`/pages/assets/check-missing`) and uploads only those (identical files once), so a content
  change uploads just the changed files; the full tree is only hashed, not resent.
- Headers (`_headers`): `Access-Control-Allow-Origin: *` (public files, no credentials), exposed
  `Content-Length, Content-Range, ETag`, `Cache-Control: public, max-age=86400` for `/assets/*`
  (a day, not a year: `_headers` cannot tell `?v=` URLs from bare ones), `no-store` for
  `assets/version.txt`. Pages serves byte ranges (206) and ETags itself.
- The launcher's `/updates/` (manifests, 18k content objects, client packages of 27-33 MB) is not
  on the CDN: kru serves it as `https://dl.krunk.io/updates/` (deploy/ansible/README.md "Download
  host"), the launcher's default updates URL.
- `"live"` in the file: false until both custom domains serve the projects (the cutover runbook).
  Until then the page splits only on the pages.dev names (`?assets=a-dev`) and the manifest names
  the pages.dev mirrors; `true` (a commit; Deploy kru copies the file to kru, the next index
  rewrites the manifest) switches both to `assets.krunk.io` / `assets1.krunk.io`.

## Native client and launcher

The launcher's default updates URL (`KN_UPDATE_DEFAULT_URL`, launcher/include/kn_update.h) is
`https://dl.krunk.io/updates/client/latest.json` (kru; the store does not fit on Pages) and the
client's update links are checked against `dl.krunk.io` and `assets.krunk.io` (client/src/ui/
update_ui.c). Launchers built before that ask `https://assets.krunk.io/updates/...`: project a
302s `/updates/*` to `dl.krunk.io`, which kn_http follows (relative URLs in the manifest still
resolve against assets.krunk.io, so their objects take the same 302; a pipelined batch does not
follow it and the object is retried as a plain GET, which does: slower, not broken). The content
comes from the manifest's mirrors (the two projects, each with its path filter) next to kru's
objects; `KRUNKNATIVE_UPDATES_URL` / `--updates-url` points a single install elsewhere.

## Asset CDN split cutover

From one Pages project (`krunknative-assets`, over the file limit) to two plus kru's download host.
Order (prod: each step verified before the next; the DNS of `assets.krunk.io` last):

1. `dl.krunk.io`: its A record (kru's public IP), then `kn_dl_origin_enabled: true`
   (group_vars/central/main.yml; the first Deploy kru gets the certificate). Verify
   `curl -fsSI https://dl.krunk.io/assets/version.txt https://dl.krunk.io/updates/client/latest.json`.
2. The projects `krunknative-assets-a` / `-b` (`wrangler pages project create <name>
   --production-branch main`; fix `pages_dev` in deploy/asset-shards.json if Pages gave another
   subdomain), `KN_ASSET_CDN=1`, `gh workflow run assets-cdn.yml -f force=true`; its Verify step
   checks both pages.dev names and a's redirects. Try the page on them: `https://krunk.io/?assets=a-dev`.
3. `assets1.krunk.io`: CNAME to `krunknative-assets-b.pages.dev`, custom domain on project b
   (new name, no traffic yet); wait for it to be active.
4. Custom domain `assets.krunk.io` on project a (pending until the DNS), its CNAME to
   `krunknative-assets-a.pages.dev` instead of the edges; wait for the certificate.
5. `"live": true` in deploy/asset-shards.json (page split + manifest mirrors on the custom
   domains), `KN_ASSET_ORIGIN=own`, `KN_ASSET_FALLBACK=dl`, delete `KN_ASSET_CDN_URL`; rerun pages.yml.
   Back: the old CNAME of `assets.krunk.io` (`byod.abstracteducation.loan`), `"live": false`.

## Cutover

From Cloudflare Pages to GitHub Pages. **Done on 2026-10-13**: steps 1-6 ran in order, and the
old Cloudflare Pages project `krunknative`, its deploy step and deploy.sh are gone. Kept as the
record of how it was done, and for moving the apex to another DNS host (step 4).

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
   `rebel10n.github.io` on both. (If GitHub shows no certificate state for a long time, remove the
   custom domain and add it again: `-F cname=null`, then `-f cname=krunk.io`.)
5. HTTPS: wait for GitHub's certificate (`gh api repos/rebel10n/KrunkNative/pages --jq
   .https_certificate.state` = `approved`; minutes, up to an hour; https://krunk.io shows a
   certificate error until then), then `gh api -X PUT repos/rebel10n/KrunkNative/pages -F
   https_enforced=true`. Verify: `deploy/pages/check.sh https://krunk.io/`.
6. Retire the old front end: remove `krunk.io` / `www.krunk.io` from the Cloudflare Pages project
   `krunknative` (the DNS records stay the GitHub ones), delete the project, drop its deploy step,
   deploy.sh and `https://krunknative.pages.dev` from `kn_web_origins`.

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

