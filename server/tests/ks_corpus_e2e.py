#!/usr/bin/env python3
"""KrunkScript compatibility corpus: real map scripts must check and run with no errors.

  python3 -I server/tests/ks_corpus_e2e.py BUILD_DIR [--no-client] [--only NAME,...] [--extra DIR ...]
      (run from the repository root; KN_KS_CORPUS=DIR adds a folder of map JSON files)

Corpus: every map under assets/maps/official/ and assets/maps/custom/ that carries a script, plus
any map JSON in --extra folders or KN_KS_CORPUS (community maps, fetched through KN_FETCH_PROXY by
the map tools into ~/.cache/krunknative/krunker-maps; a folder with no scripts adds nothing).
For each map and each side it has a script:
  1. `ksc check --json`: the script compiles (compiled-JS scripts go through the importer) with no
     error;
  2. a headless run: server maps on server_bin with 3 bots for 6 s, client maps on headless
     client_bin (offline, a timed input script with `play`) for 8 s; the log must have no
     "RUNTIME ERROR", no compile error ("Server script:" / "Client script:"), no "[KRUNKSCRIPT] error:",
     and the process must exit cleanly. The few runtime errors v10 raises too are listed in
     V10_SAME_ERRORS with the reason.
Prints PASS/FAIL lines; exit code 0 only when everything passed. A failing map keeps its log in
the work directory (printed).
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ARGS = sys.argv[1:]
BUILD = next((a for a in ARGS if not a.startswith("--")), "cmake-build-debug")
NO_CLIENT = "--no-client" in ARGS
ONLY = None
EXTRA = []
i = 0
while i < len(ARGS):
    if ARGS[i] == "--only" and i + 1 < len(ARGS):
        ONLY = set(ARGS[i + 1].split(","))
        i += 1
    elif ARGS[i] == "--extra" and i + 1 < len(ARGS):
        EXTRA.append(ARGS[i + 1])
        i += 1
    i += 1
if os.environ.get("KN_KS_CORPUS"):
    EXTRA.append(os.environ["KN_KS_CORPUS"])
WORK = tempfile.mkdtemp(prefix="kn-ks-corpus-")
results = []
ERROR_MARKERS = ("RUNTIME ERROR", "Server script:", "Client script:", "[KRUNKSCRIPT] error:", "compiled JavaScript scripts are not supported")
# Runtime errors that v10.0.2 raises too, for the same reason, so they are not ours to fix: the
# community map 114845 (compiled JS) reads GAME.PLAYERS.getSelf().active in its render hook, and v10's
# getSelf() is null until the local player exists (v10.js L183509), so the hook throws each frame
# before the spawn, in v10 and here alike ("Cannot read properties of null", the same text).
V10_SAME_ERRORS = {
    "custom_114845": ["RUNTIME ERROR: Cannot read properties of null (reading 'active')"],
}


def script_errors(name, log):
    """Marker hits in the log, minus the runtime errors v10 raises as well (V10_SAME_ERRORS)."""
    allowed = V10_SAME_ERRORS.get(name, [])
    hits = []
    for ln in log.splitlines():
        if any(m in ln for m in ERROR_MARKERS) and not any(a in ln for a in allowed):
            hits.append(ln.strip()[:120])
    return hits


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def decode(b64):
    import base64
    return base64.b64decode(b64).decode("utf-8", errors="replace")


def scripts_of(path):
    """(side, text) for every non-empty script of a map JSON (base64 or raw), or []."""
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return []
    sc = data.get("scripts") if isinstance(data, dict) else None
    if not isinstance(sc, dict):
        return []
    out = []
    for side in ("client", "server"):
        v = sc.get(side)
        if not isinstance(v, str) or not v.strip():
            continue
        try:
            text = decode(v)
        except Exception:
            text = v
        if text.strip():
            out.append((side, text))
    return out


def maps():
    found = []
    for pattern in ("assets/maps/official/*.json", "assets/maps/custom/*.json"):
        found += sorted(glob.glob(pattern))
    for d in EXTRA:
        found += sorted(glob.glob(os.path.join(os.path.expanduser(d), "*.json")))
    seen, out = set(), []
    for p in found:
        key = os.path.basename(p)
        if key in seen or not scripts_of(p):
            continue
        seen.add(key)
        if ONLY is None or os.path.splitext(key)[0] in ONLY or key in ONLY:
            out.append(p)
    return out


def ksc_check(path):
    p = subprocess.run([f"{BUILD}/ksc", "check", "--json", path], capture_output=True, text=True)
    try:
        doc = json.loads(p.stdout)
    except ValueError:
        return False, p.stdout + p.stderr
    bad = []
    for s in doc.get("scripts", []):
        if s.get("status") not in ("ok", "compiled_js", "missing"):
            bad.append(f"{s.get('side')}: {[e.get('message') for e in s.get('errors', [])]}")
    return not bad, "; ".join(bad)


def run_server(path, tag):
    cfg = os.path.join(WORK, f"{tag}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "KS corpus"\nport 0\nregion local\nmaxplayers 8\nmode ffa\nround_seconds 600\nbot_quota 3\n')
    none = os.path.join(WORK, "none.json")
    if not os.path.exists(none):
        with open(none, "w") as f:
            f.write("{}")
    log = os.path.join(WORK, f"{tag}-server.log")
    with open(log, "w") as out:
        proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", none, path], stdout=out,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        time.sleep(6.0)
        proc.terminate()
        time.sleep(0.5)
        proc.terminate()
        try:
            rc = proc.wait(20)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = -9
    return rc, open(log, errors="replace").read()


def run_client(path, tag):
    script = os.path.join(WORK, f"{tag}-input.txt")
    with open(script, "w") as f:
        f.write("0 play\n8000 quit\n")
    cfg = os.path.join(WORK, f"{tag}-cfg")
    os.makedirs(cfg, exist_ok=True)
    env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_AUDIO_BACKEND="null", KRUNKNATIVE_FIXED_DELTA="16",
               KRUNKNATIVE_INPUT_SCRIPT=script, XDG_CONFIG_HOME=cfg)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    if "KRUNKNATIVE_ASSETS_DIR" not in env:
        here = os.path.abspath("assets")
        env["KRUNKNATIVE_ASSETS_DIR"] = here if os.path.isdir(os.path.join(here, "css")) else os.path.join(
            os.path.dirname(os.path.abspath(subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True,
                                                            text=True).stdout.strip() or ".")), "assets")
    try:
        p = subprocess.run([f"{BUILD}/client_bin", "--offline", path], env=env, capture_output=True, text=True, timeout=240)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        return -9, (e.stdout or b"").decode(errors="replace") + (e.stderr or b"").decode(errors="replace")


def main():
    corpus = maps()
    check("corpus found", len(corpus) > 0, f"{len(corpus)} maps")
    for path in corpus:
        name = os.path.splitext(os.path.basename(path))[0]
        sides = [s for s, _ in scripts_of(path)]
        ok, detail = ksc_check(path)
        check(f"{name}: ksc check ({'+'.join(sides)})", ok, detail)
        if "server" in sides:
            rc, log = run_server(path, name + "-s")
            bad = script_errors(name, log)
            loaded = "Loading the server script" in log
            check(f"{name}: server run, no script errors", not bad and rc == 0 and loaded,
                  f"rc={rc} loaded={loaded} errors={bad[:3]} log={WORK}/{name}-s-server.log" if (bad or rc or not loaded) else "")
        if "client" in sides and not NO_CLIENT:
            rc, log = run_client(path, name + "-c")
            bad = script_errors(name, log)
            ran = "[KRUNKSCRIPT]" in log or "start" in log
            check(f"{name}: client run, no script errors", not bad and rc == 0 and ran,
                  f"rc={rc} ran={ran} errors={bad[:3]}" if (bad or rc or not ran) else "")
            if bad:
                with open(os.path.join(WORK, f"{name}-c.log"), "w") as f:
                    f.write(log)
    print(f"{sum(results)}/{len(results)} passed (work dir {WORK})")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
