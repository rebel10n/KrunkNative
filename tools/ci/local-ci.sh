#!/usr/bin/env bash
# Local CI without GitHub Actions (deploy/ci-runner/README.md "Local CI"): runs the jobs of
# .github/workflows/build.yml (+ the client.yml / release.yml package builds that have a host here) for
# one commit, in parallel, on our own machines:
#
#   CT 208 `ci` on divide (as user knci, in kn-ci.slice): meta (Version), check (make check + shellcheck),
#                                                       linux (Linux GCC), web (Emscripten), services
#                                                       (Backend tests: PostgreSQL 18, MariaDB 11.8 containers)
#   this machine (Fedora, mingw64-cmake):               windows (MinGW-w64 cross build + package, wine --version)
#   VM 126 `mac` on divide (macOS x86_64, 2 vCPU):      macos (ANGLE/Metal client + server, proc_input_test,
#                                                       ui_test, package BoxOffice-macos-x64.zip)
#   skipped (no host): android (no Android SDK here), ios (the mac has no Xcode), macOS arm64 (no Apple silicon)
#
# The commit is exported with `git archive` (never the shared, dirty checkout). Logs and results go to
# ~/.cache/kn-ci/<commit12>/ (<job>.log, <job>.status = "pass|fail|skip <seconds> <note>", summary.txt);
# PASS is written there when every job that ran passed (tools/ci/local-deploy.sh requires it).
#
# Usage: tools/ci/local-ci.sh [--only job,job] [--keep] <rev>
#   --only  (re)run these jobs only (meta always runs), keeping the other jobs' results of an earlier run:
#           PASS is written once every job's latest result passes
#   --keep  keep the remote work trees (default: removed after the run)
# Env: KN_CI_OUT (default ~/.cache/kn-ci), KN_CI_JOBS (make -j on CT 208, default 8)
set -euo pipefail

only="" keep=0
while [ $# -gt 1 ]; do
    case "$1" in
        --only) only=",$2,"; shift 2 ;;
        --keep) keep=1; shift ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || { sed -n '2,24p' "$0" >&2; exit 2; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
sha="$(git -C "$repo" rev-parse --verify "$1^{commit}")"
s12="${sha:0:12}"
out="${KN_CI_OUT:-$HOME/.cache/kn-ci}/$s12"
jobs_ct="${KN_CI_JOBS:-8}"
sshcfg="$repo/deploy/ansible/ssh_config"
ct_work="/home/knci/kn-local/work/$s12"
mac_work="kn-ci/work/$s12"
if [ -z "$only" ]; then rm -rf "$out"; fi
mkdir -p "$out"
rm -f "$out/PASS"
want() { [ -z "$only" ] || [[ "$only" == *",$1,"* ]]; }
log() { printf '[local-ci %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

# ssh to divide, then into CT 208 as knci inside kn-ci.slice; the script comes on stdin
ct() {
    ssh -F "$sshcfg" -o BatchMode=yes -o ServerAliveInterval=30 divide \
        "pct exec 208 -- systemd-run --quiet --pipe --wait --collect --uid=knci --gid=knci --slice=kn-ci.slice -p TimeoutStartSec=infinity -E HOME=/home/knci -E USER=knci --working-directory=/home/knci bash -s" 2> >(grep -v -E 'post-quantum|store now|openssh.com/pq' >&2)
}
on_mac() { ssh -o BatchMode=yes -o ServerAliveInterval=30 mac bash -s 2> >(grep -v -E 'post-quantum|store now|openssh.com/pq' >&2); }

# The environment every CT 208 job starts with (the runners' .env, deploy/ci-runner/setup-kn-runners.sh)
ct_env() {
    cat << EOF
set -euo pipefail
node_dir=\$(ls -d /home/knci/kn-*/_work/_tool/node/22.*/x64 2> /dev/null | sort -V | tail -1)
go_dir=\$(ls -d /home/knci/kn-*/_work/_tool/go/*/x64 2> /dev/null | sort -V | tail -1)
export PATH="\$node_dir/bin:\$go_dir/bin:/home/knci/go/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export JOBS=$jobs_ct CCACHE_DIR=/home/knci/.ccache EMSDK=/home/knci/kn-local/emsdk
export XDG_CACHE_HOME=/home/knci/kn-local/cache/$s12 TMPDIR=/home/knci/kn-local/tmp/$s12
mkdir -p "\$XDG_CACHE_HOME" "\$TMPDIR"
EOF
}

# run_job <name> <runner fn> <script file>: background, timed, status file
pids=()
run_job() {
    local name="$1" fn="$2" script="$3"
    want "$name" || { [ -f "$out/$name.status" ] || echo "skip 0 not selected" > "$out/$name.status"; return 0; }
    (
        t0=$(date +%s)
        if "$fn" < "$script" > "$out/$name.log" 2>&1; then st=pass; else st=fail; fi
        echo "$st $(( $(date +%s) - t0 )) ${note:-}" > "$out/$name.status"
        log "$name: $st ($(( $(date +%s) - t0 )) s)"
    ) &
    pids+=($!)
}
skip_job() { echo "skip 0 $2" > "$out/$1.status"; }

# ---- meta (Version): here, where the full history and tags are -------------------------------------
t0=$(date +%s)
if "$repo/tools/release/version-meta.sh" "$sha" > "$out/meta.txt" 2> "$out/meta.log" \
    && "$repo/tools/release/version-meta.sh" --server "$sha" > "$out/meta-server.txt" 2>> "$out/meta.log"; then
    echo "pass $(( $(date +%s) - t0 ))" > "$out/meta.status"
else
    echo "fail $(( $(date +%s) - t0 ))" > "$out/meta.status"; cat "$out/meta.log" >&2; exit 1
fi
cmake_flags="$(sed -n 's/^cmake_flags=//p' "$out/meta.txt")"
meta() { sed -n "s/^$1=//p" "$out/meta.txt"; }
pkg_env="KN_COMMIT=$(meta commit) KN_COMMIT_TIME=$(meta commit_time) KN_BUILD_TIME=$(meta build_time) KN_PROTOCOL=$(meta protocol)"
log "commit $sha, logs in $out"

# ---- sources ---------------------------------------------------------------------------------------
need_ct=0; for j in check linux web services; do want "$j" && need_ct=1; done
need_mac=0; want macos && need_mac=1
if [ $need_ct = 1 ]; then
    log "CT 208: staging the source"
    git -C "$repo" archive --format=tar "$sha" | ssh -F "$sshcfg" -o BatchMode=yes divide \
        "pct exec 208 -- su -s /bin/bash knci -c 'rm -rf $ct_work && mkdir -p $ct_work/src && tar -x -C $ct_work/src'" \
        2> >(grep -v -E 'post-quantum|store now|openssh.com/pq' >&2)
fi
if [ $need_mac = 1 ]; then
    log "mac: staging the source"
    git -C "$repo" archive --format=tar "$sha" | ssh -o BatchMode=yes mac \
        "rm -rf ~/$mac_work && mkdir -p ~/$mac_work/src && tar -x -C ~/$mac_work/src" \
        2> >(grep -v -E 'post-quantum|store now|openssh.com/pq' >&2)
fi

# ---- check: make check + shellcheck (build.yml `check`) --------------------------------------------
if want check; then
    # "Test game content": CT 208 cannot reach dl.krunk.io (208.fw), so the files come from this checkout
    tar -c -C "$repo" assets/models/melee assets/textures/melee assets/textures/weapons/pat assets/user-assets/46802 \
        | ssh -F "$sshcfg" -o BatchMode=yes divide \
            "pct exec 208 -- su -s /bin/bash knci -c 'mkdir -p $ct_work/content && tar -x -C $ct_work/content'" \
            2> >(grep -v -E 'post-quantum|store now|openssh.com/pq' >&2)
fi
{
    ct_env
    cat << EOF
cp -al '$ct_work/src' '$ct_work/check' && cd '$ct_work/check'
cp -a '$ct_work/content/assets/.' assets/
command -v actionlint > /dev/null || go install github.com/rhysd/actionlint/cmd/actionlint@v1.7.12
(cd net-bridge && npm ci --no-audit --no-fund)
make check
shellcheck -S warning tools/release/*.sh tools/angle/*.sh deploy/ansible/build.sh tools/ci/*.sh
EOF
} > "$out/.check.sh"
run_job check ct "$out/.check.sh"

# ---- linux: Linux (GCC) ----------------------------------------------------------------------------
{
    ct_env
    cat << EOF
cp -al '$ct_work/src' '$ct_work/linux' && cd '$ct_work/linux'
make release CMAKE_FLAGS="$cmake_flags"
./cmake-build-release/proc_input_test
./cmake-build-release/ui_test lexbor-probe
EOF
} > "$out/.linux.sh"
run_job linux ct "$out/.linux.sh"

# ---- web: Emscripten / WebGL2 + the GitHub Pages stage check ---------------------------------------
{
    ct_env
    cat << EOF
cp -al '$ct_work/src' '$ct_work/web' && cd '$ct_work/web'
source "\$EMSDK/emsdk_env.sh" > /dev/null 2>&1
emcc -v > "\$TMPDIR/emcc-v.txt" 2>&1; sed -n 1p "\$TMPDIR/emcc-v.txt"
make web
for f in index.html client.js client.wasm client.data js/kn-shim.js js/loader.js; do test -f "dist/web/\$f"; done
KN_PAGES_LIVE_URL="" KN_GH_PAGES_STAGE="\$TMPDIR/gh-pages" deploy/pages/github.sh
cat "\$TMPDIR/gh-pages/kn-config.json"
EOF
} > "$out/.web.sh"
run_job web ct "$out/.web.sh"

# ---- services: Backend tests (PostgreSQL 18, MariaDB 11.8, SQLite, Redis) --------------------------
# The containers bind host ports 5432 / 3306 (the tests' defaults): one run at a time per CT (flock).
{
    ct_env
    cat << EOF
cp -al '$ct_work/src' '$ct_work/services' && cd '$ct_work/services'
exec 9> /home/knci/kn-local/services.lock
flock 9
pg=kn-local-pg-$s12 my=kn-local-my-$s12
cleanup() { docker rm -f "\$pg" "\$my" > /dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup
docker run -d --name "\$pg" -e POSTGRES_USER=krunknative -e POSTGRES_PASSWORD=kn-ci-only -e POSTGRES_DB=krunknative \
    -p 127.0.0.1:5432:5432 postgres:18 > /dev/null
docker run -d --name "\$my" -e MARIADB_ALLOW_EMPTY_ROOT_PASSWORD=1 -p 127.0.0.1:3306:3306 mariadb:11.8 > /dev/null
for i in \$(seq 60); do docker exec "\$pg" pg_isready -U krunknative > /dev/null 2>&1 && break; sleep 2; done
for i in \$(seq 90); do docker exec "\$my" healthcheck.sh --connect --innodb_initialized > /dev/null 2>&1 && break; sleep 2; done
export PGPASSWORD=kn-ci-only
python3 -I - << 'PY'
import json
c = json.load(open("config/config.example.json"))
c["database"].update(driver="postgres", host="127.0.0.1", port=5432, name="krunknative",
                     user="krunknative", password="kn-ci-only")
json.dump(c, open("config/config.json", "w"), indent=2)
PY
for d in krunknative_svccore krunknative_svcacc; do
    psql -h 127.0.0.1 -U krunknative -d krunknative -qAt -c "CREATE DATABASE \$d"
done
make configure
cmake --build cmake-build-debug --parallel \$JOBS --target services_all server_bin
cd cmake-build-debug
ctest -L 'knsvc|db' --timeout 900 --output-on-failure -j 4
EOF
} > "$out/.services.sh"
run_job services ct "$out/.services.sh"

# ---- windows: MinGW-w64 cross build on this machine (build.yml / client.yml `windows`) -------------
local_win="$out/windows"
cat > "$out/.windows.sh" << EOF
set -euo pipefail
rm -rf '$local_win' && mkdir -p '$local_win/src'
git -C '$repo' archive --format=tar '$sha' | tar -x -C '$local_win/src'
cd '$local_win/src'
export CMAKE=mingw64-cmake KN_CMAKE_ARGS="$cmake_flags" KN_BUILD_DIR='$local_win/build-windows'
tools/release/build-client.sh windows '$local_win/win-bin'
$pkg_env tools/release/package-client.sh windows-x64 '$local_win/win-bin' '$local_win/pkg' --channel latest
rm -rf '$local_win/t' && mkdir '$local_win/t' && unzip -q '$local_win/pkg/BoxOffice-windows-x64.zip' -d '$local_win/t'
# wine: a smoke test only (a missing wine or display is not a failure of the build)
if command -v wine > /dev/null; then
    for exe in runtime/client.exe runtime/server.exe BoxOffice.exe; do
        WINEDEBUG=-all timeout 120 wine '$local_win/t/'\$exe --version 2>&1 | tail -2 || echo "wine \$exe: no answer (not fatal)"
    done
fi
ls -la '$local_win/pkg'
EOF
local_run() { bash -s; }
run_job windows local_run "$out/.windows.sh"

# ---- macos: VM 126, x86_64 (build.yml `macos` + client.yml `macos` x64) ----------------------------
angle_pin="$(awk '$1 == "commit" { print substr($2, 1, 12) }' "$repo/tools/angle/angle.lock")"
ossl_key="$(sha256sum "$repo/tools/release/build-openssl.sh" | cut -c1-12)"
cat > "$out/.macos.sh" << EOF
set -euo pipefail
export PATH="\$HOME/kn-ci/tools/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
cd ~/$mac_work/src
export ANGLE_DIR="\$HOME/kn-ci/angle-$angle_pin-x86_64" KN_OPENSSL_PREFIX="\$HOME/kn-ci/openssl-$ossl_key"
[ -f "\$ANGLE_DIR/lib/libEGL.dylib" ] || [ "\$(tools/angle/fetch-angle.sh macos-x86_64 "\$ANGLE_DIR")" = available=true ]
export KN_CMAKE_ARGS="$cmake_flags" KN_BUILD_DIR=~/$mac_work/build-macos
tools/release/build-client.sh macos ~/$mac_work/bin
cmake --build ~/$mac_work/build-macos --target proc_input_test ui_test
~/$mac_work/build-macos/proc_input_test
~/$mac_work/build-macos/ui_test lexbor-probe
$pkg_env tools/release/package-client.sh macos-x64 ~/$mac_work/bin ~/$mac_work/pkg --channel latest
rm -rf ~/$mac_work/t && mkdir ~/$mac_work/t && unzip -q ~/$mac_work/pkg/BoxOffice-macos-x64.zip -d ~/$mac_work/t
codesign --verify --deep --strict ~/$mac_work/t/BoxOffice.app
(cd / && ~/$mac_work/t/runtime/client_bin --version)
(cd / && ~/$mac_work/t/runtime/client_bin --voice-info) | grep '^opus=libopus '
ls -la ~/$mac_work/pkg
EOF
run_job macos on_mac "$out/.macos.sh"

skip_job android "no Android SDK on our hosts"
skip_job ios "the mac VM has no Xcode (Command Line Tools only)"
skip_job macos-arm64 "no Apple silicon host"

# ---- wait, collect --------------------------------------------------------------------------------
rc=0
for p in "${pids[@]}"; do wait "$p" || true; done

# copy the package artifacts back (local-deploy.sh may publish them)
mkdir -p "$out/artifacts"
if want macos && [ "$(cut -d' ' -f1 "$out/macos.status")" = pass ]; then
    scp -q -o BatchMode=yes "mac:$mac_work/pkg/*" "$out/artifacts/" 2> /dev/null || true
fi
if want windows && [ "$(cut -d' ' -f1 "$out/windows.status")" = pass ]; then
    cp "$local_win/pkg/"* "$out/artifacts/" 2> /dev/null || true
fi

if [ $keep = 0 ]; then
    [ $need_ct = 1 ] && ssh -F "$sshcfg" -o BatchMode=yes divide \
        "pct exec 208 -- su -s /bin/bash knci -c 'rm -rf $ct_work /home/knci/kn-local/tmp/$s12 /home/knci/kn-local/cache/$s12'" 2> /dev/null || true
    [ $need_mac = 1 ] && ssh -o BatchMode=yes mac "rm -rf ~/$mac_work/src ~/$mac_work/build-macos ~/$mac_work/t" 2> /dev/null || true
    rm -rf "$local_win/src" "$local_win/t"
fi
rm -f "$out"/.*.sh

{
    printf 'local CI %s (%s)\n' "$s12" "$(git -C "$repo" log -1 --format=%s "$sha" | cut -c1-80)"
    printf '%-12s %-5s %6s  %s\n' job result secs note
    for j in meta check linux web services windows macos macos-arm64 android ios; do
        [ -f "$out/$j.status" ] || continue
        read -r st secs note < "$out/$j.status" || true
        printf '%-12s %-5s %6s  %s\n' "$j" "$st" "$secs" "${note:-}"
        case "$st" in
            fail) rc=1 ;;
            # Missing host toolchains are expected; a job omitted by --only with
            # no earlier result is not an acceptable substitute for a pass.
            skip) if [[ "$note" == "not selected" ]]; then rc=1; fi ;;
        esac
    done
} | tee "$out/summary.txt"
if [ $rc = 0 ]; then
    echo "$sha" > "$out/PASS"
    echo "PASS: $sha" >&2
fi
exit $rc
