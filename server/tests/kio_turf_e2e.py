#!/usr/bin/env python3
"""Turf Wars with krunker.io as the active account backend (client/src/ui/turf_ui.c), end to end.

  python3 -I server/tests/kio_turf_e2e.py BUILD_DIR      (run from the repository root)

While an official krunker.io game is joined, Turf Wars reads krunker.io's own Turf Wars routes (v10's gapi client,
v10:26670-26806: /territorial-wars/status, the leaderboards, the clan's stats, and /clans/by-name for the clan id that
v10's clanId is), never Box Office's brain. Before this, every one of those reads was a brain request that the brain
gate refused, so the Overview showed "Failed to load turf wars data".

The krunker.io stubs of krunker_account_e2e.py (its gapi answers in v10's shapes: TURF there) and one headless
client_bin (--offline, no brain: nothing of Box Office's is asked). The script:
  1. a guest in an official game: the Overview shows krunker.io's territory (KPD on Burg), no load failure
  2. the Leaderboards' clans board lists krunker.io's clan
  3. signed in to krunker.io with the clan KPD: My Clan asks by-name for the clan id and shows KPD's stats (not the
     no-clan page), and its stats are requested by that id
Exit 0 when every check passed, 77 when it cannot run.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-turf-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
FAILED_TEXT = "Failed to load turf wars data"
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


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


def script():
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         # 1. a guest in the official game: the Overview
         # (waitfor: the clock holds until the node is there, so the probe sees the answer, not the loading text)
         "7000 call openTurfWars",
         "7010 waitfor .territory-clan",
         "9000 text @tw1 [id=genericPop]",
         "9010 nodes @twlab .territory-clan",
         "9020 grep @twlab_kpd %s [id=genericPop]" % kae.TURF_CLAN,
         "9030 text @tw1txt [id=genericPop]",
         # 2. the Leaderboards' clans board
         "9500 call knTwTab 2",
         "9800 call knTwBoard 0",
         "9810 waitfor .clan-name-cell",
         "10300 grep @twlb_kpd %s [id=genericPop]" % kae.TURF_CLAN,
         "10310 text @twlbtxt [id=genericPop]",
         # 3. sign in to krunker.io (v10's login modal, as kio_store_e2e does)
         "11000 call loginOrRegister",
         "11500 clickid knV10LoginMode",
         "11700 clickid accName", "11750 type " + kae.LOGIN,
         "11900 clickid accPass", "11950 type " + kae.PASSWORD,
         "12100 clickid knV10LoginSubmit"]
    t = 16000
    for i, d in enumerate(kae.CODE):
        s += ["%d clickid knV10Code%d" % (t, i), "%d type %s" % (t + 60, d)]
        t += 150
    s += ["18500 call closWind",
          "19000 call openTurfWars",
          "19500 call knTwTab 1",
          "19510 waitfor .section-title",
          "22000 text @mc [id=genericPop]",
          "22010 nodes @mcnone .no-clan-container",
          "22030 grep @mc_kpd %s [id=genericPop]" % kae.TURF_CLAN,
          "22040 text @mctxt [id=genericPop]",
          "22500 quit"]
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
                # no brain: the official game must not need one (a refused port answers nothing)
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
    tw = [r for r in stub.requests if r[1].startswith("/api/territorial-wars/") or r[1].startswith("/api/clans/")]
    check("1. the Overview asks krunker.io's /territorial-wars/status", bool(stub.find("GET", "/api/territorial-wars/status")),
          [r[1] for r in tw][:6])
    guest = stub.find("GET", "/api/territorial-wars/status")
    check("1. ... as a guest (no Bearer: the account is not signed in yet)", bool(guest) and "authorization" not in guest[0][3],
          [r[3].get("authorization", "")[:10] for r in guest])
    check("1. the Overview shows krunker.io's clan on Burg", grep(text, "twlab_kpd") > 0 and nodes(text, "twlab") >= 1,
          (grep(text, "twlab_kpd"), nodes(text, "twlab")))
    check("1. ... and no load failure", FAILED_TEXT not in text_of(text, "tw1txt"), text_of(text, "tw1txt")[:160])
    check("2. the Leaderboards' clans board lists krunker.io's clan", grep(text, "twlb_kpd") > 0, "")
    check("2. ... with no load failure", FAILED_TEXT not in text_of(text, "twlbtxt"), text_of(text, "twlbtxt")[:160])
    check("3. My Clan: the account's clan is looked up by name on krunker.io (v10's by-name)",
          bool(stub.find("GET", "/api/clans/by-name/" + kae.TURF_CLAN)), [r[1] for r in tw][:8])
    by = stub.find("GET", "/api/clans/by-name/" + kae.TURF_CLAN)
    check("3. ... with the krunker.io account's Bearer", bool(by) and by[-1][3].get("authorization", "").startswith("Bearer "),
          [r[3].get("authorization", "")[:10] for r in by])
    check("3. ... and its stats are asked by the krunker.io clan id (9001), never by the tag",
          bool(stub.find("GET", "/api/territorial-wars/clan/%d" % kae.TURF_CLAN_ID))
          and not stub.find("GET", "/api/territorial-wars/clan/" + kae.TURF_CLAN), [r[1] for r in tw][:8])
    check("3. My Clan shows the clan, not the no-clan page", nodes(text, "mcnone") == 0 and grep(text, "mc_kpd") > 0,
          (nodes(text, "mcnone"), grep(text, "mc_kpd")))
    check("3. ... with no load failure", FAILED_TEXT not in text_of(text, "mctxt"), text_of(text, "mctxt")[:160])
    posts = [r for r in stub.requests if r[0] != "GET" and not r[1].startswith("/api/auth/") and not r[1].startswith("/frvr/")]
    check("nothing but the sign-in was POSTed to krunker.io", not posts, [(r[0], r[1]) for r in posts])


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_turf_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.account_data = [{"player_clan": kae.TURF_CLAN}]  # the krunker.io account's clan (v10's player_clan)
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
