#!/usr/bin/env python3
"""The map editor's AI proxy on kn_brain (services/brain/ai.c) against a stub upstream
(tests/ai_stub.py; never the live provider) and, with a search command, a stub MCP server.

  ai_test.py <kn_brain> <repo root>

Checks: /ai/status and /ai/models (normalised list, hidden ids, no secret in any answer), the
health probe (healthy, then "degraded" when the stub drops prompts, healthy again), sign-in gating
(no / bad token -> 401 SignInRequired), a streamed reply (reasoning + text deltas, finish, kn.done
with usage and cost), the key and User-Agent the upstream sees, a client tool call, a non-streamed
reply, an upstream failure, cancellation when the client goes away, server tools (web search through
the stub MCP: kn.server_tool events, the second round, an error result) and the per-account
rate limit (429 RateLimited). The key never appears in the brain log.

Database: KN_TEST_DB_DRIVER (default sqlite here: no server needed; postgres / mysql via db_scratch.py).
Ports: an ephemeral brain port and a free stub port. 77 = skip (no websockets / database).
"""
import asyncio
import json
import os
import re
import shutil
import socket
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
SCHEMA = "kn_test_ai_%d" % os.getpid()
KEY = "sk-stub-%d-SECRET" % os.getpid()
ENV_PROBE = os.path.join(os.path.expanduser("~"), ".cache", "kn-ai-env-%d.json" % os.getpid())
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
            return r.status, r.read().decode(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(), dict(e.headers)


def sse_events(text):
    """[(event, data)] of an SSE body; data parsed as JSON when it is."""
    out = []
    for block in text.split("\n\n"):
        ev, data = "", []
        for line in block.split("\n"):
            if line.startswith(":"):
                continue
            if line.startswith("event:"):
                ev = line[6:].strip()
            elif line.startswith("data:"):
                data.append(line[5:].lstrip())
        if not data:
            continue
        d = "\n".join(data)
        try:
            d = json.loads(d)
        except ValueError:
            pass
        out.append((ev, d))
    return out


def stream_summary(events):
    text, reasoning, calls, finish, done, errors, kn = "", "", [], None, None, [], []
    for ev, d in events:
        if ev == "kn.done":
            done = d
        elif ev.startswith("kn."):
            kn.append((ev, d))
        elif ev == "error":
            errors.append(d)
        elif isinstance(d, dict) and d.get("choices"):
            ch = d["choices"][0]
            delta = ch.get("delta", {})
            text += delta.get("content") or ""
            reasoning += delta.get("reasoning_content") or ""
            calls += delta.get("tool_calls") or []
            if ch.get("finish_reason"):
                finish = ch["finish_reason"]
    return {"text": text, "reasoning": reasoning, "calls": calls, "finish": finish, "done": done, "errors": errors, "kn": kn}


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "pass_word1", None]]))
        deadline = time.time() + 15
        while time.time() < deadline:
            f = json.loads(await asyncio.wait_for(ws.recv(), deadline - time.time()))
            if f[0] == "a":
                return f[5] if len(f) > 5 and not f[1] else None
    return None


def wait_health(port, state, timeout=10):
    h = None
    for _ in range(int(timeout * 10)):
        _, body, _ = http(port, "GET", "/ai/status")
        h = json.loads(body)["health"]
        if h["state"] == state:
            return h
        time.sleep(0.1)
    return h


def scenario(port, up, log_path):
    # ---- status + models (public, no secrets)
    st, body, _ = http(port, "GET", "/ai/status")
    s = json.loads(body)
    check(st == 200 and s["configured"] and s["enabled"], "/ai/status: configured")
    check(KEY not in body and up.url not in body, "/ai/status: no key or upstream URL in the answer")
    h = wait_health(port, "healthy")
    check(h["state"] == "healthy" and h.get("model") == "glm-5.3-flash", "health probe: healthy (%s)" % h)
    st, body, _ = http(port, "GET", "/ai/models")
    m = json.loads(body)
    ids = [x["id"] for x in m.get("models", [])]
    check(st == 200 and m["success"] and ids == ["glm-5.3-flash", "gpt-oss-20b", "llama-3.1-8b"], "/ai/models: sorted, hidden-model left out (%s)" % ids)
    glm = m["models"][0]
    check(glm.get("tools") is True and glm.get("reasoning") is True and glm.get("vision") is True and glm.get("price_in") == 0.15
          and glm.get("context") == 131072 and glm.get("max_tokens") == 4096, "/ai/models: normalised fields %s" % glm)
    check(m["default"] == "glm-5.3-flash" and "limits" in m and "health" in m, "/ai/models: default, limits, health")
    check(m["models"][1].get("efforts") == ["low", "medium", "high"], "/ai/models: reasoning efforts")
    check(KEY not in body, "/ai/models: no key in the answer")
    st, body, _ = http(port, "GET", "/api/ai/models")
    check(st == 200 and json.loads(body)["success"], "/api/ai/models alias")

    # ---- sign-in gating
    chat = {"model": "glm-5.3-flash", "messages": [{"role": "user", "content": "hello"}]}
    st, body, _ = http(port, "POST", "/ai/chat", chat)
    check(st == 401 and json.loads(body)["error"] == "SignInRequired" and "Sign in" in json.loads(body)["message"],
          "no token: 401 SignInRequired (%d %s)" % (st, body[:120]))
    st, body, _ = http(port, "POST", "/ai/chat", chat, {"Authorization": "Bearer not-a-session"})
    check(st == 401 and json.loads(body)["error"] == "SignInRequired", "bad token: 401 SignInRequired")
    before = len(up.chat_requests())
    check(before == 1, "nothing reached the upstream without a session (only the probe: %d)" % before)

    token = asyncio.run(register(port, "aitest%d" % (os.getpid() % 100000)))
    check(bool(token), "registered an account (session token)")
    auth = {"Authorization": "Bearer " + (token or "")}

    # ---- streamed reply
    st, body, hdr = http(port, "POST", "/ai/chat", dict(chat, messages=[{"role": "system", "content": "You edit maps."},
                                                                       {"role": "user", "content": "hello"}]), auth)
    check(st == 200 and hdr.get("Content-Type", "").startswith("text/event-stream"), "stream: 200 text/event-stream")
    check(hdr.get("X-Accel-Buffering") == "no", "stream: X-Accel-Buffering: no")
    r = stream_summary(sse_events(body))
    check(r["text"] == "Hello from the stub: hello", "stream: text deltas (%r)" % r["text"])
    check(r["reasoning"] == "Thinking about it.", "stream: reasoning deltas (%r)" % r["reasoning"])
    check(r["finish"] == "stop", "stream: finish stop")
    d = r["done"] or {}
    check(d.get("usage", {}).get("prompt_tokens") == 100 and d.get("usage", {}).get("completion_tokens") == 20, "kn.done usage %s" % d.get("usage"))
    check(abs(d.get("cost_usd", -1) - (100 * 0.15 + 20 * 0.5) / 1e6) < 1e-12, "kn.done cost_usd %s" % d.get("cost_usd"))
    msgs = d.get("messages", [])
    check(len(msgs) == 1 and msgs[0]["role"] == "assistant" and msgs[0]["content"] == r["text"]
          and msgs[0].get("reasoning_content") == "Thinking about it.", "kn.done messages: the assistant message")
    check(d.get("account", {}).get("requests_today") == 1, "kn.done account usage %s" % d.get("account"))
    check(body.endswith("data: [DONE]\n\n"), "stream ends with [DONE]")
    meta = [x for x in r["kn"] if x[0] == "kn.meta"]
    check(meta and meta[0][1]["model"] == "glm-5.3-flash" and meta[0][1]["account"].startswith("aitest"), "kn.meta: model + account")
    req = up.chat_requests()[-1]
    hl = {k.lower(): v for k, v in req["headers"].items()}
    check(hl.get("authorization") == "Bearer " + KEY, "upstream: the brain's key (server-side injection)")
    check(hl.get("user-agent", "").startswith("KrunkNative-brain/"), "upstream: KrunkNative User-Agent (%s)" % hl.get("user-agent"))
    check(req["body"].get("stream") is True and req["body"]["messages"][0]["role"] == "system"
          and req["body"].get("max_tokens") == 4096, "upstream: streamed request with the conversation")
    check("token" not in json.dumps(req["body"]) or token not in json.dumps(req["body"]), "upstream: the session token is not forwarded")

    # ---- a client tool call (the editor's tools pass through)
    tools = [{"type": "function", "function": {"name": "objects_create", "description": "create objects",
                                               "parameters": {"type": "object", "properties": {"objects": {"type": "array"}}}}}]
    args = '{"objects":[{"prefab":"CUBE","p":[0,0,0]}]}'
    st, body, _ = http(port, "POST", "/ai/chat", {"model": "gpt-oss-20b", "effort": "high", "tools": tools,
                                                  "messages": [{"role": "user", "content": "call tool objects_create " + args}]}, auth)
    r = stream_summary(sse_events(body))
    check(r["finish"] == "tool_calls" and len(r["calls"]) == 1 and r["calls"][0]["function"]["name"] == "objects_create"
          and r["calls"][0]["function"]["arguments"] == args, "client tool call relayed whole (%s)" % r["calls"])
    starts = [x for x in r["kn"] if x[0] == "kn.tool_start"]
    check(starts and starts[0][1] == {"name": "objects_create", "server": False}, "kn.tool_start for the client tool")
    ub = up.chat_requests()[-1]["body"]
    check(ub.get("tools") == tools and ub.get("reasoning_effort") == "high", "upstream: tools + reasoning_effort")
    # the tool result goes back (the editor's next request)
    conv = [{"role": "user", "content": "call tool objects_create " + args}] + (r["done"] or {}).get("messages", [])
    conv.append({"role": "tool", "tool_call_id": "call_stub1", "content": "created 1 object (id 7)"})
    st, body, _ = http(port, "POST", "/ai/chat", {"model": "gpt-oss-20b", "tools": tools, "messages": conv}, auth)
    r = stream_summary(sse_events(body))
    check(r["text"] == "Tool said: created 1 object (id 7)", "the tool result reaches the model (%r)" % r["text"])

    # ---- non-streamed
    st, body, _ = http(port, "POST", "/ai/chat", dict(chat, stream=False), auth)
    j = json.loads(body)
    check(st == 200 and j.get("success") and j.get("message", {}).get("content") == "Hello from the stub: hello", "stream:false JSON answer")

    # ---- unknown model, upstream failure
    st, body, _ = http(port, "POST", "/ai/chat", dict(chat, model="no-such-model"), auth)
    check(st == 400 and json.loads(body)["error"] == "UnknownModel", "unknown model: 400")
    st, body, _ = http(port, "POST", "/ai/chat", dict(chat, messages=[{"role": "user", "content": "fail"}]), auth)
    r = stream_summary(sse_events(body))
    check(r["errors"] and r["errors"][0]["error"]["code"] == "UpstreamError" and "stub failure" in r["errors"][0]["error"]["message"],
          "upstream failure: error event (%s)" % r["errors"])
    check((r["done"] or {}).get("finish") == "error", "upstream failure: kn.done finish error")

    # ---- the client goes away: the upstream transfer is cancelled
    s = socket.create_connection(("127.0.0.1", port))
    b = json.dumps(dict(chat, messages=[{"role": "user", "content": "slow"}])).encode()
    s.sendall(b"POST /ai/chat HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n"
              % (token.encode(), len(b)) + b)
    got = b""
    while b"tick 1" not in got:
        got += s.recv(4096)
    s.close()
    check(up.closed_early.wait(8), "client gone: the upstream connection is closed early")

    # ---- server tools (web search through the stub MCP)
    st, body, _ = http(port, "GET", "/ai/status")
    stools = json.loads(body).get("server_tools", [])
    if stools:
        check(stools == ["web_search", "web_fetch"], "server tools listed %s" % stools)
        st, body, _ = http(port, "POST", "/ai/chat", dict(chat, web_search=True,
                                                          messages=[{"role": "user", "content": 'call tool web_search {"query":"krunker maps"}'}]), auth)
        r = stream_summary(sse_events(body))
        sv = [d for e, d in r["kn"] if e == "kn.server_tool"]
        check(len(sv) == 2 and sv[0]["status"] == "running" and sv[1]["status"] == "done" and "Stub result for krunker maps" in sv[1]["result"],
              "kn.server_tool running + done (%s)" % sv)
        check(r["text"].startswith("Tool said: Found 1 search results"), "second round answers with the search result (%r)" % r["text"][:60])
        d = r["done"] or {}
        check(d.get("rounds") == 2 and [m["role"] for m in d.get("messages", [])] == ["assistant", "tool", "assistant"],
              "kn.done: 2 rounds, messages assistant/tool/assistant")
        ub = up.chat_requests()[-2]["body"]
        names = [t["function"]["name"] for t in ub.get("tools", [])]
        check("web_search" in names and "web_fetch" in names, "upstream sees the web tools %s" % names)
        st, body, _ = http(port, "POST", "/ai/chat", dict(chat, web_search=True,
                                                          messages=[{"role": "user", "content": 'call tool web_search {"query":"boom"}'}]), auth)
        r = stream_summary(sse_events(body))
        sv = [d for e, d in r["kn"] if e == "kn.server_tool"]
        check(sv and sv[-1]["status"] == "error" and r["text"].startswith("Tool said: Error: search failed"), "server tool error reaches the model")
        st, body, _ = http(port, "POST", "/ai/chat", dict(chat, messages=[{"role": "user", "content": "hello"}]), auth)
        ub = up.chat_requests()[-1]["body"]
        check("tools" not in ub, "web tools only when web_search is asked for")
        probe = json.load(open(ENV_PROBE))
        check(not [k for k in probe["env"] if k.startswith("KN_AI_") or k.startswith("KN_DATABASE") or k.startswith("KN_BRAIN")],
              "the search child gets none of the brain's secrets (%s)" % probe["env"])
        os.kill(probe["pid"], 9)
        ok = False
        for _ in range(60):  # restarted after a 2 s backoff
            time.sleep(0.25)
            try:
                if json.load(open(ENV_PROBE))["pid"] != probe["pid"]:
                    st, body, _ = http(port, "POST", "/ai/chat", dict(chat, web_search=True,
                                       messages=[{"role": "user", "content": 'call tool web_search {"query":"again"}'}]), auth)
                    ok = "Stub result for again" in body
                    if ok:
                        break
            except (OSError, ValueError):
                pass
        check(ok, "the search child is restarted after it dies")

    # ---- degraded upstream (drops request content), then healthy again
    up.drop = True
    h = wait_health(port, "degraded", 10)
    check(h["state"] == "degraded" and "known-prompt" in h["reason"], "health: degraded when prompts are dropped (%s)" % h)
    st, body, _ = http(port, "GET", "/ai/models")
    check(json.loads(body)["health"]["state"] == "degraded", "/ai/models carries the degraded state")
    up.drop = False
    h = wait_health(port, "healthy", 10)
    check(h["state"] == "healthy", "health: healthy again")

    # ---- a model the provider blocks: marked unavailable, the default moves on, a readable error
    up.blocked.add("glm-5.3-flash")
    h = None
    for _ in range(100):
        st, body, _ = http(port, "GET", "/ai/models")
        m = json.loads(body)
        rows = {r["id"]: r for r in m.get("models", [])}
        if rows.get("glm-5.3-flash", {}).get("status") == "unavailable" and m.get("default") == "gpt-oss-20b":
            h = m
            break
        time.sleep(0.1)
    check(h is not None, "blocked model: status unavailable, default moves to gpt-oss-20b (%s)" % (m.get("default"), ))
    if h:
        glm = {r["id"]: r for r in h["models"]}["glm-5.3-flash"]
        check("Blocked for: code interpreter abuse" in glm.get("status_reason", "") and h.get("preferred") == "glm-5.3-flash",
              "the reason reaches the picker (%s)" % glm)
        check({r["id"]: r for r in h["models"]}["gpt-oss-20b"].get("status") == "ok", "the new default is probe-confirmed")
    st, body, _ = http(port, "POST", "/ai/chat", chat, auth)
    r = stream_summary(sse_events(body))
    check(r["errors"] and r["errors"][0]["error"]["code"] == "ModelUnavailable"
          and r["errors"][0]["error"]["message"] == "This model is unavailable: Blocked for: code interpreter abuse",
          "a chat with the blocked model: ModelUnavailable, readable (%s)" % r["errors"])
    st, body, _ = http(port, "POST", "/ai/chat", dict(chat, model=""), auth)
    r = stream_summary(sse_events(body))
    check(r["text"] == "Hello from the stub: hello" and (r["done"] or {}).get("model") == "gpt-oss-20b", "no model given: the effective default answers")
    up.blocked.clear()

    # ---- per-account rate limit (ai.limit.rpm = 12 in this test)
    codes = []
    for _ in range(14):
        st, body, hdr = http(port, "POST", "/ai/chat", dict(chat, stream=False), auth)
        codes.append(st)
        if st == 429:
            j = json.loads(body)
            check(j["error"] == "RateLimited" and j["retry_after_s"] > 0 and hdr.get("Retry-After"), "429 RateLimited with Retry-After")
            break
    check(429 in codes, "the per-minute limit applies (%s)" % codes)


def main():
    scratch = [sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
    if r.returncode:
        print("ai test: %s unavailable: skipped" % DRIVER)
        sys.exit(r.returncode)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    work = tempfile.mkdtemp(prefix="kn-ai.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    up = Upstream(KEY).start()
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")}
    env.update(dbenv, KN_BRAIN_INTERNAL_SECRET="kn-ai-test-%d" % os.getpid(), KN_AI_UPSTREAM_URL=up.url + "/", KN_AI_UPSTREAM_KEY=KEY)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        extra = ["--ai.health_interval_s=1", "--ai.limit.rpm=12", "--ai.models_hide=hidden-model", "--accounts.email.required=false",
                 "--smtp.host=", "--ai.search.command=%s -I %s mcp" % (sys.executable, os.path.join(HERE, "ai_stub.py")),
                 "--ai.search.env=KN_STUB_ENV_FILE=" + ENV_PROBE]
        with open(log_path, "a") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=ai-%d" % os.getpid()] + extra,
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        scenario(port, up, log_path)
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
        up.stop()
        subprocess.run(scratch + ["drop", DRIVER, SCHEMA], capture_output=True)
        if os.path.exists(ENV_PROBE):
            os.unlink(ENV_PROBE)
    with open(log_path) as f:
        log = f.read()
    check(KEY not in log, "the upstream key never appears in the brain log")
    bad = [l.strip() for l in log.splitlines() if re.search(r"ERROR|FATAL", l)]
    check(not bad, "no errors in the brain log %s" % bad[:3])
    print("ai test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
