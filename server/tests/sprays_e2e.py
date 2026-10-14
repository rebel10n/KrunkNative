#!/usr/bin/env python3
"""Sprays end to end: the chosen spray is the one everyone sees (client/src/ui/ui_loadout.c
ui_loadout_spray / Select Spray, shared/src/social_v7.c game_spray_pick, server/src/net.c "spry" and
GameHooks.can_use_skin, client/src/sprays.c).

  python3 -I server/tests/sprays_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch sqlite database (items are granted with SQL), one brain-linked server_bin node
on Sandstorm (skins enforced: linked to the coordinator), every port from kn_proto.free_ports().
Krunker v10.0.2 players.spray (v10:7939498): the spray is the player's sprayIndex (ENTER_GAME ent[1],
the saved "sprayIndex", which the client sends as it is), kept when it is a free SPRAY item or
canUseSkin says the account owns it, else store.skins[2482] "Krunker"; addSpray paints
textures/sprays/<store.skins[i].id>.png.

  1. protocol (v7 peers, plain "spry"): a guest's free spray (2535 Poisn) is kept, a guest's paid spray
     (867 Clickbait) becomes 2482; an account owning 867 sprays it, an account owning nothing sprays
     2482 for 867 and keeps a free one; the sprayer and a watching peer get the same "sp" skin
  2. client_bin: an account owning Clickbait picks it in Customize > Spray (Select Spray) and sprays at
     its feet with the spray key; a guest client_bin stands next to it. The account's ENT sends 867,
     both clients get "sp" 867 and paint textures/sprays/40.png ([sprays] log), and in both frames the
     decal matches Clickbait's texture better than the default's (template check against the frame
     before the spray)
  3. client_bin, a guest picks the free Poisn (2535) in Select Spray: ENT 2535, "sp" 2535, Poisn on
     screen; a guest whose saved sprayIndex is the paid 867 sends it (v10 sends the saved index) and
     the server sprays 2482: the default on screen (the frame check's negative control)

KN_SPRAYS_TRANSPORT=tcp|udp|... picks the client transport (default: the client's own).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 without client_bin or sqlite.
"""
import json
import os
import re
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
NAME = "kn_test_sprays_%d" % os.getpid()
SECRET = "kn-sprays-%d" % os.getpid()
(GAME_PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-sprays-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
SPOT = (0, 13, -3)  # Sandstorm's first spawn: open floor
DEFAULT, CLICKBAIT, POISN = 2482, 867, 2535  # store.skins indices (type 4 SPRAY); Poisn is free
SPRAY_V7 = 247  # NET_PACKET_BLOCK_SOCIAL + 7: C->S "spry" index, S->C "sp"
SPRAY_V7_FMT = "<iHi3fhhhi"  # NetSprayV7Packet (packed): uid, slot, skin, position, rotY, rotX, rotZ, dye
INPUT_BATCH, INPUT_FMT = 111, "<iifffBBBb"  # NET_PACKET_INPUT_BATCH: count + NetInputPacket (seq, moveDir, delta, xDir, yDir, ...)
results = []
procs = []

with open(os.path.join(ROOT, "assets", "skins.json")) as f:
    SKINS = json.load(f)


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(str(detail)) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def stop(p, secs=10):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


class Node:
    """The brain-linked server_bin (stdin: its console, for `bring`)."""

    def __init__(self, brain_port):
        self.log = os.path.join(WORK, "gs.log")
        gs = os.path.join(WORK, "gs.json")
        cfg = {"node_id": "SPR", "name": "sprays e2e", "region": "local", "listen_port": GAME_PORT,
               "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
               "brain_secret": SECRET, "modes": ["ffa"], "maps": ["Sandstorm"], "max_players": 8,
               "shutdown_grace": 0, "idle_kick_seconds": 0}
        with open(gs, "w") as f:
            json.dump({"gameserver": cfg}, f)
        self.p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                                  start_new_session=True)
        procs.append(self.p)
        self.linked = bool(wait_for(lambda: "welcomed" in read_log(self.log), 20))
        self.game = wait_for(lambda: next((g[0] for g in http(brain_port, "/mm/game-list")[1].get("games", [])
                                           if g[0].startswith("SPR:")), None), 15)

    def console(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()


# ------------------------------------------------------------------ accounts


def register(brain_port, name):
    st, reg = http(brain_port, "/auth/register", {"username": name, "password": "pw-sprays-%d" % os.getpid()})
    return (reg.get("token", ""), reg) if st == 200 and isinstance(reg, dict) else ("", reg)


def grant(db, name, skin):
    con = sqlite3.connect(db, timeout=30)
    try:
        (aid,) = con.execute("SELECT id FROM accounts WHERE lower(username) = lower(?)", (name,)).fetchone()
        con.execute("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (aid, skin))
        con.commit()
    finally:
        con.close()


# ------------------------------------------------------------------ 1. protocol peers


def sprays_of(conn, uid):
    """skins of the NetSprayV7Packets of `uid` read so far"""
    out = []
    for t, payload in conn.packets:
        if t == SPRAY_V7 and len(payload) == struct.calcsize(SPRAY_V7_FMT):
            v = struct.unpack(SPRAY_V7_FMT, payload)
            if v[0] == uid:
                out.append(v[2])
    return out


def peer(brain_port, spray, token=""):
    """A v7 peer (HELLO 1.3.1) through a seek ticket, ENT with sprayIndex `spray`; (conn, uid)"""
    q = "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=true" + ("&token=" + token if token else "")
    code, rep = http(brain_port, q)
    if code != 200:
        return None, None
    c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    c.send(kn_proto.HELLO, bytes([1, 3, 1]))
    c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""), session=token)
    c.ent(spray=spray, skins=(-1, -1, -1), lag_comp=1.0)
    end = time.time() + 10
    while time.time() < end:
        payload = c.read(until=kn_proto.SPAWN, timeout=max(0.1, end - time.time()))
        if payload is None:
            break
        sp = kn_proto.spawn_info(payload)
        if sp and sp["is_you"]:
            return c, sp["uid"]
    return c, None


def received(conn, uid):
    conn.read(timeout=0.2)
    return sprays_of(conn, uid)


def protocol_cases(brain_port, owner_token, poor_token):
    watcher, wuid = peer(brain_port, DEFAULT)
    check("1: a watching guest joins", wuid is not None)
    if wuid is None:
        return
    cases = [("a guest's free spray (Poisn 2535) is kept", POISN, "", POISN),
             ("a guest's paid spray (Clickbait 867) sprays 2482", CLICKBAIT, "", DEFAULT),
             ("an account owning Clickbait sprays it", CLICKBAIT, owner_token, CLICKBAIT),
             ("an account owning nothing: Clickbait sprays 2482", CLICKBAIT, poor_token, DEFAULT),
             ("an account's free spray (Poisn) is kept", POISN, poor_token, POISN)]
    for label, spray, token, want in cases:
        c, uid = peer(brain_port, spray, token)
        if uid is None:
            check("1: " + label + " (spawned)", False)
            if c:
                c.close()
            continue
        time.sleep(0.3)
        look = [struct.pack(INPUT_FMT, i, -1, 1 / 60, -1.5, 0.0, 0, 0, 0, 0) for i in range(3)]  # straight down
        c.send(INPUT_BATCH, bytes([len(look)]) + b"".join(look))
        c.send(SPRAY_V7, struct.pack("<i", -1))  # the spray key: "spry" without a wheel index
        mine = wait_for(lambda: received(c, uid), 5)
        seen = wait_for(lambda: received(watcher, uid), 5)
        got = (mine[-1] if mine else None, seen[-1] if seen else None)
        check("1: " + label, got == (want, want), "sprayer, watcher got %s, want %d" % (got, want))
        c.close()
    watcher.close()


# ------------------------------------------------------------------ client_bin


def client(name, brain_port, script, storage):
    path = os.path.join(WORK, name + ".script")
    lines = [line for line in script.splitlines() if line.strip()]
    with open(path, "w") as f:  # events in time order (stable)
        f.write("\n".join(sorted(lines, key=lambda line: int(line.split()[0]))) + "\n")
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # no first-person weapon: the frames before and after a spray differ by the decal alone
    hide = '{"showWeapon":false,"showWeaponSec":false,"showWeaponMel":false}'
    for key, value in dict({"knSettings": hide}, **storage).items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_PACKET_LOG": "1",
                "KRUNKNATIVE_UI_DIR": "ui/v10", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port})
    if os.environ.get("KN_SPRAYS_TRANSPORT"):
        env["KRUNKNATIVE_NET_TRANSPORT"] = os.environ["KN_SPRAYS_TRANSPORT"]
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def finish(p, secs):
    try:
        p.wait(secs)
        return True
    except subprocess.TimeoutExpired:
        stop(p)
        return False


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def ent_sprays(text):
    """spray_index of every ENT the client sent (KRUNKNATIVE_PACKET_LOG)"""
    return [int(m) for m in re.findall(r'=>\s+\{"type":"ent".*?"spray_index":(-?\d+)', ANSI.sub("", text))]


def got_sprays(text):
    """skin of every "sp" (spray_v7) the client received"""
    return [int(m) for m in re.findall(r'<=\s+\{"type":"spray_v7".*?"skin":(-?\d+)', ANSI.sub("", text))]


def painted(text):
    """(skin, texture id) of every decal the client placed ([sprays] lines of sprays.c)"""
    return [(int(a), int(b)) for a, b in re.findall(r"\[sprays\] decal \S+ skin=(-?\d+) texture=textures/sprays/(\d+)\.png", text)]


def playing(text):
    """a `state` line in the game: the client spawned"""
    return re.search(r"\[input_script\] state t=\d+ .*menu=0", text) is not None


def listed(text):
    """the PICK_SCRIPT's `nodes @card` found the spray's card"""
    return re.search(r"\[input_script\] nodes t=\d+ @card elements=\d+ match=1\b", text) is not None


def texture_of(skin):
    return SKINS[skin]["id"]


# ------------------------------------------------------------------ frame check


def changed_region(changed, cell=4):
    """The box of the largest connected changed area (cells of cell x cell pixels, two cells of
    slack): the decal, not another player moving in view."""
    import numpy as np
    h, w = changed.shape
    gh, gw = (h + cell - 1) // cell, (w + cell - 1) // cell
    pad = np.zeros((gh * cell, gw * cell), dtype=bool)
    pad[:h, :w] = changed
    counts = pad.reshape(gh, cell, gw, cell).sum(axis=(1, 3))
    label = np.zeros(counts.shape, dtype=np.int32)
    best, best_n, n = 0, 0, 0
    for sy, sx in zip(*np.nonzero(counts)):
        if label[sy, sx]:
            continue
        n += 1
        label[sy, sx] = n
        stack, total = [(sy, sx)], 0
        while stack:
            y, x = stack.pop()
            total += int(counts[y, x])
            for yy in range(max(0, y - 2), min(gh, y + 3)):
                for xx in range(max(0, x - 2), min(gw, x + 3)):
                    if counts[yy, xx] and not label[yy, xx]:
                        label[yy, xx] = n
                        stack.append((yy, xx))
        if total > best_n:
            best, best_n = n, total
    ys, xs = np.nonzero(np.kron(label == best, np.ones((cell, cell), dtype=bool))[:h, :w] & changed)
    return int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1


def decal_fit(before_path, after_path, skin):
    """How well store spray `skin` explains what changed between two frames of the same view (no
    first-person weapon): the texture's drawn part (each sprite-sheet frame, 4 rotations, mirrored
    or not) scaled onto the changed region and blended over `before` with its alpha * opacity and a
    fitted light level. Returns (mean abs error, changed fraction of the frame); error None
    without a change."""
    import numpy as np
    from PIL import Image
    try:
        before = np.asarray(Image.open(before_path).convert("RGB"), dtype=np.float32)
        after = np.asarray(Image.open(after_path).convert("RGB"), dtype=np.float32)
    except OSError:
        return None, 0.0
    changed = np.abs(after - before).sum(axis=2) > 40
    frac = float(changed.mean())
    if frac < 0.01:
        return None, frac
    y0, y1, x0, x1 = changed_region(changed)
    b, a = before[y0:y1, x0:x1], after[y0:y1, x0:x1]
    item = SKINS[skin]
    opacity = item.get("opacity") or 1.0
    sheet = Image.open(os.path.join(ROOT, "assets", "textures", "sprays", "%d.png" % item["id"])).convert("RGBA")
    frames = item.get("frames") or 1
    fw = sheet.size[0] // frames
    best = None
    for fr in range(frames):
        tile = sheet.crop((fr * fw, 0, (fr + 1) * fw, sheet.size[1]))
        drawn = tile.getchannel("A").point(lambda v: 255 if v > 25 else 0).getbbox()
        tile = tile.crop(drawn) if drawn else tile
        for mirror in (False, True):
            base = tile.transpose(Image.FLIP_LEFT_RIGHT) if mirror else tile
            for rot in (0, 90, 180, 270):
                t = np.asarray(base.rotate(rot, expand=True).resize((x1 - x0, y1 - y0), Image.BILINEAR), dtype=np.float32)
                alpha = t[:, :, 3:4] / 255.0 * opacity
                rgb = t[:, :, :3] * alpha
                rest = b * (1.0 - alpha)
                denom = float((rgb * rgb).sum())
                k = float((rgb * (a - rest)).sum()) / denom if denom > 0 else 0.0  # the light on the decal
                err = float(np.abs(rest + k * rgb - a).sum(axis=2).mean())
                best = err if best is None or err < best else best
    return best, frac


def check_frame(label, before, after, want):
    """the decal between two frames is `want`'s texture rather than the other candidate's"""
    other = DEFAULT if want != DEFAULT else CLICKBAIT
    e_want, frac = decal_fit(before, after, want) if before and after else (None, 0.0)
    e_other, _ = decal_fit(before, after, other) if before and after else (None, 0.0)
    ok = e_want is not None and e_other is not None and e_want < e_other * 0.6
    check(label, ok, "fit %s %s vs %s %s (changed %.1f%% of the frame)" % (
        want, "-" if e_want is None else "%.1f" % e_want, other, "-" if e_other is None else "%.1f" % e_other, frac * 100))


# ------------------------------------------------------------------ 2. / 3. clients

def shots(first, count, step, work, name):
    """`count` shots from t=first every `step` ms, each paired with a `vis @<name>_<n>` log line that
    orders it against the [sprays] decal line"""
    return "".join("%d shot %s/%s_%02d.png\n%d vis @%s_%02d :cursor\n" % (first + n * step, work, name, n, first + n * step, name, n)
                   for n in range(count))


# The watcher looks down all the time and takes a shot every second.
WATCHER_SCRIPT = "0 play\n500 state\n" + "".join("%d look 0 -1.6\n" % (t * 1000) for t in range(1, 45)) + "{shots}45000 quit\n"
# Customize (windows[2].tabIndex = 2, showWindow(3)) > Spray (showWindow(9): Select Spray), the
# spray's card is listed and its onclick (selectSpray) picks it, Play; then look down and spray with
# the spray key (F), shots after it. (The handler is called, not clicked: at the bottom of the list
# a card can sit under the window's edge.)
PICK_SCRIPT = """0 menu
500 clickid customizeButton
1500 frames {work}/{name}_customize 1
1600 call showWindow 9
3000 setscroll skinList 100000
3300 setscroll skinList 100000
3600 setscroll skinList 100000
3900 scroll @list skinList
4000 nodes @card .noBtnCard[data-index='{spray}']
4100 frames {work}/{name}_picker 1
4300 call selectSpray {spray}
5000 frames {work}/{name}_picked 1
5200 play
9000 state
11000 look 0 -1.6
11500 shot {work}/{name}_before.png
11700 down F
11800 up F
{shots}20000 quit
"""
SAVED_SCRIPT = """0 play
500 state
2500 look 0 -1.6
3000 shot {work}/{name}_before.png
3200 down F
3300 up F
{shots}12000 quit
"""


def shot_after_decal(text, name, skip=1):
    """the shot `skip` shots after the first one logged after the client placed the decal (the
    texture has drawn by then), else None"""
    decal = text.find("[sprays] decal")
    if decal < 0:
        return None
    later = [m.group(1) for m in re.finditer(r"\[input_script\] vis t=\d+ @(%s_\d+) " % re.escape(name), text) if m.start() > decal]
    return os.path.join(WORK, later[skip] + ".png") if len(later) > skip else None


def shot_before_decal(text, name):
    decal = text.find("[sprays] decal")
    before = [m.group(1) for m in re.finditer(r"\[input_script\] vis t=\d+ @(%s_\d+) " % re.escape(name), text)
              if decal < 0 or m.start() < decal]
    return os.path.join(WORK, before[-1] + ".png") if before else None


def account_case(brain_port, node, owner, owner_token):
    # The server's `bring` puts each client on the open floor (its POSITION + RECON also sets the
    # camera the scripts then turn down): the watcher while it is alone, the sprayer 4 units away.
    watcher, wlog = client("watcher", brain_port, WATCHER_SCRIPT.format(shots=shots(1500, 43, 1000, WORK, "watch")), {})
    check("2: the watching guest plays", wait_for(lambda: playing(read_log(wlog)), 60), read_log(wlog)[-300:])
    node.console("bring all %d %d %d" % SPOT)
    sprayer, slog = client("owner", brain_port, PICK_SCRIPT.format(work=WORK, name="owner", spray=CLICKBAIT,
                                                                    shots=shots(12500, 7, 1000, WORK, "owner_after")),
                           {"krunker_username": owner, "krunker_token": owner_token})
    check("2: the account plays", wait_for(lambda: playing(read_log(slog)), 60), read_log(slog)[-300:])
    node.console("bring %s %d %d %d" % (owner, SPOT[0] + 4, SPOT[1], SPOT[2]))
    finish(sprayer, 90)
    wait_for(lambda: got_sprays(read_log(wlog)), 5)
    time.sleep(4)  # the watcher's shots after the decal
    stop(watcher)
    stext, wtext = read_log(slog), read_log(wlog)
    saved = read_log(os.path.join(WORK, "owner-cfg", "krunknative", "sprayIndex.json")).strip()
    texture = texture_of(CLICKBAIT)
    check("2: Select Spray lists the owned Clickbait", listed(stext), stext[-200:])
    check("2: selecting it saves sprayIndex 867", saved == str(CLICKBAIT), repr(saved))
    check("2: the account's ENT sends 867", ent_sprays(stext)[-1:] == [CLICKBAIT], str(ent_sprays(stext)))
    check("2: the sprayer gets sp 867", got_sprays(stext) == [CLICKBAIT], str(got_sprays(stext)))
    check("2: the watcher gets sp 867", got_sprays(wtext) == [CLICKBAIT], str(got_sprays(wtext)))
    check("2: the sprayer paints textures/sprays/%d.png" % texture, painted(stext) == [(CLICKBAIT, texture)], str(painted(stext)))
    check("2: the watcher paints textures/sprays/%d.png" % texture, painted(wtext) == [(CLICKBAIT, texture)], str(painted(wtext)))
    check_frame("2: the sprayer's frame shows Clickbait", os.path.join(WORK, "owner_before.png"),
                shot_after_decal(stext, "owner_after"), CLICKBAIT)
    before, after = shot_before_decal(wtext, "watch"), shot_after_decal(wtext, "watch")
    check("2: watcher frames around the decal", before and after, "%s %s" % (before, after))
    if before and after:
        shutil.copy(before, os.path.join(WORK, "watcher_before.png"))
        shutil.copy(after, os.path.join(WORK, "watcher_after.png"))
        check_frame("2: the watcher's frame shows Clickbait", before, after, CLICKBAIT)


def guest_cases(brain_port, node):
    p, log = client("guest", brain_port, PICK_SCRIPT.format(work=WORK, name="guest", spray=POISN,
                                                            shots=shots(12500, 7, 1000, WORK, "guest_after")), {})
    if wait_for(lambda: playing(read_log(log)), 60):
        node.console("bring all %d %d %d" % SPOT)
    finish(p, 90)
    text = read_log(log)
    saved = read_log(os.path.join(WORK, "guest-cfg", "krunknative", "sprayIndex.json")).strip()
    check("3: a guest's Select Spray lists the free Poisn", listed(text), text[-200:])
    check("3: selecting it saves sprayIndex 2535", saved == str(POISN), repr(saved))
    check("3: the guest's ENT sends 2535", ent_sprays(text)[-1:] == [POISN], str(ent_sprays(text)))
    check("3: the guest gets sp 2535 and paints textures/sprays/%d.png" % texture_of(POISN),
          got_sprays(text) == [POISN] and painted(text) == [(POISN, texture_of(POISN))], "%s %s" % (got_sprays(text), painted(text)))
    check_frame("3: the guest's frame shows Poisn", os.path.join(WORK, "guest_before.png"), shot_after_decal(text, "guest_after"), POISN)

    p, log = client("stale", brain_port, SAVED_SCRIPT.format(work=WORK, name="stale", shots=shots(4000, 7, 1000, WORK, "stale_after")),
                    {"sprayIndex": str(CLICKBAIT)})
    if wait_for(lambda: playing(read_log(log)), 60):
        node.console("bring all %d %d %d" % SPOT)
    finish(p, 90)
    text = read_log(log)
    check("3: a guest with a saved paid spray sends it (v10: the saved index)", ent_sprays(text)[-1:] == [CLICKBAIT], str(ent_sprays(text)))
    check("3: the server sprays the default 2482 for it", got_sprays(text) == [DEFAULT] and painted(text) == [(DEFAULT, 0)],
          "%s %s" % (got_sprays(text), painted(text)))
    check_frame("3: the stale guest's frame shows the default", os.path.join(WORK, "stale_before.png"),
                shot_after_decal(text, "stale_after"), DEFAULT)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("sprays_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("sprays_e2e: sqlite scratch database unavailable: skipped")
        return 77
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=sprays-%d" % os.getpid()],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        stamp = os.getpid() % 100000
        owner, poor = "spo%d" % stamp, "spp%d" % stamp
        owner_token, reg = register(port, owner)
        check("account registered", owner_token, str(reg)[:200])
        poor_token, reg = register(port, poor)
        check("second account registered", poor_token, str(reg)[:200])
        grant(dbcfg["path"], owner, CLICKBAIT)
        node = Node(port)
        check("node linked, lobby listed", node.linked and node.game, read_log(node.log)[-300:])
        protocol_cases(port, owner_token, poor_token)
        account_case(port, node, owner, owner_token)
        guest_cases(port, node)
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", "sqlite", NAME], capture_output=True)
    if all(results) and not os.environ.get("KN_SPRAYS_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs and frames kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
