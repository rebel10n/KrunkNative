#!/bin/bash
# KrunkNative's self-hosted GitHub Actions runners on divide's CT 208 `ci` (deploy/ci-runner/README.md).
# Run as root inside the CT, idempotent:
#   KN_RUNNER_TOKEN=<registration token> ./setup-kn-runners.sh [count]
# (token: gh api -X POST repos/rebel10n/KrunkNative/actions/runners/registration-token --jq .token;
# without one, existing runners are kept and only the packages, user, slice and hooks are refreshed).
# The CT also hosts the Fracital org's ci-N runners (user `runner`): this script never touches them.
set -euo pipefail
count=${1:-6}
repo_url=https://github.com/rebel10n/KrunkNative
user=knci
home=/home/$user
version=2.338.2 # the runner the CT's other runners use (/home/runner/dl)
tarball=actions-runner-linux-x64-$version.tar.gz

# The Linux jobs' apt packages (.github/workflows/build.yml installs the same on GitHub-hosted runners).
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
	build-essential cmake ninja-build pkg-config shellcheck rsync ccache mold git git-lfs curl jq zip unzip xz-utils \
	libwayland-dev wayland-protocols libx11-dev libxcursor-dev libxinerama-dev libxrandr-dev libxi-dev \
	libgl1-mesa-dev libglu1-mesa-dev libxkbcommon-dev libxcb-present-dev libegl-dev \
	postgresql-client mariadb-client redis-server \
	python3-websockets python3-numpy python3-cryptography \
	libssl-dev libpq-dev libsqlite3-dev libmariadb-dev libhiredis-dev libcurl4-openssl-dev libwebp-dev
# the backend tests start their own redis-server (tests/with_redis.py)
systemctl disable --now redis-server >/dev/null 2>&1 || true

# Own user (not the Fracital runners' `runner`): own HOME, caches and work dirs; docker for the
# backend job's service containers.
id "$user" >/dev/null 2>&1 || useradd -m -s /bin/bash "$user"
usermod -aG docker "$user"
install -d -o "$user" -g "$user" "$home/dl" "$home/hooks" "$home/tmp" "$home/cache" "$home/.cache" "$home/.ccache"

# All KN runners share one slice: at most 12 of the CT's 28 cores and 24 GB, so Fracital's CI keeps headroom.
cat >/etc/systemd/system/kn-ci.slice <<'SLICE'
[Unit]
Description=KrunkNative CI runners (capped; deploy/ci-runner/README.md)

[Slice]
CPUQuota=1200%
MemoryHigh=20G
MemoryMax=24G
TasksMax=16384
SLICE

# Job hook: kill what an earlier job of this runner left in its service cgroup (servers, test daemons on
# fixed ports) and clear the runner's TMPDIR. Never fails the job.
cat >"$home/hooks/job.sh" <<'HOOK'
#!/bin/bash
set -u
cg=$(sed -n 's/^0:://p' /proc/self/cgroup)
procs=/sys/fs/cgroup$cg/cgroup.procs
declare -A keep
p=$$
while [[ $p =~ ^[0-9]+$ ]] && ((p > 1)); do
	keep[$p]=1
	p=$(sed -E 's/^.*\) [A-Za-z] ([0-9]+) .*$/\1/' "/proc/$p/stat" 2>/dev/null) || break
done
killed=0
if [[ $cg == /kn.slice/kn-ci.slice/* || $cg == /kn-ci.slice/* ]] && [[ -r $procs ]]; then
	for pid in $(<"$procs"); do
		[[ -n ${keep[$pid]:-} ]] && continue
		comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
		case $comm in Runner.Listener | Runner.Worker | Runner.PluginHo* | runsvc.sh) continue ;; esac
		kill -KILL "$pid" 2>/dev/null && killed=$((killed + 1))
	done
fi
if [[ ${TMPDIR:-} == /home/knci/tmp/kn-* && -d $TMPDIR ]]; then
	find "$TMPDIR" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null
fi
echo "kn ci hook (${RUNNER_NAME:-?}): killed $killed leftover processes, cleaned tmp"
exit 0
HOOK
chown "$user:$user" "$home/hooks/job.sh"
chmod 755 "$home/hooks/job.sh"

[[ -f $home/dl/$tarball ]] ||
	sudo -u "$user" curl -fsSL -o "$home/dl/$tarball" \
		"https://github.com/actions/runner/releases/download/v$version/$tarball"

for n in $(seq 1 "$count"); do
	name=kn-$n
	dir=$home/$name
	labels=self-hosted,linux,x64,kn-ci
	((n == 1)) && labels=$labels,kn-db # the backend job's service containers bind 5432 / 3306: one runner
	if [[ ! -f $dir/.runner ]]; then
		[[ -n ${KN_RUNNER_TOKEN:-} ]] || { echo "$name: not registered and no KN_RUNNER_TOKEN, skipped"; continue; }
		install -d -o "$user" -g "$user" "$dir" "$home/tmp/$name" "$home/cache/$name"
		sudo -u "$user" tar -xzf "$home/dl/$tarball" -C "$dir"
		sudo -u "$user" "$dir/config.sh" --unattended --url "$repo_url" --token "$KN_RUNNER_TOKEN" \
			--name "ci208-$name" --labels "$labels" --work _work --replace
		(cd "$dir" && ./svc.sh install "$user" >/dev/null)
	fi
	install -d -o "$user" -g "$user" "$home/tmp/$name" "$home/cache/$name"
	# JOBS: the Makefile's --parallel (it defaults to nproc = 28); the slice caps the CPU anyway.
	# CCACHE_DIR: one compiler cache for all KN runners (CMakeLists.txt KRUNKNATIVE_FAST_BUILD picks up
	# ccache and mold), kept across jobs: actions/checkout cleans the build dirs, not this.
	cat >"$dir/.env" <<ENV
LANG=C.UTF-8
TMPDIR=$home/tmp/$name
XDG_CACHE_HOME=$home/cache/$name
JOBS=8
CCACHE_DIR=$home/.ccache
CCACHE_MAXSIZE=30G
ImageOS=ubuntu26
ACTIONS_RUNNER_HOOK_JOB_STARTED=$home/hooks/job.sh
ACTIONS_RUNNER_HOOK_JOB_COMPLETED=$home/hooks/job.sh
ENV
	chown "$user:$user" "$dir/.env"
	unit=$(cat "$dir/.service")
	install -d "/etc/systemd/system/$unit.d"
	cat >"/etc/systemd/system/$unit.d/kn.conf" <<UNIT
[Service]
Slice=kn-ci.slice
LimitNOFILE=1048576
UNIT
done
systemctl daemon-reload
for d in "$home"/kn-*; do
	[[ -f $d/.service ]] && systemctl restart "$(cat "$d/.service")"
done
systemctl --no-pager --no-legend list-units 'actions.runner.rebel10n-KrunkNative.*'
