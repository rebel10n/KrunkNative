# Self-hosted CI runners (divide CT 208 `ci`)

The Build workflow's Linux jobs (`meta`, `linux`, `web`, `services`) run on KrunkNative's own
GitHub Actions runners in divide's CT 208 `ci` (172.16.5.208, Ubuntu 26.04, 28 cores, 64 GB), not on
GitHub-hosted `ubuntu-latest`. Windows, macOS, iOS, Android and every packaging / deploy workflow
(client.yml, release.yml, deploy-*.yml) stay GitHub-hosted: their Linux binaries are built on Ubuntu 22.04
for the glibc they ship against, and their secrets stay where they were.

- **Not here: `check`.** Its "Test game content" step downloads from `dl.krunk.io`, which resolves to
  divide's own public address. CT 208's Proxmox firewall (`/etc/pve/firewall/208.fw`: untrusted job code,
  outbound internet only) drops that address and every private range, so no job here reaches kru, the
  game servers or any other `*.krunk.io` name on divide. That is deliberate; `check` stays GitHub-hosted.
  The Cloudflare CDN (`assets.krunk.io`) is reachable, but it mirrors paths only, not the content index.
- **Runners:** `ci208-kn-1` .. `ci208-kn-6`, repo-level on rebel10n/KrunkNative, labels
  `self-hosted,linux,x64,kn-ci`; `ci208-kn-1` also has `kn-db`, the only label the backend job (`services`)
  takes, because its PostgreSQL / MariaDB service containers bind host ports 5432 / 3306 and two at once
  would collide.
- **User:** `knci` (in `docker`), runners in `/home/knci/kn-N`, per-runner `TMPDIR` / `XDG_CACHE_HOME` under
  `/home/knci/{tmp,cache}/kn-N`, `JOBS=8` (the Makefile's `--parallel`, else nproc = 28), one shared
  ccache (`CCACHE_DIR=/home/knci/.ccache`, 30 GB) that CMake's `KRUNKNATIVE_FAST_BUILD` uses with mold.
- **Speed:** the CT runs at `cpuunits: 25` on divide (game servers come first), so a cold build is slower
  than GitHub-hosted under load; ccache makes the usual few-file rebuild fast.
- **Shared CT:** the CT also runs the Fracital org's 20 `ci-N` runners as user `runner`. Never touch them;
  the KN setup has its own user, units, hooks and slice.
- **Caps:** every KN runner unit is in `kn-ci.slice` (`/kn.slice/kn-ci.slice`): `CPUQuota=1200%` (12 of 28
  cores), `MemoryHigh=20G`, `MemoryMax=24G`. Docker service containers run in Docker's own cgroup.
- **Job hook:** `/home/knci/hooks/job.sh` (job started + completed) kills processes an earlier job left in
  the runner's cgroup (servers, test daemons on fixed ports) and empties its `TMPDIR`.
- **Packages:** preinstalled by the setup script (the same list the workflow's `Install build
  dependencies` steps apt-install; those steps run only when `runner.environment == 'github-hosted'`).
  Node, Go and emsdk come from `actions/setup-node`, `setup-go` and `setup-emsdk` as on GitHub-hosted.
  The stock `redis-server` service is disabled (the tests start their own).

## Setup / refresh

As root in the CT (idempotent; without a token it refreshes packages, user, slice, hooks and `.env` and
keeps the registered runners):

```sh
tok=$(gh api -X POST repos/rebel10n/KrunkNative/actions/runners/registration-token --jq .token)
ssh -F deploy/ansible/ssh_config divide "pct exec 208 -- env KN_RUNNER_TOKEN=$tok bash -s 6" \
  < deploy/ci-runner/setup-kn-runners.sh
```

A new runner version: bump `version` in the script, then per runner stop the unit, unpack the new
tarball over `/home/knci/kn-N` and start it (the runner also self-updates when GitHub requires it).

## Fallback to GitHub-hosted

If CT 208 is down, set the repo variables (Settings > Variables > Actions) `KN_LINUX_RUNNER` and
`KN_DB_RUNNER` to `"ubuntu-latest"` (JSON, with the quotes); the jobs read
`fromJSON(vars.KN_LINUX_RUNNER || '["self-hosted", "kn-ci"]')`. Delete them to go back.

## Local CI and hand deploys (no GitHub Actions)

Every GitHub account we push with has ended up flagged (pushes start no workflow runs), so CI and
deploys also run from an operator's machine, on the same hosts: `tools/ci/local-ci.sh` and
`tools/ci/local-deploy.sh` (user, 2026-11-20: "CI all locally on our mac and ci ... you are authorized to
deploy").

```sh
tools/ci/local-ci.sh origin/main          # ~/.cache/kn-ci/<commit12>/{summary.txt,<job>.log,PASS}
tools/ci/local-deploy.sh origin/main      # refuses without that commit's PASS
tools/ci/local-deploy.sh --dry-run --only kru,edges <rev>   # builds, ansible --check --diff
```

`local-ci.sh` exports the commit with `git archive` (never the shared checkout) and runs build.yml's jobs
in parallel:

| job | host | what |
| --- | --- | --- |
| meta | this machine | `version-meta.sh` (and `--server`): needs the history and tags |
| check | CT 208 | `make check` + the workflow's shellcheck; the "Test game content" files come from this checkout's `assets/` (CT 208 cannot reach dl.krunk.io) |
| linux | CT 208 | `make release`, `proc_input_test`, `ui_test lexbor-probe` |
| web | CT 208 | `make web` (emsdk 6.0.12 in `/home/knci/kn-local/emsdk`), the GitHub Pages stage check |
| services | CT 208 | the backend tests on PostgreSQL 18 + MariaDB 11.8 containers (host ports 5432 / 3306, one run at a time: `flock`) |
| windows | this machine | `build-client.sh windows` with `mingw64-cmake` (Fedora), `package-client.sh windows-x64`, `wine --version` as a smoke test |
| macos | VM 126 `mac` | `build-client.sh macos` (x86_64, the pinned prebuilt ANGLE, static OpenSSL cached in `~/kn-ci/openssl-*`), `proc_input_test`, `ui_test`, `package-client.sh macos-x64`, codesign check, `--version` |
| android, ios, macos-arm64 | - | skipped: no Android SDK, no Xcode on the mac (Command Line Tools only), no Apple silicon |

CT 208 jobs run as `knci` inside `kn-ci.slice` (`systemd-run --slice=kn-ci.slice`, the runners' caps), with
work trees in `/home/knci/kn-local/work/<commit12>/` (removed after the run unless `--keep`). The mac's
tools (cmake 3.31.6, ninja 1.12.1) are in `~/kn-ci/tools/bin`; nothing is installed system-wide there, and
its existing `~/actions-runner` is not ours. Package artifacts (Windows, macOS) are copied to
`~/.cache/kn-ci/<commit12>/artifacts/`.

`local-deploy.sh` does deploy-kru.yml, deploy-edges.yml and assets-cdn.yml from this machine with the
operator's ssh keys (deploy/ansible/ssh_config, ProxyJump divide): `make web` + kn_brain / kn_migrate
(bookworm container) + `central.yml`; server_bin + kn_rentd (bullseye / bookworm / trixie) + `edges.yml` on
kn-mnz (ashburn, the only edge), its health (instance 1 answers with this build's server commit); the asset
CDN when it lacks dl.krunk.io's `assets/version.txt`. Secrets come from `~/.config/kn-secrets/` (smtp, ai,
discord env files: a missing one keeps kru's file; the CDN's `github-ci/CLOUDFLARE_*`). Two things still
need GitHub and are not done by hand: the krunk.io front end on GitHub Pages (a `build_type=workflow`
site is only published by Actions; kru's own copy of the web build, role kn_web, does go out) and the
`client-latest` / `server-latest` releases (their publish replaces every platform's asset, and arm64,
Android and iOS have no build host here).
