#!/usr/bin/env python3
"""Hosting a krunker.io community map from a krunker.io account (ui_handlers.c selectHostMap, host_flow_ui.c), end to end
against the krunker.io stubs of server/tests/krunker_account_e2e.py. Nothing leaves the machine; no kn_brain.

  python3 -I client/tests/kio_hostmap_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7's selectHostMap(name, modURL, id) downloads nothing: it keeps {id, name} and shows the Host window
(deob-10.0.7.js:225334-225347), and createPrivateRoom sends the chosen map by its name (customMap, :204711) in
CUSTOM_SERVER; the game server sends the map's data with the game's init. Before this, a krunker.io account's pick was
downloaded from kn_brain (GET /api/map-data?name=), which the gate refuses while krunker.io is the backend: the pick
failed with "Game not found.", and Start Game refused every community map ("Community maps are hosted on Box Office
servers ...").
The client starts with a stored krunker.io session, joins the stub's game, goes back to the menu and:
  pick   selectHostMap "StubWorld" "" 4242 "StubCreator": the Host window opens with the map in its banner, no download
  start  Start Game (#startServBtn, createPrivateRoom): the official socket's "custom" carries customMap "StubWorld"
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
WORK = tempfile.mkdtemp(prefix="kn-kio-hostmap-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
MAP = "StubWorld"
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


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


def script(shots):
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "13000 respawn",
         "14400 down ESC", "14450 up ESC",
         '15000 call selectHostMap "%s" "" 4242 "StubCreator"' % MAP,
         "15010 waitfor [id=startServBtn]",
         "15500 text @banner [id=commMap]",
         "15510 text @msg0 [id=hostGameMsg]",
         "15520 shot " + shots["host"],
         "16000 clicksel [id=startServBtn]",
         "18000 text @msg1 [id=hostGameMsg]",
         "18010 shot " + shots["start"],
         "18500 quit"]
    return "\n".join(s) + "\n"


def run_client(stub, shots):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script(shots))
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (as kio_market_e2e.py): signed in at start, refreshed against the stub's FRVR auth
    for key, value in (("kio_refresh_token", kae.REFRESH1), ("kio_username", kae.ACCOUNT_NAME)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
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


def checks(text, stub, customs, shots):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    check("selectHostMap keeps the krunker.io map without a download (v10.0.7's {id, name})",
          ("selectHostMap: %s (krunker.io) -> host flow" % MAP) in text and "/api/map-data" not in text
          and "[community] %s" % MAP not in text, re.findall(r".*(?:selectHostMap|map-data|\[community\]).*", text)[:4])
    check("the Host window's card names the map (#commMap, deob-10.0.7.js:219949)", text_of(text, "banner") == MAP, text_of(text, "banner"))
    config = customs[0][1] if customs and len(customs[0]) > 1 and isinstance(customs[0][1], dict) else {}
    check("Start Game sends CUSTOM_SERVER (\"custom\") on the official socket with customMap = the map's name",
          len(customs) == 1 and config.get("customMap") == MAP, [c[:2] for c in customs][:2])
    check("no Box Office refusal of the community map", "Community maps are hosted" not in text_of(text, "msg1"), text_of(text, "msg1"))
    check("the screenshots were taken", all(os.path.exists(p) for p in shots.values()), shots)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_hostmap_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.players = []
    customs = []

    async def on_custom(send, raw):  # the client's CUSTOM_SERVER: [name, config, rid, rehost]
        try:
            customs.append(kae.frame_args(raw))
        except Exception as e:  # noqa: BLE001 (a frame the decoder cannot read is a failure of the check)
            customs.append(["custom", "undecodable: %s" % e])

    stub.frame_handlers["custom"] = on_custom
    shots = {k: os.path.join(WORK, k + ".png") for k in ("host", "start")}
    try:
        stub.start()
        text = run_client(stub, shots)
        checks(text, stub, customs, shots)
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
