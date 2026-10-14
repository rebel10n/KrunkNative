#!/usr/bin/env python3
"""The map editor's AI panel end to end (client/src/editor/editor_ai.c, services/brain/ai.c).

  python3 -I tools/editor/ai_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server) on an ephemeral port,
talking to the stub upstream of services/brain/tests/ai_stub.py (never the live provider) and the
stub MCP for web search. Headless editors (client_bin --editor, EGL, no window on any display,
KRUNKNATIVE_EDITOR_SCRIPT):

1. guest: the panel opens, lists the provider's 31 models smartest first (ai_model_rank) with
   glm-5.3-flash selected (a patch-2 stored model without "picked" is ignored), shows "Sign in to
   use AI" and no account indicator, sends nothing; the header row shares one centre line and the
   Tools / Web / Ask / Stop buttons one size (element boxes); the model picker's open popup is
   readable (pixels: dark list, light text, contrast >= 4.5);
2. signed in (session restored from storage): a prompt streams a reply (text + thinking trace),
   the usage line fills in, the upstream sees the system prompt and the message;
3. the model asks for editor tools: objects_create runs in this editor against the open map,
   map_save_as waits for approval and writes the map once allowed, web_search runs on the brain
   (stub MCP), render_view shows the editor's render in the card (the model gets text);
4. a second editor restores the conversation from the local history;
5. the user's own endpoint (a second stub with its own key): called directly, never via the brain;
6. checkpoints: a tool's map edit, Restore checkpoint (confirm, the cube undone, the messages out,
   Redo brings both back), Edit + resend (asks: undo the later edits or keep them), Rewind (the text
   back in the composer, Redo), the checkpoints kept in the local history.
Frames of every step stay in the work directory with KN_AI_E2E_KEEP=1.
Exit 77 when the database, python websockets or the binaries are missing.
"""
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "services", "brain", "tests"))
from ai_stub import REAL_SMART_ORDER, Upstream, real_models  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_png import read_rgba  # noqa: E402

SMART_ORDER = REAL_SMART_ORDER  # ai_model_rank's order of the provider's 31 models

DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_edai_%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-editor-ai-", dir=os.path.expanduser("~/.cache"))
KEY = "sk-editor-ai-%d" % os.getpid()
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)[:1500]) if detail and not ok else ''}", flush=True)


def wait_for(path, pattern, secs=15.0):
    end = time.time() + secs
    while time.time() < end:
        with open(path, errors="replace") as f:
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.1)
    return None


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def centre_y(r):
    return r[1] + r[3] / 2


def lum(c):
    def ch(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return 0.2126 * ch(c[0]) + 0.7152 * ch(c[1]) + 0.0722 * ch(c[2])


def contrast(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def popup_colours(png, sel):
    """The open model picker's list below the select: (background, best text contrast, rows seen)."""
    w, h, ch, rows = read_rgba(png)
    x0, x1 = int(sel[0]) + 4, int(sel[0] + sel[2]) - 20  # inside the border, left of the scrollbar
    y0, y1 = int(sel[1] + sel[3]) + 4, min(h - 1, int(sel[1] + sel[3]) + 150)
    hist = {}
    px = []
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            c = tuple(row[x * ch:x * ch + 3])
            hist[c] = hist.get(c, 0) + 1
            px.append(c)
    if not hist:
        return None, 0
    bg = max(hist, key=hist.get)
    hl = (0x1e, 0x90, 0xff)  # the hovered row (#1e90ff): its white text does not count
    best = max((contrast(c, bg) for c in px if abs(c[0] - hl[0]) + abs(c[1] - hl[1]) + abs(c[2] - hl[2]) > 60), default=0)
    return bg, best


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=4 << 20) as ws:
        await ws.send(json.dumps(["a", 0, [name, "editorpass1", None], None]))
        end = time.time() + 10
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
            if f[0] == "a":
                return f[2], f[5]


def editor(port, cfg, script, tag, extra_env=None):
    """One headless editor run; KRUNKNATIVE_SCREENSHOT ends it once the script is done."""
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                "KRUNKNATIVE_SCREENSHOT": os.path.join(WORK, tag + ".png"), "KRUNKNATIVE_EDITOR_SCRIPT": ";".join(script) + ";wait:0.5"})
    env.update(extra_env or {})
    log = os.path.join(WORK, tag + ".log")
    with open(log, "w") as out:
        r = subprocess.run([os.path.join(BUILD, "client_bin"), "--editor", "new"], cwd=ROOT, env=env, stdout=out,
                           stderr=subprocess.STDOUT, timeout=300)
    check("%s: client_bin exits cleanly" % tag, r.returncode == 0, "exit %d (%s)" % (r.returncode, log))
    return read(log)


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")) or not os.path.exists(os.path.join(BUILD, "services/brain/kn_brain")):
        print("ai_e2e: client_bin or kn_brain not built: skipped")
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("ai_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    up = Upstream(KEY, models=real_models()).start()
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")}
    env.update(dbenv, KN_AI_UPSTREAM_URL=up.url, KN_AI_UPSTREAM_KEY=KEY)
    brain = None
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([os.path.join(BUILD, "services/brain/kn_brain"), "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        stub_mcp = "%s -I %s mcp" % (sys.executable, os.path.join(ROOT, "services", "brain", "tests", "ai_stub.py"))
        brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.id=edai-%d" % os.getpid(), "--accounts.email.required=false", "--smtp.host=",
                                  "--ai.search.command=" + stub_mcp], cwd=ROOT, env=env,
                                 stdout=open(brain_log, "w"), stderr=subprocess.STDOUT)
        m = wait_for(brain_log, r"listening on http://127\.0\.0\.1:(\d+) \(ws")
        if not m:
            check("kn_brain starts", False, brain_log)
            return 1
        port = int(m.group(1))
        D = lambda n: os.path.join(WORK, n)

        # ---- 1. guest
        cfg_guest = D("cfg-guest")
        os.makedirs(os.path.join(cfg_guest, "krunknative"), exist_ok=True)
        with open(os.path.join(cfg_guest, "krunknative", "knAiSettings.json"), "w") as f:  # patch 2's format: no "picked"
            f.write(json.dumps({"tools": True, "web": True, "effort": "", "model": "gpt-oss-20b"}))
        with urllib.request.urlopen("http://127.0.0.1:%d/ai/models" % port, timeout=10) as r:
            bm = json.loads(r.read())
        check("/ai/models: smartest first", [m["id"] for m in bm.get("models", [])] == SMART_ORDER, [m["id"] for m in bm.get("models", [])])
        check("/ai/models: default glm-5.3-flash", bm.get("default") == "glm-5.3-flash" and bm.get("preferred") == "glm-5.3-flash", bm.get("default"))
        before = len(up.chat_requests())
        editor(port, cfg_guest, ["closewindow", "ai:open", "wait:2", "ai:send:hello", "wait:1", "ai:dump:" + D("guest.json"),
                                 "ai:boxes:" + D("boxes.json"), "ai:openmodels", "wait:0.6"], "guest")
        g = load(D("guest.json"))
        check("guest: the panel is shown", g.get("open") and g.get("shown"), g)
        check("guest: models listed smartest first", g.get("models") == SMART_ORDER, g.get("models"))
        check("guest: glm-5.3-flash selected by default (an unpicked stored model ignored)", g.get("model") == "glm-5.3-flash" and not g.get("model_picked"),
              g.get("model"))
        check("guest: 'Sign in to use AI'", g.get("banner_kind") == 4 and not g.get("signed_in"), g)
        sent = [r for r in up.chat_requests()[before:] if "KN-PROBE" not in json.dumps(r["body"])]
        check("guest: nothing sent to the upstream", not sent and not g.get("conv"), g.get("conv"))
        check("no account indicator in the panel", g.get("account_indicator") is False and "Guest" not in g.get("list_text", ""), g.get("account_indicator"))
        bx = load(D("boxes.json"))
        k = bx["knAi"][2] / 380.0 if bx.get("knAi") else 1.0  # the editor page's scale (CSS px -> viewport px)
        head = [bx.get(k) for k in ("knAiTitle", "knAiModel", "knAiNewBtn", "knAiHistBtn", "knAiSetBtn", "knAiCloseBtn")]
        ok = all(head)
        cy = [centre_y(r) for r in head] if ok else []
        check("header: title, model picker and icons share one centre line (+-0.5 px)", ok and max(cy) - min(cy) <= 0.5, cy)
        check("header: the model picker is as tall as the icons (24 px)",
              ok and abs(bx["knAiModel"][3] / k - 24) < 0.6 and abs(bx["knAiNewBtn"][3] / k - 24) < 0.6 and abs(bx["knAiTitle"][3] / k - 24) < 0.6,
              [r[3] / k for r in head] if ok else head)
        hc = centre_y(bx["knAiHead"]) if bx.get("knAiHead") else 0
        check("header: the row is centred in the header bar (+-1 px)", ok and abs(cy[1] - hc) <= 1.0 * k, (cy, hc))
        btn = [bx.get(k) for k in ("knAiTools", "knAiWeb", "knAiSend", "knAiStop")]
        ok = all(btn)
        check("composer: Tools / Web / Ask / Stop are one height (24 px) on one line",
              ok and all(abs(r[3] / k - 24) < 0.6 for r in btn) and max(centre_y(r) for r in btn) - min(centre_y(r) for r in btn) <= 0.5, btn)
        bg, best = popup_colours(D("guest.png"), bx.get("knAiModel", [0, 0, 0, 0]))
        check("model picker popup: dark list, not white (%s)" % (bg,), bg is not None and lum(bg) < 0.1, bg)
        check("model picker popup: option text readable (contrast %.1f >= 4.5)" % best, best >= 4.5, best)

        # ---- 2. signed in
        stamp = str(int(time.time()) % 100000)
        acc_id, token = asyncio.run(register(port, "edai" + stamp))
        check("account registered", bool(token), token)
        cfg = D("cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", "edai" + stamp), ("krunker_token", token), ("krunker_id", str(acc_id))):
            with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        s = ["closewindow", "wait:2", "ai:open", "wait:1.5", "ai:send:hello", "wait:3", "ai:trace:1", "ai:dump:" + D("chat.json")]
        log = editor(port, cfg, s, "chat")
        c = load(D("chat.json"))
        conv = c.get("conv", [])
        check("signed in: no sign-in card", c.get("signed_in") and c.get("banner_kind") != 4, c)
        check("signed in: the reply streamed", len(conv) >= 2 and conv[-1].get("content") == "Hello from the stub: hello", conv)
        check("thinking trace kept", conv and conv[-1].get("reasoning_content") == "Thinking about it.", conv)
        check("the trace expands", "Thinking about it." in c.get("list_text", ""), c.get("list_text"))
        check("usage line", "today: 1 requests" in c.get("usage", ""), c.get("usage"))
        check("signed in: still no account indicator", c.get("account_indicator") is False, c.get("account_indicator"))
        req = up.chat_requests()[-1]
        check("the upstream got the system prompt + the user's message",
              req["body"]["messages"][0]["role"] == "system" and req["body"]["messages"][-1] == {"role": "user", "content": "hello"},
              req["body"]["messages"])
        check("no token or key in the editor log", token not in log and KEY not in log, log[-500:])

        # ---- 3. editor tools, approvals, web search, renders (the stub asks for the tools)
        data = D("data")
        saved = os.path.join(data, "krunknative", "editor-ai", "ai_saved.json")
        create = json.dumps({"objects": [{"prefab": "CUBE", "p": [0, 0, 0], "s": [10, 10, 10]}, {"prefab": "CRATE", "p": [30, 0, 0]}]})
        s = ["closewindow", "wait:2", "ai:open", "wait:1.5",
             "ai:send:call tool objects_create " + create, "wait:4", "objdump:" + D("objs.txt"),
             "ai:send:call tool map_save_as " + json.dumps({"path": saved, "overwrite": True}), "wait:3", "ai:dump:" + D("approval.json"),
             "ai:approve:5:0:1", "wait:4",
             "ai:send:call tool web_search " + json.dumps({"query": "krunker parkour"}), "wait:3",
             "ai:send:call tool render_view " + json.dumps({"position": [0, 60, 120], "target": [0, 0, 0], "width": 160, "height": 90}),
             "wait:5", "ai:tool:13:0", "ai:dump:" + D("tools.json")]
        log = editor(port, cfg, s, "tools", {"XDG_DATA_HOME": data})
        objs = read(D("objs.txt"))
        check("objects_create ran in the editor: a cube and a crate", "CUBE" in objs and "CRATE" in objs, objs[:400])
        ap = load(D("approval.json"))
        conv = ap.get("conv", [])
        call = conv[5] if len(conv) > 5 else {}
        st = call.get("meta", {}).get("tools", {}).get("call_stub1", {}).get("status")
        check("map_save_as waits for approval", st == "approval" and "Allow all this session" in ap.get("list_text", ""),
              "%s / %s" % (st, ap.get("list_text", "")[-300:]))
        t = load(D("tools.json"))
        conv = t.get("conv", [])
        roles = [m.get("role") for m in conv]
        check("tool rounds: user / assistant(call) / tool / assistant x4", roles == ["user", "assistant", "tool", "assistant"] * 4, roles)
        res0 = conv[2].get("content", "") if len(conv) > 2 else ""
        check("objects_create result went back to the model", '"created"' in res0 and conv[3].get("content", "").startswith("Tool said:"), res0[:300])
        check("approved map_save_as wrote the map", os.path.exists(saved), saved)
        web = conv[10].get("content", "") if len(conv) > 10 else ""
        check("web search ran on the brain (stub MCP)", "Stub result for krunker parkour" in web, web[:200])
        rv = conv[13] if len(conv) > 13 else {}
        imgs = rv.get("meta", {}).get("tools", {}).get("call_stub1", {}).get("images", [])
        check("render_view: the editor's render shown in the card", len(imgs) == 1 and imgs[0].startswith("data:image/png;base64,"), str(imgs)[:200])
        check("the model gets text, not the image", "image 1: image/png" in (conv[14].get("content", "") if len(conv) > 14 else ""),
              conv[14] if len(conv) > 14 else conv)
        req = [r for r in up.chat_requests() if "objects_create" in json.dumps(r["body"].get("tools", []))]
        check("the editor's tools are offered to the model (54 minus editor_connect)", req and len(req[-1]["body"]["tools"]) >= 50,
              len(req[-1]["body"]["tools"]) if req else 0)
        hist = t.get("history", [])
        check("the chat is in the local history", hist and hist[0].get("title", "").startswith("call tool objects_create"), [h.get("title") for h in hist])

        # ---- 4. a new editor: the history restores the conversation
        editor(port, cfg, ["closewindow", "wait:1", "ai:open", "wait:1", "ai:history:0", "ai:dump:" + D("restored.json")], "restored",
               {"XDG_DATA_HOME": data})
        rs = load(D("restored.json"))
        check("history: the chat restores in a new editor", len(rs.get("conv", [])) == 16 and "Tool said:" in rs.get("list_text", ""),
              len(rs.get("conv", [])))

        # ---- 5. the user's own endpoint (the stub upstream directly, with its own key): never via the brain
        own_key = "sk-own-%d" % os.getpid()
        up2 = Upstream(own_key).start()
        try:
            n_brain = len(up.chat_requests())
            editor(port, cfg, ["closewindow", "wait:1", "ai:open", "wait:1", "ai:endpoint:%s|%s|gpt-oss-20b" % (up2.url, own_key), "wait:1.5",
                               "ai:pop:2", "ai:dump:" + D("own-settings.json"), "ai:pop:0", "ai:new", "ai:send:hello", "wait:3",
                               "ai:dump:" + D("own.json"), "ai:endpoint:"], "own", {"XDG_DATA_HOME": data})
            o = load(D("own.json"))
            conv = o.get("conv", [])
            check("own endpoint: the reply streams from it", o.get("custom") and conv and conv[-1].get("content") == "Hello from the stub: hello", conv)
            reqs = up2.chat_requests()
            hl = {k.lower(): v for k, v in (reqs[-1]["headers"] if reqs else {}).items()}
            check("own endpoint: called directly with the user's key", reqs and hl.get("authorization") == "Bearer " + own_key
                  and reqs[-1]["body"].get("model") == "gpt-oss-20b" and reqs[-1]["body"].get("tools"), hl.get("authorization"))
            check("own endpoint: nothing went through the brain's upstream", len(up.chat_requests()) == n_brain)
            st = load(D("own-settings.json"))
            check("own endpoint: the settings warn about it", "stored on this computer only" in st.get("pop_text", ""), st.get("pop_text", "")[:300])
            check("own endpoint: models listed from it", st.get("models", [])[:2] == ["glm-5.3-flash", "gpt-oss-20b"], st.get("models"))
            check("own endpoint: the key never reaches the brain", own_key not in read(brain_log))
        finally:
            up2.stop()

        # ---- 6. checkpoints: Restore checkpoint (+ Redo), Edit + resend, Rewind (+ Redo)
        cube = json.dumps({"objects": [{"prefab": "CUBE", "p": [0, 0, 0], "s": [10, 10, 10]}]})
        data6 = D("data6")
        o1, o2, o3, o4 = D("cp-objs1.txt"), D("cp-objs2.txt"), D("cp-objs3.txt"), D("cp-objs4.txt")
        # one run per frame (a screenshot ends a run): each replays the steps up to its frame
        steps = [
            ("cp-1-chat", ["closewindow", "wait:2", "ai:open", "wait:1.5", "ai:send:hello", "wait:3",
                           "ai:send:call tool objects_create " + cube, "wait:4", "objdump:" + o1, "ai:dump:" + D("cp1.json")]),
            # Restore checkpoint on message 2 (the tool call): asks first
            ("cp-2-confirm", ["ai:restore:2", "ai:dump:" + D("cp2.json")]),
            ("cp-3-restored", ["ai:confirm:1", "wait:0.3", "objdump:" + o2, "ai:dump:" + D("cp3.json")]),
            ("cp-4-redone", ["ai:redo", "wait:0.3", "objdump:" + o3, "ai:dump:" + D("cp4.json")]),
            # Edit message 0: the cube was made after it, so Send asks: undo and send
            ("cp-5-edit", ["ai:edit:0", "ai:edittext:hello again"]),
            ("cp-6-edit-confirm", ["ai:editsend", "ai:dump:" + D("cp5.json")]),
            ("cp-7-edited", ["ai:confirm:1", "wait:3", "objdump:" + o4, "ai:dump:" + D("cp6.json")]),
            # Rewind message 0: the chat empties, its text goes back into the composer; Redo; Rewind again
            ("cp-8-rewound", ["ai:rewind:0", "ai:dump:" + D("cp7.json"), "ai:redo", "wait:0.3", "ai:dump:" + D("cp8.json"), "ai:rewind:0"]),
        ]
        frames = ("cp-2-confirm", "cp-3-restored", "cp-5-edit", "cp-6-edit-confirm", "cp-8-rewound")
        for i, (tag, _) in enumerate(steps):
            if tag in frames:
                script = [c for _, cmds in steps[:i + 1] for c in cmds]
                shutil.rmtree(data6, ignore_errors=True)
                editor(port, cfg, script, tag, {"XDG_DATA_HOME": data6})
        c1, c2, c3, c4 = load(D("cp1.json")), load(D("cp2.json")), load(D("cp3.json")), load(D("cp4.json"))
        c5, c6, c7, c8 = load(D("cp5.json")), load(D("cp6.json")), load(D("cp7.json")), load(D("cp8.json"))
        roles = [m.get("role") for m in c1.get("conv", [])]
        check("checkpoints: the chat before (user, reply, user, tool call, result, reply)",
              roles == ["user", "assistant", "user", "assistant", "tool", "assistant"] and "CUBE" in read(o1), roles)
        cps = [m.get("meta", {}).get("cp") for m in c1.get("conv", []) if m.get("role") == "user"]
        check("checkpoints: every user message records its checkpoint", len(cps) == 2 and all(c and c.get("sid") and c.get("p") for c in cps), cps)
        tm = (c1.get("conv", [{}] * 4)[3].get("meta", {}).get("tools") or {})
        check("checkpoints: the tool call counts its undo step", [t.get("undo") for t in tm.values()] == [1], tm)
        check("checkpoints: editor changes since each user message ([total, by the AI])", c1.get("changes", [])[:3] == [[1, 1], None, [1, 1]],
              c1.get("changes"))
        check("restore checkpoint: asks first", c2.get("confirm_idx") == 2 and c2.get("confirm_kind") == 1 and "Restore this checkpoint?" in c2.get("list_text", "")
              and len(c2.get("conv", [])) == 6 and "CUBE" in read(o1), c2.get("list_text", "")[-300:])
        check("restore checkpoint: the cube is undone", "CUBE" not in read(o2) and c3.get("objects") == 0, read(o2)[:200])
        check("restore checkpoint: the message and the later ones leave the chat, Redo offered",
              len(c3.get("conv", [])) == 2 and c3.get("redo_kind") == 2 and c3.get("redo_steps") == 1 and c3.get("redo_messages") == 4
              and "Checkpoint restored" in c3.get("list_text", ""), (len(c3.get("conv", [])), c3.get("redo_kind"), c3.get("list_text", "")[-200:]))
        check("restore checkpoint: the message text back in the composer", c3.get("input", "").startswith("call tool objects_create"), c3.get("input"))
        check("redo: the cube and the messages are back", "CUBE" in read(o3) and len(c4.get("conv", [])) == 6 and c4.get("redo_kind") == 0
              and c4.get("input", "") == "", (read(o3)[:100], len(c4.get("conv", [])), c4.get("input")))
        check("edit: Send asks about the later editor changes", c5.get("edit_idx") == 0 and c5.get("confirm_kind") == 2
              and "Undo the editor changes made after this message?" in c5.get("list_text", ""), c5.get("list_text", "")[-300:])
        conv = c6.get("conv", [])
        check("edit + resend: the edited message replaces the old turns and gets a reply",
              [m.get("role") for m in conv] == ["user", "assistant"] and conv[0].get("content") == "hello again"
              and conv[1].get("content") == "Hello from the stub: hello again", [(m.get("role"), m.get("content", "")[:40]) for m in conv])
        check("edit + resend (undo): the cube made after the old message is undone", "CUBE" not in read(o4), read(o4)[:200])
        check("edit + resend: the history keeps the edited chat with its checkpoint",
              c6.get("history") and c6["history"][0].get("conv", [{}])[0].get("content") == "hello again"
              and c6["history"][0]["conv"][0].get("meta", {}).get("cp"), (c6.get("history") or [{}])[0].get("title"))
        check("rewind: the chat empties, the text goes back into the composer", not c7.get("conv") and c7.get("input") == "hello again"
              and c7.get("redo_kind") == 1 and "Rewound" in c7.get("list_text", ""), (c7.get("conv"), c7.get("input")))
        check("rewind + redo: the messages are back, the composer cleared", len(c8.get("conv", [])) == 2 and c8.get("input") == "", (len(c8.get("conv", [])), c8.get("input")))
        for f in frames:
            check("frame " + f, os.path.exists(D(f + ".png")), D(f + ".png"))

        # ---- 7. the panel UX overhaul: permission levels, agent modes, the model flyout, the ring
        cfgx = D("cfg-ux")
        os.makedirs(os.path.join(cfgx, "krunknative"), exist_ok=True)
        for k in ("krunker_username", "krunker_token", "krunker_id"):
            shutil.copy(os.path.join(cfg, "krunknative", k + ".json"), os.path.join(cfgx, "krunknative", k + ".json"))
        longmsg = "please describe the map in detail " * 60  # grows the context noticeably (no ';')
        sx = ["closewindow", "wait:2", "ai:open", "wait:1.5",
              # usage ring grows with the conversation (Ask mode: plain replies, no tool rounds)
              "ai:perm:2", "ai:mode:1", "ai:send:hi", "wait:3", "ai:dump:" + D("ux-ring1.json"),
              "ai:send:" + longmsg, "wait:3", "ai:dump:" + D("ux-ring2.json"),
              # each mode exposes a different tool set
              "ai:mode:0", "ai:dump:" + D("ux-agent.json"),
              "ai:mode:2", "ai:dump:" + D("ux-plan.json"),
              "ai:mode:1", "ai:dump:" + D("ux-ask.json"),
              # the model dropdown + its per-model flyout (restore Agent, open the picker)
              "ai:mode:0", "ai:boxes:" + D("ux-boxes.json"), "ai:openmodels", "wait:0.5", "ai:hovermodel:0", "wait:0.5",
              "ai:dump:" + D("ux-models.json")]
        editor(port, cfgx, sx, "ux", {"XDG_DATA_HOME": data})
        r1, r2 = load(D("ux-ring1.json")), load(D("ux-ring2.json"))
        ag, pl, ak = load(D("ux-agent.json")), load(D("ux-plan.json")), load(D("ux-ask.json"))
        mp = load(D("ux-models.json"))
        # permissions
        check("permissions: Autopilot level set and shown as allow_all", r1.get("perm") == 2 and r1.get("allow_all") is True, (r1.get("perm"), r1.get("allow_all")))
        # agent modes change tool exposure: Agent all, Plan read-only subset, Ask none
        na, npl, nak = ag.get("tools_offered", -1), pl.get("tools_offered", -1), ak.get("tools_offered", -1)
        check("modes: Agent exposes the full tool set", na >= 50, na)
        check("modes: Plan exposes only read-only tools (fewer, more than zero)", 0 < npl < na, (npl, na))
        check("modes: Ask exposes no tools", nak == 0, nak)
        check("modes: the mode is recorded per dump", (ag.get("mode"), pl.get("mode"), ak.get("mode")) == (0, 2, 1),
              (ag.get("mode"), pl.get("mode"), ak.get("mode")))
        # the context-usage ring grows as the chat grows, against the model's real context size
        check("usage ring: the model's context size is known (>0)", r1.get("ctx_max", 0) > 0, r1.get("ctx_max"))
        check("usage ring: context usage grows with the conversation", r2.get("ctx_used", 0) > r1.get("ctx_used", 0),
              (r1.get("ctx_used"), r2.get("ctx_used")))
        # the model flyout: the dropdown opens and each model's flyout shows its reasoning level
        check("model dropdown: opens, lists models", mp.get("model_pop") and mp.get("models") == SMART_ORDER, mp.get("models"))
        fr, lr = mp.get("fly_rect"), mp.get("list_rect")
        check("model flyout: shown beside the hovered row, outside the list (not clipped)", mp.get("fly_shown") and fr and lr and fr[2] > 100
              and abs(fr[1] - lr[1]) < 40, (mp.get("fly_shown"), fr, lr))
        check("model flyout: opens on the side of the screen with room (left, panel right-docked)",
              mp.get("fly_left") is True and fr and lr and fr[0] + fr[2] <= lr[0] + 1, (mp.get("fly_left"), fr, lr))
        pt = mp.get("fly_text", "")
        check("model flyout: shows the hovered model's reasoning level (deepseek-v4-pro: low / medium / high)",
              "Reasoning" in pt and "low / medium / high" in pt and "Context" in pt, pt[:400])
        bx = load(D("ux-boxes.json"))
        k = bx["knAi"][2] / 380.0 if bx.get("knAi") else 1.0
        head = [bx.get(k2) for k2 in ("knAiTitle", "knAiModel", "knAiNewBtn", "knAiCloseBtn")]
        cy = [centre_y(r) for r in head] if all(head) else []
        check("model picker: the button is centred with the AI label and the header icons (+-0.5 px)", bool(cy) and max(cy) - min(cy) <= 0.5, cy)
        bg, best = popup_colours(D("ux.png"), bx.get("knAiModel", [0, 0, 0, 0]))
        check("model dropdown: dark list, not white (%s)" % (bg,), bg is not None and lum(bg) < 0.1, bg)
        check("model dropdown: option text readable (contrast %.1f >= 4.5)" % best, best >= 4.5, best)
        # the choice persists into a fresh editor on the same machine
        editor(port, cfgx, ["closewindow", "wait:1", "ai:open", "wait:1", "ai:dump:" + D("ux-persist.json")], "ux-persist", {"XDG_DATA_HOME": data})
        pp = load(D("ux-persist.json"))
        check("permissions + mode persist across editor sessions", pp.get("perm") == 2 and pp.get("mode") == 0, (pp.get("perm"), pp.get("mode")))
        return 0
    finally:
        if brain:
            brain.terminate()
            try:
                brain.wait(5)
            except subprocess.TimeoutExpired:
                brain.kill()
        up.stop()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)


rc = main()
if rc == 77:
    shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(77)
ok = all(results) and results
print("ai_e2e: %s (%d checks)" % ("OK" if ok else "FAILED", len(results)))
if ok and not os.environ.get("KN_AI_E2E_KEEP"):
    shutil.rmtree(WORK, ignore_errors=True)
else:
    print("kept " + WORK)  # KN_AI_E2E_KEEP=1 keeps it on success too (screenshots, logs)
sys.exit(0 if ok else 1)
