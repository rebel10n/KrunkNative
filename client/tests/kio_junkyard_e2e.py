#!/usr/bin/env python3
"""The Junkyard on an official krunker.io game (client/src/ui/v10_junkyard.c), end to end against the krunker.io stubs of
server/tests/krunker_account_e2e.py. Nothing leaves the machine; no kn_brain.

  python3 -I client/tests/kio_junkyard_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7 asks the recipes and crafts on the game socket (windows[13]: CRAFT_DATA_REQUEST "recps" once the Junkyard
mounts, CRAFT_REQUEST ["crft", SID] from the confirmation's startCraft; deob-10.0.7.js:220747-220790), answered
"recps" (craftDatRes) and "crftr" (craftRes). Before this, a krunker.io account's Junkyard sent "recps" to the brain's
social socket, which the brain gate refuses while krunker.io is the backend: the tab read "No Recipes Found" and Craft
did nothing. The client starts with a stored krunker.io session, joins the stub's game and enters it ("en": the live
server refuses "recps" before it), then:
  list     openExchange(3): one "recps" on the game socket, after "en"; the stub's recipes are listed, the account's
           materials (its "a" skins) and junk (player_jnk) decide Craft / Missing materials
  craft    the card's Craft -> v10's confirmation ("Craft Krystallizer", Craft time "1h 0m") -> its Craft:
           ["crft", 1] on the game socket; the stub's ["crftr", 1, true] closes the popups (craftRes: clearPops)
  again    openExchange(3) again: no second "recps" (v10 keeps craftData), and the crafted card reads
           "<t> remaining" (craftRes: EXP = 1000 * DUR, STATORD = 1), its NEW tag and Craft button gone
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
WORK = tempfile.mkdtemp(prefix="kn-kio-junkyard-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
# skins.json store indices: 1 Wood, 3 Wires (materials); 3283 Krystallizer, 3921 Plasma Cutter (craftable)
WOOD, WIRES, KRYSTALLIZER, PLASMA = 1, 3, 3283, 3921
RECIPES = [  # krunker.io's rows (craftDatRes): no junk balance slot, the account's junk is player_jnk
    {"SID": 1, "SOUT": KRYSTALLIZER, "SIN": "%d,2,%d,1" % (WOOD, WIRES), "JUNK": 0, "DUR": 3600, "CNT": -1, "DAT": 1700000000000},
    {"SID": 2, "SOUT": PLASMA, "SIN": "%d,9" % WOOD, "JUNK": 0, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
]
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


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


CRAFTS = []


async def answer_recipes(send, msg):  # CRAFT_DATA_RESPONSE: craftDatRes(rows)
    await send("recps", RECIPES)


async def answer_craft(send, msg):  # CRAFT_RESPONSE: craftRes(SID, ok, msg)
    args = kae.frame_args(msg)
    CRAFTS.append(args[1:])
    await send("crftr", args[1] if len(args) > 1 else -1, True)


def script(shots):
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "13000 respawn",  # ENTER_GAME ("en"): the game is joined, the server answers recps from here on
         "14400 down ESC", "14450 up ESC",
         "15000 call openExchange 3",
         "15010 waitfor [id=v10jyCraft1]",
         "15500 text @grid [id=jyGrid]",
         "15510 nodes @craft1 [id=v10jyCraft1]",
         "15520 nodes @craft2 [id=v10jyCraft2]",
         "15530 shot " + shots["list"],
         "15600 clickid v10jyCraft1",
         "15610 waitfor [id=confirmBtn]",
         "16000 text @pop [id=genericPop]",
         "16010 shot " + shots["confirm"],
         "16100 clickid confirmBtn",
         "16110 waitfor ![id=confirmBtn]",
         "17000 nodes @popafter [id=confirmBtn]",
         "17500 call openExchange 3",
         "17510 waitfor [id=jyGrid]",
         "18000 text @grid2 [id=jyGrid]",
         "18010 nodes @craft1b [id=v10jyCraft1]",
         "18020 nodes @hammer .craft-hammer-animation",
         "18030 shot " + shots["again"],
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


def checks(text, stub, shots):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    frames = list(stub.ws_frames)
    asks = [i for i, f in enumerate(frames) if f == "recps"]
    enter = frames.index("en") if "en" in frames else -1
    check("the Junkyard asks the recipes on the game socket (\"recps\") once the game is entered",
          len(asks) >= 1 and enter >= 0 and asks[0] > enter, (enter, asks, frames[-12:]))
    grid = text_of(text, "grid")
    check("the stub's recipes are listed (Krystallizer, Plasma Cutter), not \"No Recipes Found\" / Loading",
          "Krystallizer" in grid and "Plasma Cutter" in grid and "No Recipes" not in grid and "Loading" not in grid, grid[:200])
    check("the account's materials decide: recipe 1 craftable (Craft), recipe 2 short of Wood (Missing materials)",
          nodes(text, "craft1") == 1 and nodes(text, "craft2") == 0 and "Missing materials" in grid,
          (nodes(text, "craft1"), nodes(text, "craft2")))
    pop = text_of(text, "pop")
    check("the card's Craft opens v10's confirmation (Craft Krystallizer, Craft time 1h 0m)",
          "Craft Krystallizer" in pop and "Craft time" in pop and "1h 0m" in pop, pop[:200])
    check("the confirmation's Craft sends [\"crft\", 1] on the game socket", CRAFTS == [[1]], CRAFTS)
    check("the craft's answer closes the popups (craftRes: clearPops)", nodes(text, "popafter") == 0, nodes(text, "popafter"))
    grid2 = text_of(text, "grid2")
    check("the Junkyard again: no second \"recps\" (v10 keeps craftData)", len(asks) == 1, asks)
    check("the crafted card reads \"<t> remaining\" (craftRes: EXP = 1000 * DUR, STATORD 1) with the hammer, no Craft button",
          " remaining" in grid2 and nodes(text, "hammer") == 1 and nodes(text, "craft1b") == 0,
          (grid2[:200], nodes(text, "hammer"), nodes(text, "craft1b")))
    check("nothing went to the brain (no brain gate refusal for the Junkyard)", "recps" not in "".join(re.findall(r".*brain.*refus.*", text)),
          re.findall(r".*refus.*recps.*", text)[:2])
    check("the screenshots were taken", all(os.path.exists(p) for p in shots.values()), shots)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_junkyard_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.players = []  # "en" spawns our player (the stub's opt-in)
    # the account's "a": 10 junk, 5 Wood and 1 Wires (recipe 1's materials, not recipe 2's 9 Wood)
    stub.account_data = [{"player_jnk": 10, "skins": [{"ind": WOOD, "cnt": 5}, {"ind": WIRES, "cnt": 1}]}] * 2
    stub.frame_handlers = {"recps": answer_recipes, "crft": answer_craft}
    shots = {k: os.path.join(WORK, k + ".png") for k in ("list", "confirm", "again")}
    try:
        stub.start()
        text = run_client(stub, shots)
        checks(text, stub, shots)
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
