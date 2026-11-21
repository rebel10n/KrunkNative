# Owned front-end host: staging and cutover

`krunk.io` and `www.krunk.io` currently serve the browser app from GitHub Pages. The
new front end can instead be served by `kru` using the **same** `dist/web` built by
`make web`. It uses the existing `deploy/pages/site.sh` settings and `stage.py` to
generate `kn-config.json`, the `<meta>` entries read by `ui/js/loader.js`, the
Krunker Strike page and the content-hashed client JS/Wasm/data triplet. Assets
remain on `assets.krunk.io` / `assets1.krunk.io`, with `dl.krunk.io` as fallback.
The `<head>` hot loader is preserved from `dist/web/index.html` by staging.

## Live state (cut over 2026-11-21 ~01:50 UTC)

`krunk.io` and `www.krunk.io` are served by **kru**, not GitHub Pages:

```
browser -> 91.199.82.174:443 (divide DNAT) -> edge relay CT 201 OpenResty
           conf.d/25-krunknative-pages.conf (owned-edge.openresty.conf), cert /etc/letsencrypt/live/krunk.io
        -> http://172.16.5.47:80 Host krunk.io (kru host nginx, owned-origin.nginx.conf,
           sites-enabled/zz-krunknative-pages-origin.conf, allow 172.16.5.10 only)
        -> /opt/krunknative-pages/current -> releases/<id>
```

- The origin uses kru's existing wildcard `:80` listener (selected by `server_name`), because
  divide's `divide_guests` `from_edge` chain and CT 120's `/etc/pve/firewall/120.fw` already pass
  edge -> kru tcp 80/443; no firewall rule was added. Never make it `listen 172.16.5.47:80`: an
  address-specific listener takes that address from kru's other `:80` vhosts (`kru.eli.gift`).
- Certificate: certbot on CT 201, DNS-01 through Spaceship's API with
  `/etc/letsencrypt/kn-spaceship-hook.py` (= `certbot-spaceship-hook.py`; credentials in
  `/etc/letsencrypt/kn-spaceship.env`, root 0600). The renewal config keeps the auth/cleanup hooks
  and `renew_hook = systemctl reload openresty`, so `certbot.timer` renews unattended. Check with
  `pct exec 201 -- certbot renew --dry-run --cert-name krunk.io`.
- DNS at Spaceship: `@ A 91.199.82.174` (TTL 300), `www CNAME krunk.io` (TTL 300), no AAAA (divide
  has no inbound IPv6 for the relay). Rollback snapshot of the whole zone, from Spaceship's API:
  `~/.config/kn-pages/rollback-spaceship-20261121T014946Z.json` (0600). The old values were
  `@ CNAME rebel10n.github.io` and `www CNAME rebel10n.github.io`, both TTL 1800.
- The GitHub Pages site and its `krunk.io` custom-domain setting are left in place as the rollback.
- Served release `ad0ed3f80c5edd97` (client build `00b9da7e4e577ebd`, from `aaf774e7`).
- `127.0.0.1:18080` (`owned-stage.nginx.conf`) remains for `owned.sh verify-stage`.

Publishing a new front end is now: `make web`, `owned.sh stage`, `owned.sh publish-stage`,
`owned.sh verify-stage`, then `check.sh https://krunk.io/` (the stage *is* production).

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

## Cutover procedure (done 2026-11-21; kept for a rebuild)

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
   `owned-stage.nginx.conf`, on kru's wildcard `:80` by `server_name`, allowing only
   CT 201 (`172.16.5.10`). Keep the loopback staging listener. Add a
   corresponding allow rule in the actual CT 120 firewall only if required;
   verify that the public Internet cannot reach the port.
3. Add a dedicated `krunk.io www.krunk.io` OpenResty vhost to CT 201,
   forwarding to `http://172.16.5.47:80` with the original `Host` and
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

## CT 201 ingress and certificate commands (as executed)

Two prepared configurations are **not enabled**:

- `owned-origin.nginx.conf` joins kru's wildcard `:80` for `krunk.io www.krunk.io`, accepts only
  `172.16.5.10` (CT 201), and serves the staged release using the same cache,
  MIME and 404 rules as the loopback preview.
- `owned-edge.openresty.conf` binds `krunk.io www.krunk.io` on the public edge
  relay's HTTP/HTTPS listeners, presents a dedicated certificate, and proxies
  to that private origin. It does not edit the existing service vhosts in
  `20-krunkio.conf`. Both templates passed syntax checks with the respective
  nginx/OpenResty binaries using separate temporary test configs; the edge
  check used the relay's existing fallback certificate only for syntax.

These are the exact installation actions for a **later approved cutover**. On
CT 201, `certbot` is installed. The certificate is issued by DNS-01 while the old
records still answer: `certbot-spaceship-hook.py` (installed as
`/etc/letsencrypt/kn-spaceship-hook.py`, credentials in `/etc/letsencrypt/kn-spaceship.env`,
root 0600) writes and removes only the `_acme-challenge` TXT records through Spaceship's API
and waits until both authoritative servers answer them.

```sh
# Issue (or reissue) with the hooks; certbot.timer renews with the same hooks.
ssh -t -F deploy/ansible/ssh_config divide \
  'pct exec 201 -- certbot certonly --non-interactive --agree-tos --manual --preferred-challenges dns --cert-name krunk.io -d krunk.io -d www.krunk.io \
     --manual-auth-hook "/etc/letsencrypt/kn-spaceship-hook.py auth" --manual-cleanup-hook "/etc/letsencrypt/kn-spaceship-hook.py cleanup" \
     --deploy-hook "systemctl reload openresty"'

# Install the private origin on kru. Only CT 201 may reach this LAN listener.
scp -F deploy/ansible/ssh_config deploy/pages/owned-origin.nginx.conf \
  kru:/etc/nginx/sites-available/krunknative-pages-origin.conf
ssh -F deploy/ansible/ssh_config kru \
  'ln -s /etc/nginx/sites-available/krunknative-pages-origin.conf /etc/nginx/sites-enabled/zz-krunknative-pages-origin.conf && nginx -t && systemctl reload nginx'

# Verify inside CT 201 that its private network reaches the origin.
ssh -F deploy/ansible/ssh_config divide \
  'pct exec 201 -- curl --noproxy "*" -fsSI -H "Host: krunk.io" http://172.16.5.47/'

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
edge file, recheck syntax and leave live routing alone. The origin rides kru's
`:80`, which divide's `from_edge` chain and CT 120's firewall already pass from
CT 201; no port is opened to the Internet. `certbot renew --dry-run --cert-name
krunk.io` on CT 201 exercises the renewal hooks.

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

## Discord link (borrowed callback)

The Discord app the brain uses ("Meal") belongs to abulxt's Discord account, and an app's redirect
list can be edited only in its owner's Developer Portal (Discord's API cannot change it). Its list has
`https://abstracteducation.loan/api/account/discord/callback`, a name divide's edge relay serves, so
the link borrows it:

- kn_brain (`kn_brain_discord_redirect_uri`, `kn_brain_discord_state_prefix` in
  `deploy/ansible/roles/kn_brain/defaults/main.yml`) sends that `redirect_uri` and a state starting
  `kbo-`, and uses the same `redirect_uri` for the token exchange; `/discord/callback` takes the prefix
  off before looking the state up (`services/brain/discord.c`).
- CT 201's `00-edge-relay.conf` (Fracital/infra `roles/edge_relay/templates/byod-edge.conf.j2`, map
  `$kn_discord_bounce`) answers a callback for that host whose state starts `kbo-` with
  `302 https://coordinator.krunk.io/discord/callback?<the same query>`; every other callback still
  goes to abulxt. Applied live 2026-11-21 (backup `/root/00-edge-relay.conf.pre-kbo-discord` on CT 201).
- Check: `curl -sI 'https://abstracteducation.loan/api/account/discord/callback?code=x&state=kbo-x'`
  must 302 to the coordinator, and a state without `kbo-` must not.
- If the owner ever registers `https://coordinator.krunk.io/discord/callback` in the portal, set
  `kn_brain_discord_redirect_uri: ""` and `kn_brain_discord_state_prefix: ""` (the brain's default
  `<public_url>/discord/callback`) and drop the edge map.
