#!/usr/bin/env bash
# The packaged launcher reaches the updates server: unpacks a client package (BoxOffice-<platform>) and runs its launcher's
# update check (`--cli --check`) against the live channel manifest (default
# https://dl.krunk.io/updates/client/latest.json; KRUNKNATIVE_UPDATES_URL overrides it). That is
# the launcher's own DNS lookup, TLS (system CA store / WinHTTP's), redirects and manifest parse
# end to end, on the platform that built the package. 2026-10-13: "the box office updater fails to
# connect to the server" (assets.krunk.io had no DNS record after the move to Spaceship).
#
#   tools/release/check-launcher.sh <BoxOffice-<platform>.tar.gz|.zip> [--old-url]
#
# Exit 0 = the launcher read the manifest (its build is current or a newer one is published),
# 1 = it could not. --old-url also checks the URL launchers before 8a010d82 have built in
# (https://assets.krunk.io/updates/client/latest.json, now a 302 to dl.krunk.io).
# Windows packages run natively under MSYS2/Cygwin, else through wine (WINEPREFIX kept private).
# The package is flat (BoxOffice and runtime/ at the top), so the launcher runs from the unpacked folder.
set -euo pipefail

pkg="${1:?usage: check-launcher.sh <package.tar.gz|package.zip> [--old-url]}"
old_url=0
[ "${2:-}" = --old-url ] && old_url=1
pkg="$(cd "$(dirname "$pkg")" && pwd)/$(basename "$pkg")"
work="$(mktemp -d "${TMPDIR:-/tmp}/kn-launcher-check.XXXXXX")"
trap 'rm -rf "$work"' EXIT

case "$pkg" in
  *.zip) unzip -q "$pkg" -d "$work" ;;
  *.tar.gz) tar -xzf "$pkg" -C "$work" ;;
  *) echo "check-launcher: not a package: $pkg" >&2; exit 1 ;;
esac

[ -f "$work/runtime/kn-install.json" ] || { echo "check-launcher: $pkg is not a Krunker Box Office client package (no runtime/kn-install.json)" >&2; exit 1; }
run=()
if [ -d "$work/BoxOffice.app" ]; then
  run=("$work/BoxOffice.app/Contents/MacOS/BoxOffice")
elif [ -f "$work/BoxOffice.exe" ]; then
  case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) run=("$work/BoxOffice.exe") ;;
    *)
      command -v wine > /dev/null || { echo "check-launcher: a Windows package needs Windows or wine" >&2; exit 1; }
      export WINEPREFIX="${WINEPREFIX:-$work/wineprefix}" WINEDEBUG=-all
      run=(wine "$work/BoxOffice.exe")
      ;;
  esac
else
  run=("$work/BoxOffice")
fi

check() { # <label> [extra args]
  local label="$1" code=0
  shift
  local out="$work/check.log"
  env -u DISPLAY -u WAYLAND_DISPLAY XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config" \
    "${run[@]}" --cli --check "$@" > "$out" 2>&1 || code=$?
  sed 's/^/    /' "$out"
  # 0 = current, 3 = a newer build is published: both mean the manifest was read
  if [ "$code" = 0 ] || [ "$code" = 3 ]; then
    echo "check-launcher: $label: ok (exit $code)"
    return 0
  fi
  echo "check-launcher: $label: the launcher could not read the updates manifest (exit $code)" >&2
  return 1
}

rc=0
check "$(basename "$pkg")" || rc=1
if [ "$old_url" = 1 ]; then
  check "$(basename "$pkg") via the pre-8a010d82 URL" --updates-url https://assets.krunk.io/updates/client/latest.json || rc=1
fi
exit "$rc"
