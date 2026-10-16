#!/usr/bin/env python3
"""v10 Wild Offers end to end (client/src/ui/v10_wild_offer.c, services/brain/wild_offer.c): kn_brain on
a scratch sqlite database and a headless client_bin (EGL, no window) signed in through it, driven
with KRUNKNATIVE_UI_CMDS: the offer fetched after sign-in and shown once in the notification
center, the Store's Eterno Shop tab with the wild offer item, New Offer! (confirm, 300 KR, the next
offer), Buy (confirm, the price charged, the skin granted, the offer gone).

  python3 -I client/tests/wild_offer_e2e.py <build dir> [out dir]      (from the repository root)

Screenshots and both logs go to the out dir (default ~/.cache/kn-wild-offer-e2e). Exit 77 when
python websockets or the binaries are missing.
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
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
OUT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.cache/kn-wild-offer-e2e"))
os.makedirs(OUT, exist_ok=True)
FAILS = []
FUNDS = 50000


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


def main():
    work = tempfile.mkdtemp(prefix="kn-wild-offer-e2e-", dir=os.path.expanduser("~/.cache"))
    db = os.path.join(work, "brain.db")
    cfg = {"database": {"driver": "sqlite", "path": db, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
           "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "wo-e2e-%d" % os.getpid(), "federation": False},
           "accounts": {"password_iterations": 10000, "starting_kr": FUNDS, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}
    with open(os.path.join(work, "config.json"), "w") as f:
        json.dump(cfg, f)
    blog = os.path.join(OUT, "brain.log")
    brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--config", os.path.join(work, "config.json"), "--migrate"],
                             cwd=ROOT, stdout=open(blog, "w"), stderr=subprocess.STDOUT)
    client = None
    try:
        port = int(wait_for(blog, r"listening on http://127\.0\.0\.1:(\d+) \(ws").group(1))
        name = "wo%d" % (int(time.time()) % 100000)
        r = asyncio.run(ws_call(port, [(["a", 0, [name, "wildpass1", None], None], "a")]))[0]
        acc_id, token = r[2], r[5]
        offer = asyncio.run(ws_call(port, [(["a", 1, [name, None, token], None], "a"), (["wo"], "wo")]))[1][1]
        check(isinstance(offer, dict), "brain: the account has a wild offer (%r)" % (offer and offer["id"]))
        xdg = os.path.join(work, "cfg")
        os.makedirs(os.path.join(xdg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", name), ("krunker_token", token), ("krunker_id", str(acc_id))):
            with open(os.path.join(xdg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        cmds = os.path.join(work, "cmds.txt")
        open(cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": xdg, "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                    "KRUNKNATIVE_UI_CMDS": cmds, "KRUNKNATIVE_AUDIO_BACKEND": "null"})
        clog = os.path.join(OUT, "client.log")
        client = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--offline"], cwd=ROOT, env=env,
                                  stdout=open(clog, "w"), stderr=subprocess.STDOUT)

        def cmd(line, wait=0.5):
            with open(cmds, "a") as f:
                f.write(line + "\n")
            time.sleep(wait)

        check(wait_for(clog, r"\[wildoffer\] notified offer %d \(skin (\d+)\)" % offer["id"], 40) is not None,
              "client: the offer fetched after sign-in and shown in the notification center")
        time.sleep(1)  # slideIn 0.3 s
        cmd("shot %s/1-notification.png" % OUT, 1)
        cmd("ui knWoShop()", 2)  # View Shop: the Store's Eterno Shop tab
        cmd("shot %s/2-eterno.png" % OUT, 1)
        cmd("ui knWoView()", 1)
        cmd("shot %s/3-view.png" % OUT, 1)
        cmd("ui knViewPop(0)", 1)
        cmd("ui knWoRefresh()", 1)
        cmd("shot %s/4-refresh-confirm.png" % OUT, 1)
        cmd("ui knViewPop(1)", 3)
        m = wait_for(clog, r"\[wildoffer\] refreshed: offer (\d+)", 10)
        check(m is not None and int(m.group(1)) == offer["id"] + 1, "client: New Offer! confirmed, the next offer (%r)" % (m and m.group(1)))
        cmd("ui knViewPop(1)", 1)  # Awesome!
        cmd("shot %s/5-refreshed.png" % OUT, 1)
        cmd("ui knWoBuy()", 1)
        cmd("shot %s/6-buy-confirm.png" % OUT, 1)
        cmd("ui knViewPop(1)", 3)
        m = wait_for(clog, r"\[wildoffer\] bought offer (\d+): .* \(skin (\d+)\)", 10)
        check(m is not None and int(m.group(1)) == offer["id"] + 1, "client: Buy confirmed, the offer bought (%r)" % ((m and m.groups()),))
        cmd("shot %s/7-bought.png" % OUT, 1)
        check(wait_for(clog, r"\[wildoffer\] bought offer[^\n]*\n(?:.*\n)*?\[wildoffer\] offer 0", 10) is not None,
              "client: fetched again after the purchase: no offer")
        cmd("quit", 3)
        client.wait(timeout=30)
        con = sqlite3.connect(db)
        funds = con.execute("SELECT player_funds FROM players WHERE player_id = ?", (acc_id,)).fetchone()[0]
        skin = int(m.group(2)) if m else -1
        owned = con.execute("SELECT coalesce(sum(delta), 0) FROM skin_transactions WHERE account_id = ? AND skin_id = ? AND source = 'wildoffer'",
                            (acc_id, skin)).fetchone()[0]
        price = con.execute("SELECT -amount FROM kr_transactions WHERE account_id = ? AND source = 'wildoffer'", (acc_id,)).fetchone()
        con.close()
        check(owned == 1, "brain: the skin granted (%r)" % owned)
        check(price is not None and funds == FUNDS - 300 - price[0], "brain: 300 KR + the price charged (funds %r)" % funds)
        for shot in ("1-notification", "2-eterno", "3-view", "7-bought"):
            p = os.path.join(OUT, shot + ".png")
            check(os.path.exists(p) and os.path.getsize(p) > 10000, shot + ".png written")
    finally:
        if client and client.poll() is None:
            client.kill()
        brain.terminate()
        brain.wait(timeout=10)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    if not os.path.exists(os.path.join(BUILD, "client_bin")) or not os.path.exists(os.path.join(BUILD, "services/brain/kn_brain")):
        print("wild_offer_e2e: client_bin or kn_brain not built: skipped")
        sys.exit(77)
    main()
    print("wild_offer_e2e: %s (screenshots in %s)" % ("FAILED: " + "; ".join(FAILS) if FAILS else "ok", OUT))
    sys.exit(1 if FAILS else 0)
