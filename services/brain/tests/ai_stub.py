#!/usr/bin/env python3
"""Stub AI upstream for the editor-AI tests (services/brain/tests/ai_test.py, tools/editor/ai_e2e.py).

An OpenAI-compatible server on 127.0.0.1 (never the live provider):
  GET  /v1/models              31-model shape of the real upstream (toolCalling, pricing, reasoning)
  POST /v1/chat/completions    streamed (SSE) or JSON answers chosen by the last user message:
       "KN-PROBE-<n>"          echoes the token (the brain's health probe); with drop=True it answers
                               like the broken provider ("the <user_request> block is empty")
       "call tool <name> <json>"  a tool call of <name> with the JSON arguments (streamed in pieces)
       "slow"                  one chunk every 0.3 s for 30 s (cancel tests)
       "fail"                  HTTP 500 {"error":{"message":"stub failure"}}
       anything else           reasoning "Thinking about it." + "Hello from the stub: <prompt>"
       a conversation ending with a tool result answers "Tool said: <result>"
Every request is recorded (headers, body) for the test to check.

Also a stub MCP server (stdio JSON-RPC, `python3 -I ai_stub.py mcp`) with the tools of
duckduckgo-mcp-server (search, fetch_content) answering canned text, never the web.
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEY = None

MODELS = [
    {"id": "glm-5.3-flash", "display_name": "GLM 5.3 Flash", "max_input_tokens": 131072, "max_tokens": 4096,
     "reasoning": True, "toolCalling": True, "vision": True, "type": "model", "provider": "stub",
     "pricing": {"currency": "USD", "input_per_mtok": 0.15, "output_per_mtok": 0.5}},
    {"id": "gpt-oss-20b", "display_name": "GPT OSS 20B", "max_input_tokens": 128000, "max_tokens": 4096,
     "reasoning": True, "toolCalling": True, "vision": False, "supported_reasoning_efforts": ["low", "medium", "high"],
     "pricing": {"currency": "USD", "input_per_mtok": 0.2, "output_per_mtok": 0.3}},
    {"id": "llama-3.1-8b", "display_name": "Llama 3.1 8B", "max_input_tokens": 32000, "max_tokens": 4096,
     "reasoning": False, "toolCalling": False, "vision": False, "pricing": None},
    {"id": "hidden-model", "display_name": "Hidden", "max_tokens": 1024},
]


class Upstream:
    def __init__(self, key, port=0):
        self.key = key
        self.requests = []
        self.drop = False
        self.closed_early = threading.Event()
        self.lock = threading.Lock()
        stub = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _json(self, code, obj):
                b = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def _authed(self):
                if self.headers.get("Authorization") != "Bearer " + stub.key:
                    self._json(401, {"error": {"message": "bad key"}})
                    return False
                return True

            def do_GET(self):
                with stub.lock:
                    stub.requests.append({"path": self.path, "headers": dict(self.headers), "body": None})
                if not self._authed():
                    return
                if self.path.endswith("/models"):
                    self._json(200, {"object": "list", "data": MODELS})
                else:
                    self._json(404, {"error": {"message": "no such route"}})

            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(n) or b"{}")
                with stub.lock:
                    stub.requests.append({"path": self.path, "headers": dict(self.headers), "body": body})
                if not self._authed():
                    return
                if not self.path.endswith("/chat/completions"):
                    self._json(404, {"error": {"message": "no such route"}})
                    return
                stub.answer(self, body)

        self.server = ThreadingHTTPServer(("127.0.0.1", port), H)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.url = "http://127.0.0.1:%d/v1" % self.port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.server.shutdown()
        self.server.server_close()

    def chat_requests(self):
        with self.lock:
            return [r for r in self.requests if r["path"].endswith("/chat/completions")]

    # ---- answers
    def plan(self, body):
        msgs = body.get("messages", [])
        last = msgs[-1] if msgs else {}
        if last.get("role") == "tool":
            return ("text", None, "Tool said: " + str(last.get("content", ""))[:400])
        prompt = ""
        for m in reversed(msgs):
            if m.get("role") == "user":
                c = m.get("content")
                prompt = c if isinstance(c, str) else " ".join(p.get("text", "") for p in c or [] if isinstance(p, dict))
                break
        if "KN-PROBE-" in prompt:
            tok = prompt[prompt.index("KN-PROBE-"):].split()[0]
            if self.drop:
                return ("text", None, "I don't see a <user_request> block with actual content in your message.")
            return ("text", None, tok)
        if prompt.startswith("call tool "):
            rest = prompt[len("call tool "):]
            name, _, args = rest.partition(" ")
            return ("tool", name, args or "{}")
        if prompt == "slow":
            return ("slow", None, None)
        if prompt == "fail":
            return ("fail", None, None)
        return ("text", "Thinking about it.", "Hello from the stub: " + prompt)

    def answer(self, h, body):
        kind, a, b = self.plan(body)
        model = body.get("model", "glm-5.3-flash")
        if kind == "fail":
            h._json(500, {"error": {"message": "stub failure"}})
            return
        usage = {"prompt_tokens": 100, "completion_tokens": 20, "total_tokens": 120,
                 "completion_tokens_details": {"reasoning_tokens": 5}}
        if not body.get("stream"):
            msg = {"role": "assistant", "content": b if kind == "text" else None}
            if kind == "text" and a:
                msg["reasoning_content"] = a
            fr = "stop"
            if kind == "tool":
                msg["tool_calls"] = [{"id": "call_stub1", "type": "function", "function": {"name": a, "arguments": b}}]
                fr = "tool_calls"
            h._json(200, {"id": "chatcmpl-stub", "object": "chat.completion", "model": model,
                          "choices": [{"index": 0, "message": msg, "finish_reason": fr}], "usage": usage})
            return
        h.send_response(200)
        h.send_header("Content-Type", "text/event-stream")
        h.send_header("Cache-Control", "no-cache")
        h.send_header("Connection", "close")
        h.end_headers()
        h.close_connection = True

        def chunk(delta, finish=None, extra=None):
            o = {"id": "chatcmpl-stub", "object": "chat.completion.chunk", "model": model,
                 "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
            if extra:
                o.update(extra)
            h.wfile.write(("data: %s\n\n" % json.dumps(o)).encode())
            h.wfile.flush()

        try:
            chunk({"role": "assistant", "content": ""})
            if kind == "slow":
                for i in range(100):
                    chunk({"content": "tick %d " % i})
                    time.sleep(0.3)
                chunk({}, "stop")
            elif kind == "tool":
                chunk({"tool_calls": [{"index": 0, "id": "call_stub1", "type": "function", "function": {"name": a, "arguments": ""}}]})
                half = len(b) // 2
                chunk({"tool_calls": [{"index": 0, "function": {"arguments": b[:half]}}]})
                chunk({"tool_calls": [{"index": 0, "function": {"arguments": b[half:]}}]})
                chunk({}, "tool_calls")
            else:
                if a:
                    chunk({"reasoning_content": a})
                for i in range(0, len(b), 7):
                    chunk({"content": b[i:i + 7]})
                chunk({}, "stop")
            chunk({}, None, {"choices": [], "usage": usage})
            h.wfile.write(b"data: [DONE]\n\n")
            h.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            self.closed_early.set()


# ---------------------------------------------------------------- stub MCP (stdio)

MCP_TOOLS = [
    {"name": "search", "description": "Search DuckDuckGo and return formatted results.",
     "inputSchema": {"type": "object", "properties": {"query": {"type": "string"}, "max_results": {"type": "integer", "default": 10}},
                     "required": ["query"]}},
    {"name": "fetch_content", "description": "Fetch and parse content from a webpage URL.",
     "inputSchema": {"type": "object", "properties": {"url": {"type": "string"}}, "required": ["url"]}},
]


def mcp_main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        mid = msg.get("id")
        method = msg.get("method")
        if mid is None:
            continue
        if method == "initialize":
            res = {"protocolVersion": msg["params"].get("protocolVersion", "2025-06-18"), "capabilities": {"tools": {}},
                   "serverInfo": {"name": "stub-ddg", "version": "0.0.1"}}
        elif method == "tools/list":
            res = {"tools": MCP_TOOLS}
        elif method == "tools/call":
            p = msg.get("params", {})
            args = p.get("arguments", {})
            if p.get("name") == "search":
                q = args.get("query", "")
                if q == "boom":
                    res = {"content": [{"type": "text", "text": "search failed: boom"}], "isError": True}
                else:
                    res = {"content": [{"type": "text", "text": "Found 1 search results:\n\n1. Stub result for %s\n   URL: https://example.org/%s\n" % (q, q.replace(" ", "-"))}]}
            elif p.get("name") == "fetch_content":
                res = {"content": [{"type": "text", "text": "Stub page text of " + args.get("url", "")}]}
            else:
                sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool"}}) + "\n")
                sys.stdout.flush()
                continue
        elif method == "ping":
            res = {}
        else:
            sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "no method"}}) + "\n")
            sys.stdout.flush()
            continue
        sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid, "result": res}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "mcp":
        mcp_main()
    else:
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        up = Upstream(sys.argv[1] if len(sys.argv) > 1 else "stub-key", port).start()
        print(up.url, flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            up.stop()
