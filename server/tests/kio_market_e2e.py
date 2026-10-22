#!/usr/bin/env python3
"""The Market with krunker.io as the active account backend (client/src/ui/v10_market.c), end to end.

  python3 -I server/tests/kio_market_e2e.py BUILD_DIR      (run from the repository root)

While an official krunker.io game is joined and the krunker.io account is signed in, the Market's Browse grid lists
krunker.io's listings (v10's market.getListings: gapi GET /market/listings) and its Stats tab shows krunker.io's stats
(v10's market.getStats: GET /market/stats). Before this, both were economy-socket frames that the held socket never
answered, so the Browse grid said "No listings available" and the Stats tab stayed empty.

The krunker.io stubs of krunker_account_e2e.py (GAPI_READS there: the listing and the stats in v10's shapes) and one
headless client_bin (--offline, no brain). The script signs in to krunker.io (v10's login modal, as kio_turf_e2e.py
does), opens the Market and checks the Browse grid and the Stats tab.
Exit 0 when every check passed, 77 when it cannot run.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-market-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
NO_LISTINGS = "No listings available"
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    detail = str(detail)
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail[:400]) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def grep(text, label):
    m = re.search(r"\[input_script\] grep t=\d+ %s needle=\S+ n=\d+ hits=(\d+)" % re.escape("@" + label), text)
    return int(m.group(1)) if m else -1


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def script():
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "7000 call loginOrRegister",
         "7500 clickid knV10LoginMode",
         "7700 clickid accName", "7750 type " + kae.LOGIN,
         "7900 clickid accPass", "7950 type " + kae.PASSWORD,
         "8100 clickid knV10LoginSubmit"]
    t = 12000
    for i, d in enumerate(kae.CODE):
        s += ["%d clickid knV10Code%d" % (t, i), "%d type %s" % (t + 60, d)]
        t += 150
    s += ["14500 call closWind",
          # Browse: the listing's card (waitfor: the clock holds until the grid has a card, or times out)
          "15000 call openExchange 0",
          "15010 waitfor .item-wrapper",
          "16000 nodes @mkcards .item-wrapper",
          "16010 grep @mkprice 1,234 [id=genericPop]",
          "16020 grep @mknone %s [id=genericPop]" % NO_LISTINGS,
          # Stats: the Stats tab (v10mk.mtab 4) shows the stats row's card
          "17000 call v10mk.mtab 4",
          "17010 waitfor .stat-card",
          "18000 nodes @mkstat .stat-card",
          "18010 grep @mkstat_circ Circulation [id=genericPop]",
          "18500 quit"]
    return "\n".join(s) + "\n"


def run_client(stub):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH})
    p = subprocess.Popen(["xvfb-run", "-a", "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log)


def checks(text, stub):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    lst = stub.find("GET", "/api/market/listings")
    check("the Browse grid asks krunker.io's /market/listings with the account's Bearer", bool(lst) and lst[-1][3].get("authorization", "").startswith("Bearer "),
          [r[3].get("authorization", "")[:10] for r in lst])
    check("the Browse grid lists krunker.io's listing (one card)", nodes(text, "mkcards") == 1, nodes(text, "mkcards"))
    check("... at its price (1,234 KR)", grep(text, "mkprice") > 0, "")
    check("... and no 'No listings available'", grep(text, "mknone") == 0, "")
    st = stub.find("GET", "/api/market/stats")
    check("the Stats tab asks krunker.io's /market/stats", bool(st), [r[1] for r in stub.requests][-6:])
    check("the Stats tab shows krunker.io's stats row (one card, circulation)", nodes(text, "mkstat") == 1 and grep(text, "mkstat_circ") > 0,
          (nodes(text, "mkstat"), grep(text, "mkstat_circ")))
    posts = [r for r in stub.requests if r[0] != "GET" and not r[1].startswith("/api/auth/") and not r[1].startswith("/frvr/")]
    check("nothing but the sign-in was POSTed to krunker.io", not posts, [(r[0], r[1]) for r in posts])


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_market_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.account_data = [{"player_clan": kae.TURF_CLAN}]
    try:
        stub.start()
        text = run_client(stub)
        checks(text, stub)
    finally:
        stub.stop()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
