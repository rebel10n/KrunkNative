#!/usr/bin/env bash
# Web session e2e: starts the web e2e stack (stack.sh: kn_brain, server_bin, net-bridge --web,
# headless Chromium) and runs the session tests against it. Needs the native build (client_bin is
# not used; server_bin and kn_brain are) and the web dist. `make web-session-e2e` runs it.
#
#   KN_WEB_BUILD=<build dir> KN_WEB_DIST=<dist/web> tools/kref/web-session/web-session-e2e.sh [test.js ...]
#
# Default tests: smoke-e2e.js. Screenshots and logs stay in the run dir (KN_TEST_KEEP=1 keeps it).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=$(mktemp -d /tmp/kn-web-session-XXXXXX)
tests=("$@"); [ ${#tests[@]} -gt 0 ] || tests=("$HERE/smoke-e2e.js")
"$HERE/stack.sh" "$RUN" > "$RUN/stack.out" 2>&1 &
stack=$!
cleanup() {
    kill "$stack" 2>/dev/null || true
    wait "$stack" 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[web-session-e2e] kept $RUN" || rm -rf "$RUN"
}
trap cleanup EXIT
for _ in $(seq 1 240); do [ -f "$RUN/env" ] && break; kill -0 "$stack" 2>/dev/null || { cat "$RUN/stack.out"; exit 1; }; sleep 0.5; done
[ -f "$RUN/env" ] || { echo "[web-session-e2e] the stack did not start"; cat "$RUN/stack.out"; exit 1; }
sleep 1
status=0
for t in "${tests[@]}"; do
    echo "[web-session-e2e] $(basename "$t")"
    KN_WEB_RUN="$RUN" node "$t" "$RUN/$(basename "$t" .js).png" || status=$?
done
[ $status = 0 ] && echo "[web-session-e2e] PASS" || { echo "[web-session-e2e] FAIL ($status)"; exit 1; }
