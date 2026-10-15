#!/usr/bin/env python3
"""The prize wheels end to end: kn_brain on a scratch sqlite database and a headless client_bin
(EGL, no window) signed in through it, driven with KRUNKNATIVE_UI_CMDS (debug_capture.c):
a Starter 10x spin (ten pointers, the highest rarity revealed first, Next Item), the Spin History
popup, a Season 7 Starter ticket redeemed, the Prize Wheels tab and the Free KR wheel.

  python3 -I tools/kref/spin_e2e.py <build dir> [out dir]      (from the repository root)

Screenshots (1-wheel10 ... 9-freekr-won.png) and both logs go to the out dir (default
~/.cache/kn-spin-e2e). Exit 77 when python websockets or the binaries are missing.
"""
import asyncio
import json
import os
import re
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
OUT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.cache/kn-spin-e2e"))
os.makedirs(OUT, exist_ok=True)
FAILS = []


def check(cond, what):
    print("%s  %s" % ("ok  " if cond else "FAIL", what), flush=True)
    if not cond:
        FAILS.append(what)


def wait_for(path, pattern, secs=20.0):
    end = time.time() + secs
    while time.time() < end:
        with open(path, errors="replace") as f:
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.2)
    return None


async def ws_call(port, frames):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        out = []
        for fr, label in frames:
            await ws.send(json.dumps(fr))
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), 10))
                if f[0] == label:
                    out.append(f)
                    break
        return out


async def internal(port, secret, op, **args):
    async with websockets.connect("ws://127.0.0.1:%d/mm/internal/ws" % port, additional_headers={"Authorization": "Bearer " + secret}) as ws:
        await ws.send(json.dumps({"type": "hello", "nodeId": "spin-e2e", "region": "local", "publicHost": "127.0.0.1",
                                  "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(ws.recv(), 10)).get("type") != "welcome":
            pass
        await ws.send(json.dumps({"type": "acct", "rid": 1, "op": op, "args": args}))
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), 15))
            if msg.get("type") == "acct-r" and msg.get("rid") == 1:
                return msg.get("err"), msg.get("data")


def main():
    work = tempfile.mkdtemp(prefix="kn-spin-e2e-", dir=os.path.expanduser("~/.cache"))
    secret = "e2e-%d" % os.getpid()
    cfg = {"database": {"driver": "sqlite", "path": os.path.join(work, "brain.db"),
                        "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
           "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": secret, "federation": False},
           "accounts": {"password_iterations": 10000, "starting_kr": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}
    with open(os.path.join(work, "config.json"), "w") as f:
        json.dump(cfg, f)
    blog = os.path.join(OUT, "brain.log")
    brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--config", os.path.join(work, "config.json"), "--migrate"],
                             cwd=ROOT, stdout=open(blog, "w"), stderr=subprocess.STDOUT)
    client = None
    try:
        m = wait_for(blog, r"listening on http://127\.0\.0\.1:(\d+) \(ws")
        port = int(m.group(1))
        name = "spin%d" % (int(time.time()) % 100000)
        r = asyncio.run(ws_call(port, [(["a", 0, [name, "spinpass1", None], None], "a")]))[0]
        acc_id, token = r[2], r[5]
        err, _ = asyncio.run(internal(port, secret, "give_skin", id=acc_id, skin=6423, source="test"))
        check(err is None, "a Season 7 Starter ticket given")
        xdg = os.path.join(work, "cfg")
        os.makedirs(os.path.join(xdg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", name), ("krunker_token", token), ("krunker_id", str(acc_id))):
            with open(os.path.join(xdg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        cmds = os.path.join(work, "cmds.txt")
        open(cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": xdg, "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                    "KRUNKNATIVE_UI_CMDS": cmds})
        clog = os.path.join(OUT, "client.log")
        client = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--offline"], cwd=ROOT, env=env,
                                  stdout=open(clog, "w"), stderr=subprocess.STDOUT)

        def cmd(line, wait=0.5):
            with open(cmds, "a") as f:
                f.write(line + "\n")
            time.sleep(wait)

        time.sleep(12)  # page, map, session restore
        cmd("ui prizeWheel(0, null, 10)", 2)
        cmd("shot %s/1-wheel10.png" % OUT, 1)
        cmd("clickid spinButton", 3)
        cmd("shot %s/2-spinning.png" % OUT, 1)
        time.sleep(14)
        cmd("shot %s/3-revealed.png" % OUT, 1)
        cmd("clickid spinNextBtn", 2)
        cmd("shot %s/4-next.png" % OUT, 1)
        cmd("ui spinHistoryPopup()", 3)
        cmd("shot %s/5-history.png" % OUT, 1)
        cmd("ui clearPops()", 1)
        cmd("ui prizeWheel(null, 6423)", 2)
        cmd("clickid spinButton", 12)
        cmd("shot %s/6-ticket.png" % OUT, 1)
        cmd("ui windows[13].switchTab(1)", 1)
        cmd("clickid spinUI", 2)
        cmd("ui showWindow(14)", 1)
        cmd("ui windows[13].switchTab(1)", 2)
        cmd("shot %s/7-wheels-tab.png" % OUT, 1)
        cmd("ui prizeWheel(5)", 2)  # v10 store.wheels: 5 is Free KR (4 is Xmas Comm.)
        cmd("shot %s/8-freekr.png" % OUT, 1)
        cmd("clickid spinButton", 12)
        cmd("shot %s/9-freekr-won.png" % OUT, 1)
        cmd("quit", 3)
        client.wait(timeout=30)
        hist = asyncio.run(ws_call(port, [(["a", 1, [name, None, token], None], "a"), (["spinH", 0], "spinH")]))[1][1] or []
        check(len(hist) == 11 and sum(x["fds"] for x in hist) == 500, "brain: 10x Starter (500 KR) + the ticket's item: %d rows" % len(hist))
        log = open(clog, errors="replace").read()
        check(log.count("[store] unboxing") == 3, "client: three items shown (10x first, Next Item, the ticket's)")
        shown = re.findall(r"\[store\] won item shown as (its 3D model|its preview image)", log)
        check(len(shown) == 4 and shown.count("its 3D model") >= 2 and shown[-1] == "its 3D model",
              "client: the won items and the Free KR coin drawn in #spinItemCanvas, the coin and the items' models in 3D (%r)" % shown)
        check("[store] wheel stopped" in log and "spinHistoryPopup/0 -> 1" in log, "client: the wheel stopped, Spin History opened")
        funds = asyncio.run(ws_call(port, [(["a", 1, [name, None, token], None], "a")]))[0][4][5]
        check(funds in (505, 510, 520, 1000), "Free KR wheel paid out (funds %r)" % funds)
        for shot in ("3-revealed", "5-history", "6-ticket", "9-freekr-won"):
            check(os.path.getsize(os.path.join(OUT, shot + ".png")) > 10000, shot + ".png written")
    finally:
        if client and client.poll() is None:
            client.kill()
        brain.terminate()
        brain.wait(timeout=10)


if __name__ == "__main__":
    if not os.path.exists(os.path.join(BUILD, "client_bin")) or not os.path.exists(os.path.join(BUILD, "services/brain/kn_brain")):
        print("spin_e2e: client_bin or kn_brain not built: skipped")
        sys.exit(77)
    main()
    print("spin_e2e: %s (screenshots in %s)" % ("FAILED: " + "; ".join(FAILS) if FAILS else "ok", OUT))
    sys.exit(1 if FAILS else 0)
