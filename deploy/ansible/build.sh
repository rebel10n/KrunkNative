#!/usr/bin/env bash
# Build the deployable service binaries (kn_brain, kn_migrate, server_bin) inside
# Debian containers, so they link against the target hosts' glibc/OpenSSL:
#   bookworm -> kru (Debian 12), amph.ashburn (12), com.angeles/com.chitown (13)
#   bullseye -> com.clara (Debian 11: glibc 2.31, OpenSSL 1.1)
# Output: dist/deploy/<codename>/bin/. Usage: deploy/ansible/build.sh [bookworm] [bullseye]
# Needs docker (or podman: DOCKER=podman). CI runs the same script.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
docker="${DOCKER:-docker}"
build_root="${KN_BUILD_ROOT:-${TMPDIR:-/tmp}/kn-deploy-build}"
# TARGETS="kn_brain kn_migrate" builds a subset (default: all three).
# KN_CMAKE_ARGS: extra -D flags for the configure step (CI: the build version from
# tools/release/version-meta.sh, -DKRUNKNATIVE_VERSION_*; the container has no git).
read -r -a targets <<< "${TARGETS:-kn_brain kn_migrate server_bin}"
# kn_dbcopy ships with kn_brain: the coordinator's SQLite -> PostgreSQL cutover (role kn_db_cutover)
case " ${targets[*]} " in *" kn_brain "*) case " ${targets[*]} " in *" kn_dbcopy "*) ;; *) targets+=(kn_dbcopy) ;; esac ;; esac
codenames=("$@")
[ ${#codenames[@]} -gt 0 ] || codenames=(bookworm bullseye)

for codename in "${codenames[@]}"; do
    case "$codename" in bookworm|bullseye|trixie) ;; *) echo "unknown codename $codename" >&2; exit 2 ;; esac
    out="$repo/dist/deploy/$codename"
    bdir="$build_root/$codename"
    mkdir -p "$out/bin" "$bdir"
    echo "[build] $codename -> $out/bin"
    # The repo is mounted read-only; only the build dir and the output are writable.
    # shellcheck disable=SC2016 # the script expands its variables inside the container
    "$docker" run --rm \
        -v "$repo:/src:ro" -v "$bdir:/build" -v "$out:/out" \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e TARGETS="${targets[*]}" \
        -e KN_CMAKE_ARGS="${KN_CMAKE_ARGS:-}" \
        "debian:$codename" bash -euo pipefail -c '
            export DEBIAN_FRONTEND=noninteractive
            # bullseye is EOL: the deb.debian.org security pocket 404s. Use archive.debian.org
            # (as com.clara does); security + updates too, since the image already has them.
            # deb.debian.org still carries bullseye main (11.11) as a second source: apt fetches a
            # package version from the next source when one 404s (archive.debian.org returned 404
            # for g++_10.2.1-1 for ~10 min on 2026-10-15, runs 38688156771 / 38688711980).
            if grep -q bullseye /etc/os-release; then
                rm -f /etc/apt/sources.list.d/*
                printf "%s\n" "deb http://archive.debian.org/debian bullseye main" \
                    "deb http://deb.debian.org/debian bullseye main" \
                    "deb http://archive.debian.org/debian bullseye-updates main" \
                    "deb http://archive.debian.org/debian-security bullseye-security main" > /etc/apt/sources.list
                echo "Acquire::Check-Valid-Until \"false\";" > /etc/apt/apt.conf.d/99archive
            fi
            echo "Acquire::Retries \"5\";" > /etc/apt/apt.conf.d/80retries
            # libsqlite3-dev / libmariadb-dev: store adapters are AUTO (built when found);
            # libhiredis-dev: the optional Redis hot tier of kn_brain (KNSVC_REDIS, doc/backend.md 3.11).
            # Up to three tries, a fresh index each: a mirror mid-sync 404s files its index still lists.
            for try in 1 2 3; do
                if apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends cmake ninja-build gcc g++ libc6-dev make \
                    pkg-config libssl-dev libpq-dev libsqlite3-dev libmariadb-dev libcrypt-dev libcurl4-openssl-dev \
                    libhiredis-dev python3 >/dev/null; then break; fi
                [ "$try" -lt 3 ] || exit 100
                echo "apt failed (try $try), retrying in 30 s" >&2
                sleep 30
            done
            cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
                -DKRUNKNATIVE_HTML_ENGINE=OFF -DGLFW_BUILD_X11=OFF -DGLFW_BUILD_WAYLAND=OFF $KN_CMAKE_ARGS >/build/configure.log
            cmake --build /build --target $TARGETS
            for t in $TARGETS; do
                f=$(find /build -type f -name "$t" -perm -u+x | head -1)
                [ -n "$f" ] || { echo "missing $t" >&2; exit 1; }
                install -m 0755 "$f" /out/bin/"$t"
            done
            strip --strip-unneeded /out/bin/*
            chown -R "$HOST_UID:$HOST_GID" /out /build'
    ls -la "$out/bin"
done
