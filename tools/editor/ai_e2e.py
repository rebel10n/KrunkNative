#!/usr/bin/env python3
"""The map editor's AI panel end to end (client/src/editor/editor_ai.c, services/brain/ai.c).

  python3 -I tools/editor/ai_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server) on an ephemeral port,
talking to the stub upstream of services/brain/tests/ai_stub.py (never the live provider) and the
stub MCP for web search. Headless editors (client_bin --editor, EGL, no window on any display,
KRUNKNATIVE_EDITOR_SCRIPT):

1. guest: the panel opens, lists the models, shows "Sign in to use AI", sends nothing;
2. signed in (session restored from storage): a prompt streams a reply (text + thinking trace),
   the usage and account lines fill in, the upstream sees the system prompt and the message;
3. the model asks for editor tools: objects_create runs in this editor against the open map,
   map_save_as waits for approval and writes the map once allowed, web_search runs on the brain
   (stub MCP), render_view shows the editor's render in the card (the model gets text);
4. a second editor restores the conversation from the local history;
5. the user's own endpoint (a second stub with its own key): called directly, never via the brain.
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

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "services", "brain", "tests"))
from ai_stub import Upstream  # noqa: E402

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
    up = Upstream(KEY).start()
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
        before = len(up.chat_requests())
        editor(port, cfg_guest, ["closewindow", "ai:open", "wait:2", "ai:send:hello", "wait:1", "ai:dump:" + D("guest.json")], "guest")
        g = load(D("guest.json"))
        check("guest: the panel is shown", g.get("open") and g.get("shown"), g)
        check("guest: models listed", g.get("models", [])[:1] == ["glm-5.3-flash"], g.get("models"))
        check("guest: 'Sign in to use AI'", g.get("banner_kind") == 4 and not g.get("signed_in"), g)
        sent = [r for r in up.chat_requests()[before:] if "KN-PROBE" not in json.dumps(r["body"])]
        check("guest: nothing sent to the upstream", not sent and not g.get("conv"), g.get("conv"))
        check("guest: account line says Guest", "Guest" in g.get("account", ""), g.get("account"))

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
        check("account line shows the user", ("edai" + stamp) in c.get("account", ""), c.get("account"))
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
