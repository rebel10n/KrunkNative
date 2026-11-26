#!/usr/bin/env python3
"""A core shader that does not build ends on the Lost Connection box with the reason, not on a crash.

  python3 -I server/tests/shader_fail_e2e.py BUILD_DIR      (run from the repository root)

On 2026-11-26 basic.frag's Krunker variant had 17 samplers, WebGL2 on players' GPUs allows 16, the link failed and
basic_material_init returned NULL to callers that dereference it: a "null function" trap and a white screen after
Click to Play. Now the material keeps program 0, scene_render skips its draws and client_frame leaves the game with
NET_ERROR_CODE_GPU (client/src/materials/basic.c basic_material_failed, client/src/main.c).

A headless client_bin with KRUNKNATIVE_TEST_FAIL_BASIC_SHADER=1 (variant 0 refused, as that GPU did) joins a local
server_bin: it keeps running through the map load, logs "[render] ... could not build the game's shaders", shows
the Lost Connection box with that text, does not reconnect, and exits on `quit` with status 0. Needs a headless GL
device (KRUNKNATIVE_HEADLESS=1: EGL). Prints PASS/FAIL lines; exit 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-shader-fail-", dir=CACHE if os.path.isdir(CACHE) else None)
REASON = "could not build the game's shaders"
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


def main():
    port = free_ports(1)[0]
    server_log = os.path.join(WORK, "server.log")
    server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port)],
                              stdout=open(server_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                              start_new_session=True)
    procs.append(server)
    script = os.path.join(WORK, "client.script")
    with open(script, "w") as f:
        # `boot`, not `play`: play waits for the map to come up in the game, which a client that cannot draw never does
        f.write("0 boot\n12000 nodes @box .instructionsHeader\n12100 text @msg .instructionsHeader + div\n18000 quit\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_DEBUG": "2", "KRUNKNATIVE_NO_BROWSER": "1",
                "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_TEST_FAIL_BASIC_SHADER": "1"})
    client = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--connect", f"127.0.0.1:{port}"],
                              stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                              start_new_session=True)
    procs.append(client)
    try:
        code = client.wait(timeout=90)
    except subprocess.TimeoutExpired:
        os.killpg(client.pid, signal.SIGKILL)
        code = client.wait()
    text = read(log)
    check("the client reached the server", f"connected to server 127.0.0.1:{port}" in text, text[-400:])
    check("the forced failure ran (variant 0 refused)", "KRUNKNATIVE_TEST_FAIL_BASIC_SHADER" in text, text[-400:])
    check("the client logs the reason once", text.count(REASON) >= 1 and len([l for l in text.splitlines() if l.startswith("[render] ") and REASON in l]) == 1,
          [line for line in text.splitlines() if "[render]" in line][:3])
    box = [int(m.group(1)) for m in re.finditer(r"\[input_script\] nodes t=\d+ @box elements=\d+ match=(\d+)", text)]
    check("the Lost Connection box shows", box and box[0] >= 1, str(box))
    msg = re.findall(r"\[input_script\] text t=\d+ @msg n=\d+ checked=\d+ value=.*? text=(.*)", text)
    check("the box names the reason", any(REASON in m for m in msg), str(msg[:2]))
    check("no reconnect after it", text.count(f"connected to server 127.0.0.1:{port}") == 1,
          str(text.count(f"connected to server 127.0.0.1:{port}")))
    crash = [line for line in text.splitlines()
             if re.search(r"Segmentation|SIGSEGV|Aborted|AddressSanitizer|crash report|null function", line)]
    check("no crash", not crash, str(crash[:3]))
    check("the client exits on quit with status 0", code == 0, str(code))


try:
    main()
finally:
    for p in procs:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)

print(f"{sum(results)}/{len(results)} passed")
sys.exit(0 if results and all(results) else 1)
