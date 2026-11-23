#!/usr/bin/env python3
"""v10.0.7's Skin Manager end to end through the native client, headless (client/src/ui/v10_skin_manager.c,
services/brain/skins.c).

  python3 -I client/tests/v10_skin_manager_e2e.py <native build dir> <repo root>

kn_brain on sqlite + a signed-in headless client on the sandbox page (--offline --sandbox: connectToSandbox's menu),
driven by KRUNKNATIVE_UI_CMDS. The side nav's Skin Manager (shown only in the sandbox, $skinManagerEnabled) opens the
window; Add new skin creates "New Skin" at once (the editor shows its id), a rename and a rarity are saved. The
Appearance tab: the 3D preview's iframe, its Model / Textures / Sounds / Shader Studio sections; a texture, a sound
and a model go up through the brain's chunked skin upload (the files land in uploads.dir and in the client's own
user-assets), a refused file shows the drop zone's message, Shader Studio's Refresh Preview writes a graph
customShader, a dye's colour is saved as parseInt(hex, 16); Capture Thumbnail stores the preview's PNG and sets
customThumbnail. Back lists the skin, Duplicate makes "<name> (Copy)", the search narrows the table, Delete asks first
and removes it. The studio's Skins tab offers the skin under Your Skins and Assign Skin imports it. Test closes the
window, enters the sandbox and puts the skin on the player. A second client on the live offline menu has no Skin
Manager item. Outcomes are checked in the database and the files. The client runs on its own assets dir (every
entry linked, user-assets/ its own), so nothing is written to the repository's user-assets. 77 = skip (no
client_bin / kn_brain / websockets).
"""
import asyncio, json, os, re, shutil, sqlite3, struct, subprocess, sys, tempfile, time, zlib
try:
    import websockets
except ImportError:
    print("python websockets missing: skip"); sys.exit(77)

BUILD = os.path.abspath(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()
BRAIN, CLIENT = os.path.join(BUILD, "services", "brain", "kn_brain"), os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD); sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-v10-skins.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
UPLOADS = os.path.join(WORK, "uploads")
PORT = free_ports(1)[0]


def make_assets():
    """the client's own content dir: every entry of the repository's assets linked, user-assets/ a fresh directory"""
    src, assets = os.path.join(ROOT, "assets"), os.path.join(WORK, "assets")
    os.makedirs(os.path.join(assets, "user-assets"))
    for entry in sorted(os.listdir(src)):
        if entry != "user-assets" and not entry.startswith("."):
            os.symlink(os.path.realpath(os.path.join(src, entry)), os.path.join(assets, entry))
    return assets


ASSETS = make_assets()


def png(w=4, h=4):
    raw = b"".join(b"\0" + b"\x80\x40\x20\xff" * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def png_rgba(rgba, w=4, h=4):
    raw = b"".join(b"\0" + bytes(rgba) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


# a PNG's size, colour type and its first row's first pixel (that row's filter undone against a zero row above)
def png_first_pixel(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n": return None
    at, idat, ihdr = 8, b"", None
    while at + 8 <= len(data):
        n, t = struct.unpack(">I", data[at:at + 4])[0], data[at + 4:at + 8]
        if t == b"IHDR": ihdr = struct.unpack(">IIBB", data[at + 8:at + 18])
        if t == b"IDAT": idat += data[at + 8:at + 8 + n]
        at += 12 + n
    raw = zlib.decompress(idat)
    if not ihdr or raw[0] not in (0, 2): return None
    return ihdr[0], ihdr[1], ihdr[3], tuple(raw[1:5])


WAV = b"RIFF\x24\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x44\xac\0\0\x88\x58\x01\0\x02\0\x10\0data\0\0\0\0"
OBJ = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"


def fixture(name, data):
    path = os.path.join(WORK, name)
    open(path, "wb").write(data)
    return path


def cfg_of(sid):
    rows = q("SELECT item_config FROM custom_skins WHERE id = ?", sid)
    try:
        return json.loads(rows[0][0]) if rows and rows[0][0] else {}
    except ValueError:
        return {}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond: FAILS.append(what)


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall(); con.commit(); return rows
    finally:
        con.close()


def wait_q(sql, args, pred, timeout=10.0):
    end = time.time() + timeout
    rows = []
    while time.time() < end:
        rows = q(sql, *args)
        if pred(rows): return rows
        time.sleep(0.3)
    return rows


async def register(name, studio):
    """the account (its id and session token) and its studio (POST /studios over the gapi tunnel: its id)"""
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
        await ws.send(json.dumps(["a", 0, [name, "skins_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                aid, tok = f[2], f[5]
                break
        await ws.send(json.dumps(["gapi", 7, "POST", "/studios", {"name": studio}]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "gapi" and f[1] == 7:
                return aid, tok, ((f[3] or {}).get("data") or {}).get("id") or 0


class Client:
    def __init__(self, name, token, tag, args):
        self.tag, self.cmds = tag, os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        if name:
            open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w").write(name)
            open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w").write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % PORT, "KRUNKNATIVE_ASSETS_DIR": ASSETS})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT] + args, stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            return open(self.log, errors="replace").read()
        except OSError:
            return ""

    def cmd(self, line): open(self.cmds, "a").write(line + "\n")
    def mark(self): return len(self.text())

    def wait_line(self, needle, mark, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.text()[mark:]: return True
            time.sleep(0.15)
        return False

    def has(self, elem_id, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("rect " + elem_id)
            if self.wait_line("#%s rect" % elem_id, m, 2.0): return True
            time.sleep(0.2)
        return False

    def rect(self, elem_id, timeout=5.0):
        m = self.mark()
        self.cmd("rect " + elem_id)
        if not self.wait_line("#%s" % elem_id, m, timeout): return None
        found = re.findall(r"#%s rect (\S+) (\S+) (\S+) (\S+)" % re.escape(elem_id), self.text()[m:])
        return tuple(float(v) for v in found[-1]) if found else None

    def rectsel(self, sel, nth=0, timeout=5.0):
        m = self.mark()
        self.cmd("rectsel %d %s" % (nth, sel))
        if not self.wait_line("sel %s[%d]" % (sel, nth), m, timeout): return None
        found = re.findall(r"sel %s\[%d\] rect (\S+) (\S+) (\S+) (\S+)" % (re.escape(sel), nth), self.text()[m:])
        return tuple(float(v) for v in found[-1]) if found else None

    def drag(self, x0, y0, x1, y1, steps=6):
        self.cmd("press %d %d" % (x0, y0)); time.sleep(0.15)
        for i in range(1, steps + 1):
            self.cmd("move %d %d" % (x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)); time.sleep(0.05)
        time.sleep(0.4)  # the last move is the pointer's before the button goes up
        self.cmd("release %d %d" % (x1, y1)); time.sleep(0.5)

    def gone(self, elem_id, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("rect " + elem_id)
            if self.wait_line("#%s: no box" % elem_id, m, 2.0): return True
            time.sleep(0.2)
        return False

    def count(self, sel):
        m = self.mark()
        self.cmd("count " + sel)
        if not self.wait_line("[ui_cmds] count %s = " % sel, m, 5.0): return -1
        found = re.findall(r"\[ui_cmds\] count %s = (\d+)" % re.escape(sel), self.text()[m:])
        return int(found[-1]) if found else -1

    def textof(self, sel):
        m = self.mark()
        self.cmd("textof " + sel)
        if not self.wait_line("[ui_cmds] textof %s = " % sel, m, 5.0): return ""
        found = re.findall(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(sel), self.text()[m:])
        return found[-1] if found else ""

    def wait_text(self, sel, needle, timeout=8.0):
        end = time.time() + timeout
        t = ""
        while time.time() < end:
            t = self.textof(sel)
            if needle in t: return t
            time.sleep(0.3)
        return t

    def click(self, elem_id):
        self.cmd("clickid " + elem_id); time.sleep(0.5)

    def ui(self, calls):
        self.cmd("ui " + calls); time.sleep(0.6)

    def type(self, elem_id, chars):
        self.click(elem_id); self.cmd("focus " + elem_id); self.cmd("text " + chars); time.sleep(0.5)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path); time.sleep(0.8); return path

    def close(self):
        self.cmd("quit")
        try: self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(5)
        self.out.close()


def mid(r): return (r[0] + r[2] / 2, r[1] + r[3] / 2)


# Shader Studio's node canvas (sgEditor, deob-10.0.7.js:122896-124145): the default graph, Add node with the palette,
# a wire dragged from a port to a port, a node dragged, pan, wheel zoom, Undo, Delete, a template, the JSON overlay
def shader_canvas(A, sid):
    check(A.has("knSmSgCanvas"), "Shader Studio has the node canvas")
    check("Valid graph \u00b7 2 nodes" in A.wait_text("#knSmSgFooter", "Valid graph"), "the default graph (Base Color -> Output) is valid, 2 nodes")
    check("Base Color" in A.textof("#knSmSgN0") and "base1" in A.textof("#knSmSgN0"), "the Base Color node shows its title and id")
    # the canvas into the viewport: the editor's panel scrolls under the wheel (the canvas itself zooms)
    r = A.rect("knSmSgCanvas")
    for _ in range(16):
        if not r or r[1] + r[3] <= 700: break
        A.cmd("move 300 200"); A.cmd("scroll -4"); time.sleep(0.4)
        r = A.rect("knSmSgCanvas")
    check(bool(r) and r[1] >= 0 and r[1] + r[3] <= 720, "the canvas is in view (%s)" % (r,))
    A.shot("shader-canvas")

    # Add node: the palette, its search, Enter picks the first match
    A.click("knSmSgAdd")
    check(A.has("knSmSgPal"), "Add node opens the palette")
    check("Inputs" in A.textof("#knSmSgPalList"), "the palette lists the nodes by category")
    A.type("knSmSgPalSearch", "time")
    check("Time" in A.wait_text("#knSmSgPalList", "Time"), "the search narrows the palette")
    # by category, then title: Delta Time, then Time; ArrowDown moves to Time, Enter picks it
    check("Delta Time" in A.textof("#knSmSgPalItem0") and "Time" in A.textof("#knSmSgPalItem1"), "the matches sort by title (Delta Time, Time)")
    A.cmd("key 264"); time.sleep(0.3)
    A.shot("shader-palette")
    A.cmd("key 257"); time.sleep(0.6)
    check(A.gone("knSmSgPal"), "Enter picks the node and closes the palette")
    check("3 nodes" in A.wait_text("#knSmSgFooter", "3 nodes"), "the new node is in the graph (3 nodes)")

    # a wire: time1.out (float) dragged onto the output's alpha (float)
    src = A.rectsel('[data-node="time1"][data-dir="out"]')
    dst = A.rectsel('[data-node="out"][data-port="alpha"]')
    check(bool(src and dst), "the ports are on the canvas (%s, %s)" % (src, dst))
    if src and dst:
        (x0, y0), (x1, y1) = mid(src), mid(dst)
        A.drag(x0, y0, x1, y1)
        check(A.count('[data-node="out"][data-port="alpha"].connected') == 1, "dropping the wire on alpha connects time1 -> out.alpha")
    # a wire dropped on the empty canvas opens the palette there, for the nodes that take its type
    bsrc = A.rectsel('[data-node="base1"][data-dir="out"]')
    c = A.rect("knSmSgCanvas")
    if bsrc and c:
        x0, y0 = mid(bsrc)
        A.drag(x0, y0, c[0] + c[2] - 40, c[1] + c[3] - 80)
        check(A.has("knSmSgPal"), "a wire dropped on the canvas opens the palette")
        m = A.mark()
        A.cmd("count #knSmSgPalSearch[placeholder^=\"Nodes that take vec3\"]")
        check(A.wait_line("= 1", m, 5.0), "the palette searches the nodes that take vec3")
        A.cmd("key 256"); time.sleep(0.4)
        check(A.gone("knSmSgPal"), "Escape closes the palette")

    # a node dragged by its header; Undo puts it back
    n0 = A.rect("knSmSgN0")
    if n0:
        A.drag(n0[0] + 40, n0[1] + 8, n0[0] + 100, n0[1] + 48)
        n1 = A.rect("knSmSgN0")
        check(bool(n1) and abs(n1[0] - n0[0] - 60) < 3 and abs(n1[1] - n0[1] - 40) < 3, "dragging a node's header moves it with the pointer (%s -> %s)" % (n0, n1))
        A.click("knSmSgUndo"); time.sleep(0.4)
        n2 = A.rect("knSmSgN0")
        check(bool(n2) and abs(n2[0] - n0[0]) < 2 and abs(n2[1] - n0[1]) < 2, "Undo moves it back (%s)" % (n2,))
    # pan: the empty canvas dragged moves every node
    c = A.rect("knSmSgCanvas")
    n0 = A.rect("knSmSgN0")
    if c and n0:
        A.drag(c[0] + c[2] - 30, c[1] + c[3] - 60, c[0] + c[2] - 90, c[1] + c[3] - 40)
        n1 = A.rect("knSmSgN0")
        # the pointer's px over the canvas's on-screen scale (the window's scale(--ui-scale)): the nodes follow it 1:1
        check(bool(n1) and abs((n1[0] - n0[0]) + 60) < 3 and abs((n1[1] - n0[1]) - 20) < 3, "dragging the canvas pans the view (%s -> %s)" % (n0, n1))
    # the wheel zooms about the pointer
    z0 = A.textof("#knSmSgZoom")
    if c:
        A.cmd("move %d %d" % (c[0] + c[2] / 2, c[1] + c[3] / 2)); A.cmd("scroll 2"); time.sleep(0.5)
    z1 = A.textof("#knSmSgZoom")
    check(z0 != z1 and z1.endswith("%"), "the wheel zooms the canvas (%s -> %s)" % (z0, z1))
    A.click("knSmSgFit"); time.sleep(0.3)
    A.shot("shader-wired")

    # Delete: the selected node (time1) and its wire go
    t = A.rectsel('.sgNode[data-node="time1"] .sgNodeHeader')
    if t:
        A.cmd("click %d %d" % mid(t)); time.sleep(0.3)
        A.cmd("key 261"); time.sleep(0.5)
        check("2 nodes" in A.wait_text("#knSmSgFooter", "2 nodes"), "Delete removes the selected node")
        A.click("knSmSgUndo"); time.sleep(0.4)
        check("3 nodes" in A.wait_text("#knSmSgFooter", "3 nodes"), "Undo restores it")

    # Export JSON: the graph in the overlay
    A.click("knSmSgExport")
    check(A.has("knSmSgIo"), "Export JSON opens the overlay")
    A.click("knSmSgIoClose")
    check(A.gone("knSmSgIo"), "the overlay closes")

    # the canvas's onChange is itemConfig.customShader (saved with the skin)
    A.click("knSmSave")
    def graph_saved(r):
        if not r: return False
        g = (cfg_of(sid).get("customShader") or {}).get("graph") or {}
        nodes = {n.get("id"): n for n in g.get("nodes", [])}
        return "time1" in nodes and ((nodes.get("out") or {}).get("inputs") or {}).get("alpha") == {"node": "time1"}
    check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ?", (sid,), graph_saved)),
          "Save: the edited graph (time1 -> out.alpha) is itemConfig.customShader.graph (%s)" % json.dumps((cfg_of(sid).get("customShader") or {}).get("graph"))[:300])

    # Templates: Animated Noise replaces the graph
    A.click("knSmSgTpl")
    check(A.has("knSmSgTpl0"), "Templates lists the surface templates")
    check("Animated Noise" in A.textof("#knSmSgTpl0"), "the first is Animated Noise (its id in Title Case)")
    A.click("knSmSgTpl0"); time.sleep(0.6)
    f = A.wait_text("#knSmSgFooter", "Valid graph")
    check("Valid graph" in f and "3 nodes" not in f, "the template is the graph now (%s)" % f)
    A.shot("shader-template")


# the Appearance tab's Paintable, Wear, Kounter and Charm sections and the preview's Wear / paint controls
# (deob-10.0.7.js:126048-126978, :127496-127530) on the saved weapon skin
def editors(A, sid, sdir):
    def saved(pred, what):
        A.click("knSmSave")
        check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ?", (sid,), lambda r: r and pred(cfg_of(sid)))),
              "Save: %s (%s)" % (what, json.dumps({k: v for k, v in cfg_of(sid).items() if k != "customShader"})[:400]))

    # Paintable: the toggle, Specific meshes with their names, the paint mask (texture "mask", texture_p.png)
    check(A.has("knSmPaintable"), "a weapon skin's Appearance tab has Paintable")
    A.ui("knSkins('paintable',1)")
    check(A.has("paint-targets-mode"), "Paintable on: the Paint area select")
    A.ui("knSkins('paintArea','specific')")
    check(A.has("knSmPaintMeshes"), "Specific meshes: the Mesh names field")
    A.type("knSmPaintMeshes", "barrel, body")
    time.sleep(0.5)
    saved(lambda c: c.get("paintTargets") == ["barrel", "body"], "paintTargets is the mesh names")
    m = A.mark()
    A.ui("knSkins('file','texture','mask','%s')" % fixture("mask.png", png()))
    check(A.wait_line("[skins] uploaded texture mask", m, 20.0), "the paint mask uploads (texture mask)")
    check(os.path.isfile(os.path.join(sdir, "texture_p.png")), "the brain stores it as skins/<id>/texture_p.png")
    check(A.has("knSmPaintMaskRemove"), "the uploaded mask shows with Remove")
    saved(lambda c: (c.get("customTextures") or {}).get("mask") is True, "customTextures.mask")

    # Wear: two of the four layers, Generate packs their red channels into R and A of a 1024 px texture_w.png
    check(A.has("knSmWear"), "a weapon skin's Appearance tab has Wear")
    A.ui("knSkins('wearOn',1)")
    check(A.has("knSmWear-rust") and A.has("knSmWear-wear"), "Wear on: the four layers")
    A.ui("knSkins('file','wear','rust','%s')" % fixture("rust.png", png_rgba((200, 10, 20, 255))))
    A.ui("knSkins('file','wear','wear','%s')" % fixture("general.png", png_rgba((50, 60, 70, 255), 8, 8)))
    check("(2/4 layers)" in A.wait_text("#knSmWearGenerate", "2/4"), "the button counts the picked layers (2/4)")
    check("Ready" in A.textof("#knSmWear-rust") and "Ready" not in A.textof("#knSmWear-dirt"), "a picked layer is Ready")
    A.shot("editor-wear")
    m = A.mark()
    A.ui("knSkins('wearGenerate')")
    check(A.wait_line("[skins] wear map packed: 2 layer(s)", m, 20.0), "Generate packs the two layers")
    check(A.wait_line("[skins] uploaded texture wear", m, 30.0), "the wear map uploads (texture wear)")
    wpath = os.path.join(sdir, "texture_w.png")
    px = png_first_pixel(open(wpath, "rb").read()) if os.path.isfile(wpath) else None
    check(px == (1024, 1024, 6, (200, 0, 0, 50)), "texture_w.png is 1024 px RGBA: rust in R, general wear in A, the rest 0 (%s)" % (px,))
    check(A.has("knSmWearExisting"), "the packed wear map shows")
    check("Ready" not in A.textof("#knSmWear-rust"), "the uploaded layers are cleared")
    saved(lambda c: (c.get("customTextures") or {}).get("wear") is True, "customTextures.wear")

    # the preview: Wear (a wear map) and the paint select (paintable); each sets the iframe's query
    check(A.has("knSmPvWear") and A.has("knSmPvPaint"), "the preview's Wear button and paint select")
    # Clear Preview: the item's own material again (a Shader Studio graph replaces it, wear map and all, as v10's
    # customShaderInfo material does after loadAnimMesh's wear, deob-10.0.7.js:207980-208000)
    A.ui("knSkins('shader','clearPreview')")
    m = A.mark()
    A.ui("knSkins('pvWear')")
    check(A.has("knSmPvWearLevel"), "Wear opens its slider")
    A.ui("knSkins('pvWearLevel','0.5')")
    check(A.wait_line("&wear=0.5", m, 5.0), "the slider's wear is the preview's (&wear=0.5)")
    check(A.wait_line("[viewer] showcase wear map: skins/%s/texture_w at 0.5" % sid, m, 10.0),
          "the preview draws the skin's own texture_w wear map at that level")
    A.ui("knSkins('pvPaint','7776')")
    check(A.wait_line("&paint=dc3c3c", m, 5.0), "a paint picked is the preview's (Red Paint, &paint=dc3c3c)")
    A.shot("editor-preview-controls")
    A.ui("knSkins('pvPaint','')")

    # Kounter: Add, the fields (digits into 1..10, degrees to radians), the stat the preview counts (1284)
    check(A.has("knSmKounterAdd"), "a weapon skin's Kounter section offers Add Kounter")
    A.ui("knSkins('attachAdd')")
    check(A.has("knSmKounterStat"), "Add Kounter: the tools and fields")
    A.ui("knSkins('attachMode','snap')")
    check("mount the Kounter on that surface" in A.textof("#knSmKounter"), "the Snap tool's hint")
    m = A.mark()
    A.ui("knSkins('attachSet','digits','12')")
    A.ui("knSkins('attachRot',0,'90')")
    A.ui("knSkins('attachStat','jumps')")
    check(A.wait_line("&jumps=1284", m, 5.0), "the preview asks for the Kounter's stat at 1284")
    check(A.wait_line("[viewer] showcase counter: jumps 1284, 10 digits", m, 20.0), "the preview draws the Kounter")
    saved(lambda c: (c.get("attach") or {}).get("type") == 1 and c["attach"].get("digits") == 10 and c["attach"].get("stat") == "jumps"
          and (c["attach"].get("rot") or [0])[0] == 1.571 and c["attach"].get("color") == "#7fe3ff", "itemConfig.attach")
    A.shot("editor-kounter")

    # Charm: the weapon's spot to start from, a field, the scale's floor, Hide charms
    check(A.has("knSmCharmCustomize"), "a weapon skin with a charm mount: Customize charm placement")
    A.ui("knSkins('charmCustomize')")
    check(A.has("knSmCharmX"), "the charm's tools and fields")
    A.ui("knSkins('charmSet','x','0.25')")
    A.ui("knSkins('charmSet','scl','0.01')")
    saved(lambda c: c.get("charmMXOff") == 0.25 and c.get("charmMYOff") == -0.4 and c.get("charmMZOff") == -0.91 and c.get("charmMScl") == 0.05,
          "the charm placement (charmM{X,Y,Z}Off from the weapon's, charmMScl at least 0.05)")
    A.ui("knSkins('noCharm',1)")
    check(A.gone("knSmCharmX"), "Hide charms hides the placement")
    saved(lambda c: c.get("noCharm") is True, "noCharm")
    A.ui("knSkins('noCharm',0)")
    A.ui("knSkins('charmReset')")
    check(A.has("knSmCharmCustomize"), "Reset to weapon default")
    saved(lambda c: "charmMXOff" not in c and "noCharm" not in c, "the charm keys are gone again")
    A.shot("editor-charm")


# a paint (type 18): Solid Colour previewed on paintPreviewBase, then Animated Shader's node canvas (kind "paint")
def paint_editor(A, pid):
    check(A.has("knSmPaint"), "a paint's Appearance tab has its Paint section")
    check(not A.has("knSmWear", 1.0), "a paint has no Wear section")
    m = A.mark()
    A.ui("knSkins('paintColor','#ff0000')")
    check(A.wait_line("showcase=7805&paint=ff0000", m, 5.0), "the paint is previewed on item 7805 (&paint=ff0000)")
    A.ui("knSkins('paintBase','7798')")
    check(A.wait_line("showcase=7798", m, 5.0), "Cosmetic: the preview item is 7798")
    A.ui("knSkins('paintMode','shader')")
    check(A.has("knSmPgCanvas"), "Animated Shader: the node canvas")
    check("Valid graph" in A.wait_text("#knSmPgFooter", "Valid graph"), "the default paint graph is valid")
    A.shot("editor-paint")
    A.click("knSmSave")
    check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ?", (pid,),
                      lambda r: r and (cfg_of(pid).get("paintShaderGraph") or {}).get("v") == 1 and "paintColor" not in cfg_of(pid))),
          "Save: paintShaderGraph, no paintColor (%s)" % json.dumps(cfg_of(pid))[:300])


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "v10skins-%d" % os.getpid()},
               "uploads": {"dir": UPLOADS},
               "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                            "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, open(cfg, "w"))
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    clients = []
    try:
        for _ in range(200):
            try:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, open(os.path.join(WORK, "brain.log")).read()): break
            except OSError: pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        name = "skM%s" % stamp
        aid, tok, stid = asyncio.run(register(name, "SkinStudio" + stamp))

        A = Client(name, tok, "sandbox", ["--offline", "--sandbox"])
        clients.append(A)
        check(A.has("menuBtnSocial", 40.0), "the sandbox page is up")
        check(A.has("menuBtnSkinManager"), "the sandbox menu shows the Skin Manager item ($skinManagerEnabled)")
        A.click("menuBtnSkinManager")
        check(A.has("knSmWindow"), "Skin Manager opens its window (showSkinManager)")
        check("No data available" in A.wait_text("#knSmWindow", "No data available"), "the table: no skin yet")
        A.shot("empty")

        A.click("knSmAdd")
        rows = wait_q("SELECT id, name, type, weapon, rarity FROM custom_skins WHERE owner_id = ?", (aid,), lambda r: r)
        check(len(rows) == 1 and rows[0][1:] == ("New Skin", 0, 1, 0), "Add new skin: \"New Skin\" (weapon 1) is in kn_brain at once")
        sid = rows[0][0] if rows else 0
        check(str(sid) in A.wait_text("#skin-id", str(sid)), "the editor shows the new skin's id")
        A.shot("editor-general")

        A.ui("knSkins('input','name','Neon Sniper')")
        A.ui("knSkins('set','rarity','3')")
        A.click("knSmSave")
        check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ? AND name = 'Neon Sniper' AND rarity = 3", (sid,), lambda r: r)),
              "Save: the new name and rarity are in kn_brain")
        A.click("knSmTab1")
        check(A.has("copy-id-select"), "the Appearance tab's Same attributes as")
        check(A.has("knSmPreview"), "the editor's 3D preview (the viewer iframe, 300 ms after the editor opened)")
        for sec, what in (("knSmModel", "Model"), ("knSmTextures", "Textures"), ("knSmSounds", "Sounds"), ("knSmShader", "Shader Studio")):
            check(A.has(sec, 5.0), "a weapon skin's Appearance tab has its %s section" % what)

        sdir, local = os.path.join(UPLOADS, "skins", str(sid)), os.path.join(ASSETS, "user-assets", "skins", str(sid))
        for kind, cat, fname, data, stored, flag in (
                ("texture", "diffuse", "neon.png", png(), "texture.png", lambda c: (c.get("customTextures") or {}).get("diffuse") is True),
                ("sound", "shoot", "shot.wav", WAV, "sound.mp3", lambda c: c.get("customSound") is True),
                ("model", "", "gun.obj", OBJ, "model.obj", lambda c: c.get("customModel") is True and c.get("fileExt") == "obj")):
            m = A.mark()
            A.ui("knSkins('file','%s','%s','%s')" % (kind, cat, fixture(fname, data)))
            check(A.wait_line("[skins] uploaded %s" % kind, m, 20.0), "the %s upload finishes (key, chunks, done)" % kind)
            check(os.path.isfile(os.path.join(sdir, stored)) and open(os.path.join(sdir, stored), "rb").read() == data,
                  "the brain stores %s as skins/<id>/%s" % (fname, stored))
            check(os.path.isfile(os.path.join(local, stored)), "the client keeps its own copy of skins/<id>/%s" % stored)
            A.click("knSmSave")
            check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ?", (sid,), lambda r: r and flag(cfg_of(sid)))),
                  "Save: itemConfig records the %s (%s)" % (kind, json.dumps(cfg_of(sid))))
        check("Replace" in A.textof("#knSmTex-diffuse"), "the uploaded texture shows with Replace / Remove")
        check("Custom sound uploaded" in A.textof("#knSmSound-shoot"), "the uploaded sound shows \"Custom sound uploaded\"")
        check("Format: OBJ" in A.textof("#knSmModel").replace("  ", " "), "the model section shows the uploaded OBJ")
        A.ui("knSkins('glow',1)")
        A.ui("knSkins('file','texture','emissive','%s')" % fixture("notes.txt", b"hello"))
        check("File type not supported. Accepted types: .png, .jpg, .jpeg" in A.wait_text("#knSmTex-emissive", "File type not supported"),
              "a refused file shows the drop zone's message")
        A.shot("editor-appearance")

        A.ui("knSkins('shader','toggle')")
        check(A.has("knSmShaderApply"), "Shader Studio opens")
        shader_canvas(A, sid)
        A.ui("knSkins('shader','clearPreview')")  # the canvas's edits already show: the preview without, then again
        time.sleep(1.0)
        m = A.mark()
        A.ui("knSkins('shader','refresh')")  # the Refresh Preview button sits below the 720 px fold
        check(A.wait_line("[skin_fx] graph material", m, 20.0), "Refresh Preview: the preview draws the compiled graph (getMat's shaderDef)")
        A.click("knSmSave")
        check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ?", (sid,),
                          lambda r: r and (cfg_of(sid).get("customShader") or {}).get("type") == "graph")),
              "Refresh Preview: itemConfig.customShader is the graph (saved)")

        editors(A, sid, sdir)
        m = A.mark()
        A.click("knSmCapture")
        check(A.wait_line("[skins] captured %d" % sid, m, 30.0), "Capture Thumbnail: the preview's PNG is uploaded and the skin saved")
        thumb = os.path.join(sdir, "thumbnail.png")
        check(os.path.isfile(thumb) and open(thumb, "rb").read(8) == b"\x89PNG\r\n\x1a\n", "the brain stores thumbnail.png")
        check(cfg_of(sid).get("customThumbnail") is True, "itemConfig.customThumbnail is saved")
        A.shot("captured")

        A.click("knSmTab2")
        check(A.has("knSmAutoAttrs"), "the Advanced tab's Automatic Attributes")
        A.shot("editor-advanced")
        A.click("knSmBack")
        t = A.wait_text("#knSmRows", "Neon Sniper #%d" % sid)
        check("Neon Sniper #%d" % sid in t, "Back: the table lists \"Neon Sniper #id\"")

        A.click("knSmDuplicate%d" % sid)
        dup = wait_q("SELECT id FROM custom_skins WHERE owner_id = ? AND name = 'Neon Sniper (Copy)'", (aid,), lambda r: r)
        check(bool(dup), "Duplicate: \"Neon Sniper (Copy)\" is in kn_brain")
        did = dup[0][0] if dup else 0
        check(A.has("knSmRow%d" % did), "the copy is in the table")
        A.shot("list")

        A.type("knSmSearch", "copy")
        time.sleep(1.0)
        check(A.gone("knSmRow%d" % sid) and A.has("knSmRow%d" % did), "the search narrows the table to the copy")
        A.click("knSmSearchClear")
        check(A.has("knSmRow%d" % sid), "clearing the search lists both again")

        A.click("knSmDelete%d" % did)
        check(A.has("knSmConfirm"), "Delete asks first (Delete Skin)")
        A.shot("delete-confirm")
        A.click("knSmConfirm")
        check(bool(wait_q("SELECT count(*) FROM custom_skins WHERE id = ?", (did,), lambda r: r and r[0][0] == 0)), "Delete: the copy is gone")
        check(A.gone("knSmRow%d" % did) and A.has("knSmRow%d" % sid), "the table no longer lists it")

        # a dye: its colours as numbers (iïiííìí), on a skin of its own
        A.click("knSmAdd")
        rows = wait_q("SELECT id FROM custom_skins WHERE owner_id = ? AND id NOT IN (?, ?)", (aid, sid, did), lambda r: r)
        dye = rows[0][0] if rows else 0
        check(str(dye) in A.wait_text("#skin-id", str(dye)), "a second new skin (the dye)")
        A.ui("knSkins('set','type','5')")
        A.click("knSmTab1")
        check(A.has("knSmDye"), "a dye's Appearance tab has Dye Colors")
        A.ui("knSkins('dye','skinCol','#ff8000')")
        A.ui("knSkins('opac','0.5')")
        A.click("knSmSave")
        check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ?", (dye,),
                          lambda r: r and cfg_of(dye).get("skinCol") == 0xff8000 and cfg_of(dye).get("opac") == 0.5)),
              "the dye's colour and opacity are saved (%s)" % json.dumps(cfg_of(dye)))
        A.click("knSmBack")

        # a paint, on a skin of its own
        A.click("knSmAdd")
        rows = wait_q("SELECT id FROM custom_skins WHERE owner_id = ? AND id NOT IN (?, ?, ?)", (aid, sid, did, dye), lambda r: r)
        pid = rows[0][0] if rows else 0
        check(str(pid) in A.wait_text("#skin-id", str(pid)), "a third new skin (the paint)")
        A.ui("knSkins('set','type','18')")
        A.click("knSmTab1")
        paint_editor(A, pid)
        A.click("knSmBack")

        A.click("knSmClose")
        check(A.gone("knSmWindow"), "the close button closes the window")

        # the studio's Skins tab: Your Skins lists the skin with a thumbnail, Assign Skin imports it
        check(stid > 0, "the account's studio was created (%r)" % stid)
        A.ui("openStudios()")
        A.ui("knStudios('open',%d)" % stid)
        A.ui("knStudios('tab',2)")
        check("Neon Sniper" in A.wait_text("#knSdSkinSel", "Neon Sniper"), "Your Skins lists the skin with a captured thumbnail")
        check("New Skin" not in A.textof("#knSdSkinSel"), "a skin without a thumbnail is not listed")
        A.ui("knStudios('skinsel','%d')" % sid)
        A.click("knSdSkinAssign")
        check(bool(wait_q("SELECT 1 FROM studio_skins WHERE studio_id = ? AND custom_skin_id = ?", (stid, sid), lambda r: r)),
              "Assign Skin imports it into the studio")
        A.shot("studio-skins")
        A.cmd("key 256"); time.sleep(0.5)
        A.cmd("key 256"); time.sleep(0.5)

        # Test: the window closes, the sandbox game starts, the skin goes on the player's primary
        A.click("menuBtnSkinManager")
        check(A.has("knSmTest%d" % sid), "the table's Test button")
        m = A.mark()
        A.click("knSmTest%d" % sid)
        check(A.gone("knSmWindow"), "Test closes the window")
        check(A.wait_line("[skins] test %d type=0 slot=primary" % sid, m, 30.0), "Test puts the skin on the sandbox player's primary")
        check(A.wait_line("[skin_fx] graph material", m, 20.0), "Test: the player's primary draws the skin's shader graph (customShaderInfo)")
        A.shot("test")

        B = Client(None, None, "live", ["--offline"])
        clients.append(B)
        check(B.has("menuBtnSocial", 40.0), "the live offline page is up")
        check(B.gone("menuBtnSkinManager"), "the live menu has no Skin Manager item")
    except Exception as e:
        import traceback; traceback.print_exc(); check(False, "exception: %r" % e)
    finally:
        for c in clients: c.close()
        brain.terminate()
        try: brain.wait(10)
        except subprocess.TimeoutExpired: brain.kill()
        log.close()
    print("v10 skin manager e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"): shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
