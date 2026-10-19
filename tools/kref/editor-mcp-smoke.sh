#!/bin/bash
# Editor bus + kn_editor_mcp smoke test, headless (no window on any display):
#   1. client_bin --editor --mcp-bus --mcp-auto-accept 311_kanji.json 311_burg.json   (EGL pbuffer)
#   2. kn_editor_mcp --live on that bus, driven over MCP JSON-RPC on stdio:
#      projects_list, objects_query in both maps, objects_copy_between_projects Kanji -> Burg,
#      undo, render_multiview from the live editor, map_save to a temp path, map_validate
#   3. the deny path: an editor with --mcp-auto-deny refuses the MCP (isError, "denied")
# Every step is asserted; exit 0 = pass. Usage:
#   tools/kref/editor-mcp-smoke.sh [build-dir]        (default cmake-build-debug)
#   KRUNKNATIVE_EGL_DEVICE=mesa|nvidia ... picks the GPU (headless.h); KEEP=1 keeps the temp dir;
#   CLIENT=<client_bin> MCP=<kn_editor_mcp> use binaries from elsewhere
set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$(pwd)
BUILD=${1:-${BUILD:-cmake-build-debug}}
CLIENT=${CLIENT:-$BUILD/client_bin}                  # CLIENT= / MCP= override one binary
MCP=${MCP:-$BUILD/tools/editor-mcp/kn_editor_mcp}
for bin in "$CLIENT" "$MCP"; do
    [ -x "$bin" ] || { echo "FAIL: $bin not built (make, or pass the build dir)"; exit 2; }
done

TMP=$(mktemp -d /tmp/kn-editor-mcp-smoke.XXXXXX)
EDITOR_PID=
DENY_PID=
cleanup() {
    [ -n "$EDITOR_PID" ] && kill "$EDITOR_PID" 2>/dev/null
    [ -n "$DENY_PID" ] && kill "$DENY_PID" 2>/dev/null
    wait 2>/dev/null
    if [ "${KEEP:-0}" = 1 ]; then echo "kept $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT

HEADLESS=(env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 XDG_CONFIG_HOME="$TMP/cfg" XDG_DATA_HOME="$TMP/data")
mkdir -p "$TMP/cfg" "$TMP/data" "$TMP/ws" "$TMP/bus" "$TMP/bus-deny"

wait_bus() { # log file, seconds
    for _ in $(seq 1 $(($2 * 10))); do
        grep -q "\[bus\] hosting the editor bus" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

echo "== editor: --editor --mcp-bus --mcp-auto-accept 311_kanji.json 311_burg.json (headless)"
"${HEADLESS[@]}" "$CLIENT" --editor --mcp-bus --bus-dir "$TMP/bus" --mcp-auto-accept \
    assets/maps/311_kanji.json assets/maps/311_burg.json > "$TMP/editor.log" 2>&1 &
EDITOR_PID=$!
if ! wait_bus "$TMP/editor.log" 60; then
    echo "FAIL: the editor did not start the bus"; tail -20 "$TMP/editor.log"; exit 1
fi
grep -E "\[editor\] project|\[bus\]" "$TMP/editor.log"
[ "$(stat -c %a "$TMP/bus/editor-bus.sock")" = 600 ] || { echo "FAIL: socket mode is not 0600"; exit 1; }
[ "$(stat -c %a "$TMP/bus/editor-bus.token")" = 600 ] || { echo "FAIL: token mode is not 0600"; exit 1; }

cat > "$TMP/drive.py" <<'PY'
# MCP JSON-RPC (newline-delimited, stdio) driver: every step asserts
import base64, json, os, subprocess, sys

mcp, bus, repo, tmp = sys.argv[1:5]
p = subprocess.Popen([mcp, "--live", "--bus-dir", bus, "--root", repo, "--workspace", os.path.join(tmp, "ws"), "--allow", tmp, "--quiet"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
seq = 0
failures = 0

def rpc(method, params=None, notify=False):
    global seq
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None: msg["params"] = params
    if not notify:
        seq += 1
        msg["id"] = seq
    p.stdin.write(json.dumps(msg) + "\n")
    p.stdin.flush()
    if notify: return None
    while True:
        line = p.stdout.readline()
        if not line: raise SystemExit("FAIL: kn_editor_mcp closed stdout")
        m = json.loads(line)
        if m.get("id") == seq: return m

def tool(name, args):
    r = rpc("tools/call", {"name": name, "arguments": args})
    if "error" in r: raise SystemExit("FAIL: %s: JSON-RPC error %s" % (name, r["error"]))
    return r["result"]

def check(cond, what, extra=""):
    global failures
    print(("ok   " if cond else "FAIL ") + what + (("  " + extra) if extra and not cond else ""))
    if not cond: failures += 1
    return cond

def sc(res):
    if res.get("structuredContent") is not None: return res["structuredContent"]
    for c in res.get("content", []):
        if c.get("type") == "text":
            try: return json.loads(c["text"])
            except ValueError: pass
    return {}

init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "editor-mcp-smoke", "version": "1"}})
check("result" in init, "initialize", json.dumps(init)[:300])
rpc("notifications/initialized", {}, notify=True)

res = tool("projects_list", {})
projects = sc(res).get("projects", [])
names = {pr.get("name", "").lower(): pr for pr in projects if pr.get("backend") == "editor"}
check(not res.get("isError") and "kanji" in names and "burg" in names, "projects_list: kanji and burg from the live editor", json.dumps(sc(res))[:400])

res = tool("objects_query", {"project": "kanji", "prefab": "CUBE", "limit": 1})
objs = sc(res).get("objects", [])
check(not res.get("isError") and len(objs) == 1, "objects_query kanji CUBE", json.dumps(sc(res))[:300])
src_id = objs[0]["id"] if objs else None

res = tool("objects_query", {"project": "burg", "limit": 1})
burg_total = sc(res).get("total")
check(not res.get("isError") and isinstance(burg_total, int) and burg_total > 100, "objects_query burg (total %s)" % burg_total, json.dumps(sc(res))[:300])

res = tool("objects_copy_between_projects", {"from": "kanji", "to": "burg", "ids": [src_id], "offset": [0, 20, 0]})
created = sc(res).get("created", [])
check(not res.get("isError") and len(created) == 1, "copy object %s kanji -> burg (created %s)" % (src_id, created), json.dumps(res)[:400])
total = sc(tool("objects_query", {"project": "burg", "limit": 1})).get("total")
check(total == burg_total + 1, "burg has one more object (%s -> %s)" % (burg_total, total))
got = sc(tool("object_get", {"project": "burg", "id": created[0]})) if created else {}
src = sc(tool("object_get", {"project": "kanji", "id": src_id}))
def pos(o):
    o = o.get("object", o)
    return o.get("p") or o.get("raw", {}).get("p") or o.get("data", {}).get("p")
ps, pd = pos(src), pos(got)
check(ps and pd and abs(pd[1] - ps[1] - 20) < 0.01 and abs(pd[0] - ps[0]) < 0.01 and abs(pd[2] - ps[2]) < 0.01,
      "the copy keeps its transform + offset (%s -> %s)" % (ps, pd))

res = tool("undo", {"project": "burg"})
total = sc(tool("objects_query", {"project": "burg", "limit": 1})).get("total")
check(not res.get("isError") and total == burg_total, "undo removes the whole copy (one step): %s objects" % total, json.dumps(res)[:300])

res = tool("render_multiview", {"project": "burg"})
images = [c for c in res.get("content", []) if c.get("type") == "image"]
ok = not res.get("isError") and len(images) == 4
for i, img in enumerate(images):
    data = base64.b64decode(img.get("data", ""))
    ok = ok and data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) > 2000
    open(os.path.join(tmp, "multiview_%d.png" % i), "wb").write(data)
check(ok, "render_multiview burg: 4 PNG views from the live editor (%s)" % ", ".join(str(len(base64.b64decode(i.get("data", "")))) + " B" for i in images),
      json.dumps(res)[:300])

out = os.path.join(tmp, "burg_out.json")
res = tool("map_save", {"project": "burg", "path": out})
saved = None
try: saved = json.load(open(out))
except (OSError, ValueError): pass
check(not res.get("isError") and saved is not None and len(saved.get("objects", [])) > 100 and len(saved.get("spawns", [])) > 0,
      "map_save burg -> %s (%s objects, %s spawns)" % (out, len(saved.get("objects", [])) if saved else "-", len(saved.get("spawns", [])) if saved else "-"),
      json.dumps(res)[:300])

res = tool("map_validate", {"project": "burg"})
v = sc(res)
check(not res.get("isError") and v.get("ok") is True, "map_validate burg (errors %s, warnings %s)" % (v.get("errors"), v.get("warnings")), json.dumps(v)[:400])

p.stdin.close()
p.wait(timeout=10)
sys.exit(1 if failures else 0)
PY

echo "== kn_editor_mcp --live (MCP JSON-RPC on stdio)"
python3 -I "$TMP/drive.py" "$MCP" "$TMP/bus" "$REPO" "$TMP"
status=$?
grep -q "permission auto for kn_editor_mcp\|permission auto for" "$TMP/editor.log" && echo "ok   the editor logged the auto-accepted permission" \
    || { echo "FAIL the editor did not log the auto-accept"; status=1; }

echo "== deny path: --mcp-auto-deny"
"${HEADLESS[@]}" "$CLIENT" --editor --mcp-bus --bus-dir "$TMP/bus-deny" --mcp-auto-deny assets/maps/311_burg.json > "$TMP/editor-deny.log" 2>&1 &
DENY_PID=$!
if ! wait_bus "$TMP/editor-deny.log" 60; then
    echo "FAIL: the deny editor did not start the bus"; status=1
else
    out=$("$MCP" --live --bus-dir "$TMP/bus-deny" --root "$REPO" --workspace "$TMP/ws" --quiet --call projects_list '{}' 2>&1)
    rc=$?
    if [ $rc -ne 0 ] && echo "$out" | grep -qi "denied"; then
        echo "ok   the MCP is denied (exit $rc): $(echo "$out" | grep -io '[^"]*denied[^"]*' | head -1)"
    else
        echo "FAIL the deny editor let the MCP in (exit $rc): $(echo "$out" | head -c 400)"; status=1
    fi
    grep -q "permission deny" "$TMP/editor-deny.log" && echo "ok   the editor logged the denial" || { echo "FAIL no denial in the editor log"; status=1; }
fi

[ $status -eq 0 ] && echo "PASS editor-mcp smoke" || { echo "FAILED editor-mcp smoke (logs: KEEP=1 $0)"; }
exit $status
