#!/usr/bin/env python3
"""openSales opens v10.0.7's item page (deob-10.0.7.js:223623-223650), not a sales URL.

  python3 -I client/tests/open_sales_e2e.py BUILD_DIR
"""
import os
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CLIENT = os.path.join(ROOT, BUILD, "client_bin")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-open-sales-", dir=CACHE if os.path.isdir(CACHE) else None)
# Fractured, a real store skin (kill_pools_server_e2e). The call has no element, so the instance is null.
SKIN = 7582


def main():
    if not os.path.isfile(CLIENT):
        print("open_sales_e2e: client_bin missing")
        return 1
    script = os.path.join(WORK, "open.script")
    with open(script, "w") as f:
        f.write("0 menu\n3000 call openSales %d\n3500 nodes @page .items-detail-name\n3600 text @name .items-detail-name\n4000 quit\n" % SKIN)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
        "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FINDER_URL": "off",
        "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
        "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
        "LIBGL_ALWAYS_SOFTWARE": "1",
    })
    assets = os.environ.get("KRUNKNATIVE_ASSETS_DIR")
    if assets:
        env["KRUNKNATIVE_ASSETS_DIR"] = assets
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            rc = p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            print("open_sales_e2e: timeout (log %s)" % log)
            return 1
    text = open(log, errors="replace").read()
    if rc != 0:
        print("open_sales_e2e: client rc=%s (log %s)" % (rc, log))
        return 1
    import re
    nodes = re.search(r"\[input_script\] nodes t=\d+ @page elements=\d+ match=(\d+)", text)
    name = re.search(r"\[input_script\] text t=\d+ @name n=\d+ checked=\d value=.*? text=(.*)", text)
    shown = name.group(1).strip() if name else ""
    if not nodes or int(nodes.group(1)) < 1 or "Fractured" not in shown:
        print("open_sales_e2e: FAIL the item page did not open (nodes %s, name %r)" % (nodes.group(0) if nodes else "none", shown))
        print("kept %s" % WORK)
        return 1
    print("open_sales_e2e: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
