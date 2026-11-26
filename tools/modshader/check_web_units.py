#!/usr/bin/env python3
"""Links the game's shader pairs as the web client builds them (GLSL ES 3.00 with client/src/shader.c's header, defines
and `#include`s) in headless Chromium's WebGL2 and fails when a program has more active samplers than WebGL2 on players'
GPUs allows.

Chrome reports MAX_TEXTURE_IMAGE_UNITS 16 on most GPUs, while SwiftShader (the headless e2es) allows 32, so a 17th
sampler links in every test and fails only in players' browsers. On 2026-11-26 basic.frag's Krunker variant (KN_RT 0)
had 17 and krunk.io stopped on a white screen after Click to Play ("FRAGMENT shader texture image units count exceeds
MAX_TEXTURE_IMAGE_UNITS(16)"). The count has to come from ANGLE, the translator Chrome uses everywhere: it counts every
sampler that reachable code names. Mesa's llvmpipe drops the ones behind dead branches and reported 14 for that shader.

    tools/modshader/check_web_units.py <repo root>

Checked: basic.vert / basic.frag with the define set of client/src/materials/basic.c that the web client draws with
(variant 0; the ray-traced variants 1 and 2 are opt-in and fall back to it), and every other assets/shaders/<name>.frag
that has a <name>.vert. A pair that does not link is listed, not failed (tools/modshader/check_gl.py owns compile
errors). Exit 77 (skip) without Chromium (chromium on PATH, e.g. the test venv's link) or the websockets module.
"""
import asyncio
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

WEBGL_UNITS = 16
ES_HEADER = "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2D;\n"
BASIC_WEB_VARIANTS = {"basic variant 0": "#define KN_RT 0\n#define KN_RT_CINE 0\n"}

PAGE_JS = r"""(() => {
  const gl = document.createElement('canvas').getContext('webgl2');
  if (!gl) return JSON.stringify({error: 'no webgl2'});
  const kinds = new Set([gl.SAMPLER_2D, gl.SAMPLER_3D, gl.SAMPLER_CUBE, gl.SAMPLER_2D_SHADOW, gl.SAMPLER_2D_ARRAY,
    gl.SAMPLER_2D_ARRAY_SHADOW, gl.SAMPLER_CUBE_SHADOW, gl.INT_SAMPLER_2D, gl.INT_SAMPLER_3D, gl.INT_SAMPLER_CUBE,
    gl.INT_SAMPLER_2D_ARRAY, gl.UNSIGNED_INT_SAMPLER_2D, gl.UNSIGNED_INT_SAMPLER_3D, gl.UNSIGNED_INT_SAMPLER_CUBE,
    gl.UNSIGNED_INT_SAMPLER_2D_ARRAY]);
  const shader = (type, text) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, text);
    gl.compileShader(s);
    if (gl.getShaderParameter(s, gl.COMPILE_STATUS)) return [s, ''];
    return [null, gl.getShaderInfoLog(s) || 'compile failed'];
  };
  return JSON.stringify(PAIRS.map(([label, vs, fs]) => {
    const [v, vlog] = shader(gl.VERTEX_SHADER, vs), [f, flog] = shader(gl.FRAGMENT_SHADER, fs);
    if (!v || !f) return {label, error: (vlog || flog).trim().split('\n')[0]};
    const p = gl.createProgram();
    gl.attachShader(p, v);
    gl.attachShader(p, f);
    gl.linkProgram(p);
    const log = gl.getProgramInfoLog(p) || '';
    // a link refused for the unit count is the failure this check exists for: report it with its log
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) return {label, linkError: log.trim().slice(0, 300)};
    const samplers = [];
    for (let i = 0, n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS); i < n; i++) {
      const u = gl.getActiveUniform(p, i);
      if (kinds.has(u.type)) for (let k = 0; k < u.size; k++) samplers.push(u.name);
    }
    return {label, samplers};
  }));
})()"""


def skip(msg):
    print("check_web_units: " + msg + ", skipped")
    sys.exit(77)


def source(root, name, defines):
    text = open(os.path.join(root, "assets/shaders", name), encoding="utf-8").read()
    text = re.sub(r'^#include "([^"]+)".*$',
                  lambda m: open(os.path.join(root, "assets/shaders", m.group(1)), encoding="utf-8").read(), text,
                  flags=re.M)
    first, _, rest = text.partition("\n")
    if not first.startswith("#version"):
        return None
    return ES_HEADER + defines + rest


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_in_chromium(chrome, pairs):
    import websockets
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="kn-web-units.") as profile:
        proc = subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                                 "--enable-unsafe-swiftshader", "--mute-audio", f"--remote-debugging-port={port}",
                                 f"--user-data-dir={profile}", "about:blank"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            deadline = time.time() + 30
            tabs = None
            while tabs is None:
                try:
                    tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=2))
                except OSError:
                    if time.time() > deadline or proc.poll() is not None:
                        skip("Chromium did not start")
                    time.sleep(0.1)
            url = next(t["webSocketDebuggerUrl"] for t in tabs if t["type"] == "page")
            expr = PAGE_JS.replace("PAIRS", json.dumps(pairs))

            async def evaluate():
                async with websockets.connect(url, max_size=1 << 28) as ws:
                    await ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                                              "params": {"expression": expr, "returnByValue": True}}))
                    while True:
                        m = json.loads(await ws.recv())
                        if m.get("id") == 1:
                            return m["result"]["result"]["value"]

            return json.loads(asyncio.run(asyncio.wait_for(evaluate(), 120)))
        finally:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=10)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    chrome = shutil.which("chromium") or shutil.which("chromium-browser") or shutil.which("google-chrome")
    if not chrome:
        skip("no chromium on PATH")
    try:
        import websockets  # noqa: F401
    except ImportError:
        skip("no websockets module")
    pairs = [[label, source(root, "basic.vert", d), source(root, "basic.frag", d)] for label, d in BASIC_WEB_VARIANTS.items()]
    shaders = os.path.join(root, "assets/shaders")
    for frag in sorted(os.listdir(shaders)):
        stem = frag[:-5]
        if frag.endswith(".frag") and stem != "basic" and os.path.isfile(os.path.join(shaders, stem + ".vert")):
            pairs.append([stem, source(root, stem + ".vert", ""), source(root, frag, "")])
    pairs = [p for p in pairs if p[1] and p[2]]
    results = run_in_chromium(chrome, pairs)
    if isinstance(results, dict):
        skip(results.get("error", "no result"))
    fails = checked = 0
    for r in results:
        label = r["label"]
        if "error" in r:
            print("  %-28s not compiled here (%s)" % (label, r["error"]))
            continue
        checked += 1
        if "linkError" in r:
            fails += 1
            print("FAIL %-28s does not link: %s" % (label, r["linkError"]))
        elif len(r["samplers"]) > WEBGL_UNITS:
            fails += 1
            print("FAIL %-28s %d active samplers, WebGL2 on most GPUs allows %d: %s"
                  % (label, len(r["samplers"]), WEBGL_UNITS, ", ".join(r["samplers"])))
        elif len(r["samplers"]) > WEBGL_UNITS - 3:
            print("  %-28s %d active samplers (limit %d)" % (label, len(r["samplers"]), WEBGL_UNITS))
    print("check_web_units: %d programs, %d failed" % (checked, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
