#!/usr/bin/env python3
"""Named AI provider profiles on kn_brain (services/brain/ai_provider.c), against three stub upstreams
(services/brain/tests/ai_stub.py; never the live provider), each with its own key:

  editor -> profile "editor" (KN_AI_EDITOR_URL / _KEY), vision -> "vision" (moderation), fast -> "fast"
  (banter); ai.editor.provider / ai.banter.provider / ai.moderation.provider route them.

  ai_providers_e2e.py <kn_brain> <repo root>

Checks: the editor's probes, model list and a streamed chat reach the editor's upstream with the editor's
key only; the other two upstreams see no request at all; /ai/status lists the profiles and the consumers
(configured, routed) and moves its counters (requests and tokens per profile and per consumer); no key and
no URL in any answer, and no key in the brain log. The bot-banter path is covered by server/tests/
bots_chat_e2e.py (the legacy single key). 77 = skip (no websockets / database).
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
import urllib.error
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ai_stub import Upstream  # noqa: E402

DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
SCHEMA = "kn_test_aiprov_%d" % os.getpid()
PID = os.getpid()
KEY_E, KEY_V, KEY_F = "sk-editor-%d-E" % PID, "sk-vision-%d-V" % PID, "sk-fast-%d-F" % PID
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


def http(port, method, path, body=None, headers=None, timeout=20):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method,
                                 headers=dict({"Content-Type": "application/json"}, **(headers or {})))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "pass_word1", None]]))
        deadline = time.time() + 15
        while time.time() < deadline:
            f = json.loads(await asyncio.wait_for(ws.recv(), deadline - time.time()))
            if f[0] == "a":
                return f[5] if len(f) > 5 and not f[1] else None
    return None


def auth_of(req):
    for k, v in req["headers"].items():
        if k.lower() == "authorization":
            return v
    return None


def status(port):
    st, body = http(port, "GET", "/ai/status")
    return st, body, json.loads(body)


def wait_healthy(port, timeout=10):
    for _ in range(int(timeout * 10)):
        _, _, s = status(port)
        if s["health"]["state"] == "healthy":
            return True
        time.sleep(0.1)
    return False


def scenario(port, ups, log_path):
    ue, uv, uf = ups
    # ---- the editor's profile serves the editor: probes and the model list go to its upstream only
    check(wait_healthy(port), "editor: the health probe answers healthy through the editor's profile")
    check(len(ue.chat_requests()) >= 1 and all(auth_of(r) == "Bearer " + KEY_E for r in ue.requests),
          "editor upstream: every request carries the editor's key")
    check(not uv.requests and not uf.requests, "vision and fast upstreams: no request at all (probes, models)")

    st, body, s = status(port)
    check(st == 200 and KEY_E not in body and KEY_V not in body and KEY_F not in body and ue.url not in body and uv.url not in body,
          "/ai/status: no key and no upstream URL in the answer")
    p = s.get("providers", {})
    c = s.get("consumers", {})
    check(p.get("editor", {}).get("configured") is True and p.get("vision", {}).get("configured") is True
          and p.get("fast", {}).get("configured") is True and p.get("default", {}).get("configured") is False,
          "/ai/status providers: editor, vision and fast configured, default not (%s)" % sorted(p))
    check(c.get("editor", {}).get("provider") == "editor" and c.get("banter", {}).get("provider") == "fast"
          and c.get("moderation", {}).get("provider") == "vision" and c.get("banter", {}).get("configured") is True,
          "/ai/status consumers: each routed to its profile (%s)" % {k: v.get("provider") for k, v in c.items()})

    st, body = http(port, "GET", "/ai/models")
    m = json.loads(body)
    check(st == 200 and m.get("success") and [x["id"] for x in m.get("models", [])][:1] == ["glm-5.3-flash"],
          "/ai/models: the editor's list")

    # ---- a streamed chat through the editor's profile: its key, its upstream, its counters
    token = asyncio.run(register(port, "aiprov%d" % (PID % 100000)))
    check(bool(token), "registered an account (session token)")
    chat = {"model": "glm-5.3-flash", "messages": [{"role": "user", "content": "hello"}]}
    st, body = http(port, "POST", "/ai/chat", chat, {"Authorization": "Bearer " + (token or "")})
    check(st == 200 and '"Hello from the stub: hello"' in body.replace("\\", ""), "chat: answered through the editor's profile")
    creq = ue.chat_requests()[-1]
    check(auth_of(creq) == "Bearer " + KEY_E, "chat: the editor's key on the editor's upstream")
    check(not uv.requests and not uf.requests, "chat: the vision and fast upstreams still see nothing")

    st, body, s = status(port)
    p = s["providers"]
    c = s["consumers"]
    check(p["editor"]["requests_today"] >= 1 and p["editor"]["tokens_today"] == 120,
          "counters: the editor's profile counted the chat (120 tokens) (%s)" % p["editor"])
    check(c["editor"]["requests_today"] >= 1 and c["editor"]["tokens_today"] == 120, "counters: the editor consumer (%s)" % c["editor"])
    check(p["vision"]["requests_today"] == 0 and c["moderation"]["tokens_today"] == 0, "counters: vision and moderation untouched")

    log = open(log_path).read()
    check(KEY_E not in log and KEY_V not in log and KEY_F not in log, "the keys never appear in the brain log")


def main():
    scratch = [sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("ai providers e2e: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    work = tempfile.mkdtemp(prefix="kn-aiprov.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    ups = [Upstream(KEY_E).start(), Upstream(KEY_V).start(), Upstream(KEY_F).start()]
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")}
    env.update(dbenv, KN_BRAIN_INTERNAL_SECRET="kn-aiprov-test-%d" % PID,
               KN_AI_EDITOR_URL=ups[0].url + "/", KN_AI_EDITOR_KEY=KEY_E,
               KN_AI_VISION_URL=ups[1].url + "/", KN_AI_VISION_KEY=KEY_V,
               KN_AI_FAST_URL=ups[2].url + "/", KN_AI_FAST_KEY=KEY_F)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        extra = ["--ai.health_interval_s=1", "--accounts.email.required=false", "--smtp.host=",
                 "--ai.editor.provider=editor", "--ai.banter.provider=fast", "--ai.moderation.provider=vision"]
        with open(log_path, "a") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=aiprov-%d" % PID] + extra,
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        scenario(port, ups, log_path)
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
        for u in ups:
            u.stop()
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
    with open(log_path) as f:
        log = f.read()
    bad = [l.strip() for l in log.splitlines() if re.search(r"ERROR|FATAL", l)]
    check(not bad, "no errors in the brain log %s" % bad[:3])
    print("ai providers e2e: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
