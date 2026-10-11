# deploy/ansible

Ansible for the KrunkNative central node (`kru`) and the edge game servers.

| playbook | hosts | what |
| --- | --- | --- |
| `central.yml` | `kru` | user `kn`, OpenResty 127.0.0.1:8081 (static web build), kn_brain finder :8082 + auth/community :8083, `/opt/krunknative` clone |
| `edges.yml` | `edges` | user `kn`, `server_bin` x (vcpus-1, min 1), `<edge>-<n>.krunker.party` DNS |
| `site.yml` | all | both |

Tags: `user openresty repo release brain web gameserver dns`.

Routing on kru (Cloudflare tunnel ingress, managed in Cloudflare): `krunker.party -> :8081`,
`matchmaker.krunker.party -> :8082`, `coordinator.krunker.party -> :8083`. Not managed here:
the host's nginx (:80/:443), cloudflared, the pm2 app krunkbox (:2025, kru.eli.gift), user `kru`.

## Run

```sh
cd deploy/ansible
ansible-galaxy collection install -r requirements.yml
TARGETS="kn_brain kn_migrate" ./build.sh bookworm   # dist/deploy/bookworm/bin (docker)
make -C ../.. web                                   # dist/web
ansible-playbook central.yml --check --diff
ansible-playbook central.yml                        # -e kn_web_sync_assets=false skips the 1 GB asset sync
set -a; . ~/.config/kn-secrets/cloudflare.env; set +a
ansible-playbook edges.yml --tags dns
```

Layout on hosts: `/opt/krunknative-svc/{releases/<id>,current}` (binaries, maps, migrations,
release config), `/opt/krunknative-web/{releases/<id>,current,assets}`, state
`/var/lib/krunknative`, secrets `/etc/krunknative/secrets.env` (generated once, root:kn 0640;
set `kn_brain_internal_secret` from an ansible-vault file to pin it). The last 3 releases are kept.

Switches: `kn_coordinator_db` (`sqlite` | `none` | `postgres`; sqlite falls back to `--no-db`
while the shipped kn_brain has no SQLite store), `kn_gameserver_enabled` (edges stay down until
true), `kn_gameserver_ports_open` (false on amph.ashburn: abulxt owns its nftables).

CI: `.github/workflows/deploy-kru.yml` (push to slop) builds and runs `central.yml` with
secrets `KRU_SSH_KEY KRU_HOST KRU_PORT KRU_KNOWN_HOSTS`.
