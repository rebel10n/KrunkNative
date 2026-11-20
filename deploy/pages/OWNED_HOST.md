# Owned front-end host: staging and cutover

`krunk.io` and `www.krunk.io` currently serve the browser app from GitHub Pages. The
new front end can instead be served by `kru` using the **same** `dist/web` built by
`make web`. It uses the existing `deploy/pages/site.sh` settings and `stage.py` to
generate `kn-config.json`, the `<meta>` entries read by `ui/js/loader.js`, the
Krunker Strike page and the content-hashed client JS/Wasm/data triplet. Assets
remain on `assets.krunk.io` / `assets1.krunk.io`, with `dl.krunk.io` as fallback.
The `<head>` hot loader is preserved from `dist/web/index.html` by staging.

## Current staging state (November 20, 2026)

- `kru` (SSH alias from `deploy/ansible/ssh_config`) has the isolated directory
  `/opt/krunknative-pages/releases/`, with `current` as an atomic symlink.
- Host nginx serves `/opt/krunknative-pages/current` at **127.0.0.1:18080** via
  `/etc/nginx/sites-enabled/zz-krunknative-pages-stage.conf` (tracked source:
  `deploy/pages/owned-stage.nginx.conf`). It has no public port, firewall or DNS
  route. The production app and downloads continue to use their existing vhosts.
- Staged release `804ff462006f02db` serves client build
  `e7df029ceee3c32d` and keeps the prior GitHub client build
  `daeb2112581dc60c` for cached pages. This is a preview of the available
  `dist/web` at staging time, **not** a CI-approved production release.
- `nginx -t` and `deploy/pages/owned.sh verify-stage` passed. The latter checks
  HTML, client JS/Wasm/data and MIME types, 404s, PWA files, `kn-config.json`,
  and all configured CDN origins. The Cloudflare Pages CDN may answer 200 to
  a range request; the checker warns but accepts it, as the client does not
  issue byte-range requests. No public DNS, certs or edge routes were changed.

## Repeatable staging (no DNS changes)

From the project root on the controller:

```sh
# Use a matching locally verified Web build for the intended commit.
make web
deploy/pages/owned.sh stage           # local dist/owned-pages only
deploy/pages/owned.sh publish-stage   # upload immutable release to kru loopback
deploy/pages/owned.sh verify-stage    # SSH tunnel + curl --resolve through real nginx
```

`publish-stage` snapshots current public DNS to a private `0600` file under
`~/.config/kn-pages/rollback-dns-*.txt` before it changes staging. It never
loads DNS API credentials or changes Spaceship. The user can independently run
`deploy/pages/owned.sh snapshot-dns` before a cutover. Keep this file and the
actual old record values and TTLs displayed in Spaceship, including any current
A, AAAA and CNAME answers. No secrets should be pasted into logs or tickets.

The script retains the currently public site's `client.<hash>.*` files using
`--keep-from https://krunk.io/`. To skip this network fetch for a purely local
fixture, set `KN_OWNED_STAGE_PREVIOUS=`. The staged folder excludes `assets/`.
Settings such as `KN_ASSET_ORIGIN`, `KN_ASSET_FALLBACK`, `KN_BRAIN_URL`,
`KN_BRIDGE_URL`, `KN_RTC_URL`, and `KN_WT_URL` work the same as on GitHub Pages.
The owned-stage default fallback is `dl`.

For a single direct check on `kru` without SSH forwarding, the following resolves
the **Host header** locally while leaving public DNS unchanged:

```sh
ssh -F deploy/ansible/ssh_config kru \
  'curl --noproxy "*" -fsSI --resolve krunk.io:18080:127.0.0.1 http://krunk.io:18080/client.e7df029ceee3c32d.wasm'
```

The `verify-stage` command uses a temporary local SSH tunnel, picks an unused
port and cleans it up. To inspect the preview in a local browser, open another
terminal and run `ssh -F deploy/ansible/ssh_config -N -L
127.0.0.1:18080:127.0.0.1:18080 kru`, then navigate to
`http://localhost:18080/`. Browser CORS and secure-context behavior still need
validation against the eventual HTTPS origin `https://krunk.io/`.

## Public cutover prerequisites (pending, not executed)

Public `91.199.82.174:80/443` on `divide` is currently fronted by the **edge
relay CT 201** (OpenResty). `kru` is CT 120 at **172.16.5.47**, not a public
IP. Merely changing the apex A record to `91.199.82.174` will **not** publish
the front end: CT 201 needs an HTTPS `krunk.io`/`www.krunk.io` route and a
private upstream to kru. Its current `20-krunkio.conf` includes the service
domains but no apex front-end route. This is separate from the existing
`kru` nginx :443 download/edge-origin vhosts.

Prepare and review these steps before editing DNS:

1. Run local CI for the chosen commit and build/stage its matching `dist/web`;
   check the deployed page's `kn-build`, hot-loader script, Wasm and 404s.
   Preserve the public GitHub Pages site, domain setting and current DNS as
   the rollback destination.
2. Add a **LAN-only** nginx origin vhost on kru, serving the same
   `/opt/krunknative-pages/current` with the rules in
   `owned-stage.nginx.conf`, bound to `172.16.5.47:18080`, allowing only
   CT 201 (`172.16.5.10`). Keep the loopback staging listener. Add a
   corresponding allow rule in the actual CT 120 firewall only if required;
   verify that the public Internet cannot reach the port.
3. Add a dedicated `krunk.io www.krunk.io` OpenResty vhost to CT 201,
   forwarding to `http://172.16.5.47:18080` with the original `Host` and
   `X-Forwarded-Proto: https` headers. Keep existing matchmaker, coordinator,
   play, assets and download routes untouched. Use the relay's existing
   TLS/ACME infrastructure and obtain a valid certificate for both names.
   To avoid an HTTPS certificate gap at cutover, arrange DNS-01 issuance or
   another verified pre-issuance path while GitHub DNS remains in place.
4. Confirm the edge route **before switching DNS**. For HTTP behavior, use
   `curl --resolve krunk.io:443:91.199.82.174 -k
   https://krunk.io/` (only `-k` while verifying an untrusted staging cert).
   Then verify with normal TLS validation once a valid certificate exists:
   `deploy/pages/check.sh https://krunk.io/
   --resolve krunk.io:443:91.199.82.174`.
   Test `www.krunk.io` the same way, plus a real browser hot-loader/matchmaking
   smoke test against the proposed HTTPS origin. Keep the old cached-client
   files available during the transition.
5. Only after approval and successful TLS/functional tests, change the
   Spaceship apex A records to `91.199.82.174`, remove the old GitHub AAAA
   records unless a matching working IPv6 ingress exists, and change the
   `www` CNAME from `rebel10n.github.io` to `krunk.io`. Verify authoritative
   and public DNS, HTTPS, hot matchmaking and all ancillary pages.

## Reviewable CT 201 ingress and certificate commands

Two prepared configurations are **not enabled**:

- `owned-origin.nginx.conf` binds `172.16.5.47:18080` on kru, accepts only
  `172.16.5.10` (CT 201), and serves the staged release using the same cache,
  MIME and 404 rules as the loopback preview.
- `owned-edge.openresty.conf` binds `krunk.io www.krunk.io` on the public edge
  relay's HTTP/HTTPS listeners, presents a dedicated certificate, and proxies
  to that private origin. It does not edit the existing service vhosts in
  `20-krunkio.conf`. Both templates passed syntax checks with the respective
  nginx/OpenResty binaries using separate temporary test configs; the edge
  check used the relay's existing fallback certificate only for syntax.

These are the exact installation actions for a **later approved cutover**. On
CT 201, `certbot` is installed. Provision a certificate **while the GitHub
records still answer**, using a DNS-01 challenge (the interactive command
prints TXT records for the operator to add in Spaceship; it does not change A
or CNAME records). Use a contact address in your normal ACME account setup.

```sh
# Human action: add the requested DNS TXT values, then confirm validation.
ssh -t -F deploy/ansible/ssh_config divide \
  'pct exec 201 -- certbot certonly --manual --preferred-challenges dns --cert-name krunk.io -d krunk.io -d www.krunk.io'

# Install the private origin on kru. Only CT 201 may reach this LAN listener.
scp -F deploy/ansible/ssh_config deploy/pages/owned-origin.nginx.conf \
  kru:/etc/nginx/sites-available/krunknative-pages-origin.conf
ssh -F deploy/ansible/ssh_config kru \
  'ln -s /etc/nginx/sites-available/krunknative-pages-origin.conf /etc/nginx/sites-enabled/zz-krunknative-pages-origin.conf && nginx -t && systemctl reload nginx'

# Verify inside CT 201 that its private network reaches the origin.
ssh -F deploy/ansible/ssh_config divide \
  'pct exec 201 -- curl --noproxy "*" -fsSI --resolve krunk.io:18080:172.16.5.47 http://krunk.io:18080/'

# Install the edge vhost only after both certificates exist and origin works.
scp -F deploy/ansible/ssh_config deploy/pages/owned-edge.openresty.conf \
  divide:/root/kn-pages-edge.conf
ssh -F deploy/ansible/ssh_config divide \
  'pct push 201 /root/kn-pages-edge.conf /usr/local/openresty/nginx/conf/conf.d/25-krunknative-pages.conf &&
   pct exec 201 -- /usr/local/openresty/nginx/sbin/nginx -t -p /usr/local/openresty/nginx -c conf/nginx.conf &&
   pct exec 201 -- systemctl reload openresty'

# Tests through the real public IP, while the public DNS still points to Pages.
deploy/pages/check.sh https://krunk.io/ --resolve krunk.io:443:91.199.82.174
deploy/pages/check.sh https://www.krunk.io/ --resolve www.krunk.io:443:91.199.82.174
```

If any config test fails, **do not reload**. Remove the newly added
`zz-krunknative-pages-origin.conf` symlink or `25-krunknative-pages.conf`
edge file, recheck syntax and leave live routing alone. The private CT 120
firewall must allow `172.16.5.10 -> 172.16.5.47:18080` before the origin
probe can pass. Do not open that port to the Internet. Confirm DNS-01
renewal automation (or schedule and monitor renewals using Spaceship's DNS
API) before relying on a manually issued certificate in production;
`certbot --manual` without hooks does not automatically renew the cert.

## Rollback

At any point before DNS cutover, leave public DNS untouched and disable the
new CT 201/CT 120 front-end routing if necessary. After cutover, immediately
restore the exact **record set and TTLs** saved before the change in Spaceship.
At staging time the publicly observed apex was GitHub's four IPv4 addresses
`185.199.108.153`, `.109.153`, `.110.153`, `.111.153` and four IPv6 addresses
`2606:50c0:8000::153`, `:8001::153`, `:8002::153`, `:8003::153`; `www`
was a CNAME to `rebel10n.github.io`. Use the saved records as authoritative
if anything changed since this note. Keep GitHub Pages enabled and the
`krunk.io` custom-domain setting intact until the new host is proven.

After restoring DNS, allow the old DNS TTL to expire before retiring the edge
route, so cached clients still pointing at the new address can reach the site.
Then remove the new edge include and the private origin link; this leaves the
loopback stage available for diagnosis:

```sh
ssh -F deploy/ansible/ssh_config divide \
  'pct exec 201 -- mv /usr/local/openresty/nginx/conf/conf.d/25-krunknative-pages.conf /root/kn-pages-edge-retired.conf &&
   pct exec 201 -- /usr/local/openresty/nginx/sbin/nginx -t -p /usr/local/openresty/nginx -c conf/nginx.conf &&
   pct exec 201 -- systemctl reload openresty'
ssh -F deploy/ansible/ssh_config kru \
  'rm /etc/nginx/sites-enabled/zz-krunknative-pages-origin.conf && nginx -t && systemctl reload nginx'
```

To roll back **just the staged content** on `kru`, use the prior release id
from `readlink /opt/krunknative-pages/current` and atomically flip the symlink
back, then rerun `deploy/pages/owned.sh verify-stage`. For example:

```sh
# Replace PREVIOUS_RELEASE with an existing entry under releases/.
ssh -F deploy/ansible/ssh_config kru \
  'test -d /opt/krunknative-pages/releases/PREVIOUS_RELEASE &&
   ln -s releases/PREVIOUS_RELEASE /opt/krunknative-pages/.rollback-next &&
   mv -Tf /opt/krunknative-pages/.rollback-next /opt/krunknative-pages/current'
deploy/pages/owned.sh verify-stage
```

Rollback of public ingress should preserve other CT 201 service names.
GitHub's DNS answer alone is insufficient for rollback if its Pages custom
domain or repository settings have meanwhile been removed.
