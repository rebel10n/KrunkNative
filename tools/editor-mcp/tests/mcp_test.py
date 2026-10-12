#!/usr/bin/env python3
"""kn_editor_mcp test suite: drives the server over MCP stdio (JSON-RPC 2.0, one message per line)
and covers every tool.

    python3 -I tools/editor-mcp/tests/mcp_test.py [--bin PATH] [--root DIR] [--no-render] [-k NAME]

Defaults: --bin <root>/cmake-build-debug/tools/editor-mcp/kn_editor_mcp, --root = the checkout.
Renders and the playtest screenshot need client_bin (headless EGL) and the gitignored game content
(assets/css/fonts/...); they are skipped with a note when either is missing (--no-render skips
them always). Exit status 0 = all passed.
"""

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


class Mcp:
    def __init__(self, binary, root, workspace, extra=()):
        args = [binary, "--root", root, "--workspace", workspace, "--allow", os.path.dirname(workspace), "--no-live", "--quiet"]
        args += list(extra)
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.next_id = 1
        self.notifications = []

    def send_raw(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def read(self):
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout (stderr: %s)" % self.p.stderr.read()[-2000:])
        return json.loads(line)

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send_raw(json.dumps(msg))
        while True:
            m = self.read()
            if m.get("id") == rid:
                return m
            self.notifications.append(m)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.send_raw(json.dumps(msg))

    def initialize(self, version="2025-06-18"):
        r = self.request("initialize", {"protocolVersion": version, "capabilities": {}, "clientInfo": {"name": "mcp_test", "version": "1"}})
        self.notify("notifications/initialized")
        return r

    def call(self, tool, args=None, ok=True):
        r = self.request("tools/call", {"name": tool, "arguments": args or {}})
        if "error" in r:
            raise AssertionError("%s: JSON-RPC error %s" % (tool, r["error"]))
        res = r["result"]
        if ok and res.get("isError"):
            raise AssertionError("%s failed: %s" % (tool, text_of(res)))
        if not ok and not res.get("isError"):
            raise AssertionError("%s should have failed: %s" % (tool, text_of(res)[:300]))
        return res

    def sc(self, tool, args=None):
        return self.call(tool, args)["structuredContent"]

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:
            self.p.kill()


def text_of(res):
    return " | ".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")


def images_of(res):
    return [c for c in res.get("content", []) if c.get("type") == "image"]


def is_png(b64):
    return base64.b64decode(b64)[:8] == b"\x89PNG\r\n\x1a\n"


class Ctx:
    pass


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


# ---------------------------------------------------------------- protocol

@test
def protocol(c):
    m = c.mcp
    r = m.initialize()
    check(r["result"]["protocolVersion"] == "2025-06-18", "negotiates 2025-06-18")
    caps = r["result"]["capabilities"]
    for k in ("tools", "resources", "prompts", "logging"):
        check(k in caps, "capability " + k)
    check(m.request("ping")["result"] == {}, "ping")
    tools = m.request("tools/list")["result"]["tools"]
    names = {t["name"] for t in tools}
    c.tool_names = names
    check(len(tools) >= 50, "50+ tools (%d)" % len(tools))
    for t in tools:
        check(t["inputSchema"].get("type") == "object", "%s schema is an object" % t["name"])
        check("annotations" in t and "readOnlyHint" in t["annotations"], "%s annotations" % t["name"])
    r = m.request("nope/nope")
    check(r["error"]["code"] == -32601, "unknown method -32601")
    r = m.request("tools/call", {"name": "no_such_tool", "arguments": {}})
    check(r["error"]["code"] == -32602, "unknown tool -32602")
    m.send_raw("{not json")
    check(m.read()["error"]["code"] == -32700, "parse error -32700")
    # batch
    m.send_raw(json.dumps([{"jsonrpc": "2.0", "id": 900, "method": "ping"}, {"jsonrpc": "2.0", "method": "notifications/x"}]))
    b = m.read()
    check(isinstance(b, list) and len(b) == 1 and b[0]["id"] == 900, "batch answers requests only")
    check(m.request("logging/setLevel", {"level": "error"})["result"] == {}, "logging/setLevel")
    # resources
    res = m.request("resources/list")["result"]["resources"]
    uris = {x["uri"] for x in res}
    for u in ("krunker://prefabs", "krunker://textures", "krunker://catalog", "krunker://map/current", "krunker://triggers"):
        check(u in uris, "resource " + u)
    pre = json.loads(m.request("resources/read", {"uri": "krunker://prefabs"})["result"]["contents"][0]["text"])
    check(len(pre) == 97 and pre[0]["name"] == "CUBE" and "properties" in pre[0], "97 prefabs with property lists")
    one = json.loads(m.request("resources/read", {"uri": "krunker://prefabs/TRIGGER"})["result"]["contents"][0]["text"])
    check(any(p["key"] == "te" for p in one["properties"]), "TRIGGER schema has te")
    cat = json.loads(m.request("resources/read", {"uri": "krunker://catalog"})["result"]["contents"][0]["text"])
    check(any(e["key"] == "burg_new" and e["path"] == "assets/maps/burg_new.json" for e in cat), "catalog lists v7 Burg")
    check(any(e["key"] == "311_burg" and e["path"] == "assets/maps/311_burg.json" for e in cat), "catalog keeps v3 Burg")
    check("error" in m.request("resources/read", {"uri": "krunker://nope"}), "unknown resource errors")
    check(len(m.request("resources/templates/list")["result"]["resourceTemplates"]) >= 2, "resource templates")
    prompts = m.request("prompts/list")["result"]["prompts"]
    check(any(p["name"] == "build_arena" for p in prompts), "prompts")
    g = m.request("prompts/get", {"name": "review_map", "arguments": {"project": "burg"}})["result"]
    check("burg" in g["messages"][0]["content"]["text"], "prompt arguments substituted")
    # old protocol version still accepted
    m2 = Mcp(c.bin, c.root, c.ws)
    check(m2.initialize("2024-11-05")["result"]["protocolVersion"] == "2024-11-05", "2024-11-05 accepted")
    m2.close()


# ---------------------------------------------------------------- files

def save_and_compare(c, ref, path, fmt=None):
    m = c.mcp
    args = {"project": ref, "path": path}
    if fmt:
        args["format"] = fmt
    m.sc("map_save", args)


@test
def round_trip(c):
    m = c.mcp
    # the v3.1.1 maps (catalogue keys 311_<stem>): "burg" / "kanji" resolve to the v7.1.4 maps
    for name, rel in (("311_burg", "assets/maps/311_burg.json"), ("311_kanji", "assets/maps/311_kanji.json"),
                      ("Bhop_VIP", "assets/maps/custom/Bhop_VIP.json"), ("Krunk_Plaza", "assets/maps/custom/Krunk_Plaza.json")):
        p = m.sc("map_open", {"path": name})
        out = os.path.join(c.tmp, "rt_%s.json" % name)
        m.sc("map_save", {"project": p["id"], "path": out})
        with open(os.path.join(c.root, rel), "rb") as a, open(out, "rb") as b:
            check(a.read() == b.read(), "%s round-trips byte-identically" % name)
        # pretty, reopened, saved compact again: identical modulo formatting
        pretty = os.path.join(c.tmp, "rt_%s_pretty.json" % name)
        m.sc("map_save", {"project": p["id"], "path": pretty, "format": "pretty"})
        q = m.sc("map_open", {"path": pretty})
        back = os.path.join(c.tmp, "rt_%s_back.json" % name)
        m.sc("map_save", {"project": q["id"], "path": back, "format": "compact"})
        with open(os.path.join(c.root, rel), "rb") as a, open(back, "rb") as b:
            check(a.read() == b.read(), "%s pretty -> compact round-trips" % name)
        m.sc("project_close", {"project": q["id"]})
    info = m.sc("map_info", {"project": "burg"})
    check(info["object_count"] == 1152 and info["spawns"]["count"] == 12, "burg info")
    pl = m.sc("projects_list")
    check(len(pl["projects"]) == 4 and all(p["backend"] == "file" for p in pl["projects"]), "4 file projects")
    m.sc("project_focus", {"project": "burg"})
    check(m.sc("projects_list")["projects"][0]["focused"], "burg focused")


@test
def safety(c):
    m = c.mcp
    m.call("map_open", {"path": "/etc/passwd"}, ok=False)
    m.call("map_save", {"project": "burg", "path": "/etc/kn_test.json"}, ok=False)
    m.call("map_save", {"project": "burg", "path": os.path.join(c.tmp, "x.txt")}, ok=False)
    m.call("map_open", {"path": "../../../../etc/passwd"}, ok=False)
    m.call("map_save", {"project": "burg", "path": os.path.join(c.tmp, "rt_311_burg.json")}, ok=False)  # exists, no overwrite
    ro = Mcp(c.bin, c.root, c.ws, ["--read-only", "--open", "burg"])
    ro.initialize()
    ro.call("objects_delete", {"ids": [1]}, ok=False)
    ro.call("map_save", {"path": os.path.join(c.tmp, "ro.json")}, ok=False)
    ro.close()


# ---------------------------------------------------------------- queries

@test
def queries(c):
    m = c.mcp
    q = m.sc("objects_query", {"project": "burg", "prefab": "CUBE", "limit": 5})
    check(q["total"] == 874 and q["count"] == 5 and q["next_offset"] == 5, "CUBE pagination")
    q2 = m.sc("objects_query", {"project": "burg", "prefab": "CUBE", "limit": 5, "offset": 5})
    check(q2["objects"][0]["id"] != q["objects"][0]["id"], "offset pages")
    near = m.sc("objects_query", {"project": "burg", "near": {"center": [0, 0, 0], "radius": 30}, "limit": 50})
    check(near["total"] > 0 and all(o["distance"] <= 30 for o in near["objects"]), "near radius")
    check(near["objects"] == sorted(near["objects"], key=lambda o: o["distance"]), "sorted by distance")
    bb = m.sc("objects_query", {"project": "burg", "bbox": {"min": [-100, -50, -100], "max": [100, 100, 100], "mode": "inside"}, "fields": "ids", "limit": 2000})
    check(bb["total"] > 0 and isinstance(bb["objects"][0], int), "bbox inside, ids")
    w = m.sc("objects_query", {"project": "burg", "where": {"i": 9}, "limit": 100})
    check(w["total"] == 12, "where i=9 -> 12 ramps")
    has = m.sc("objects_query", {"project": "burg", "has": ["d"], "prefab": "RAMP"})
    check(has["total"] > 0, "has [d]")
    cat = m.sc("objects_query", {"project": "burg", "category": "zones"})
    check(cat["total"] == 6, "category zones")
    tex = m.sc("objects_query", {"project": "burg", "texture": "DIRT", "limit": 1})
    check(tex["total"] > 0, "texture filter")
    full = m.sc("objects_query", {"project": "burg", "prefab": "RAMP", "limit": 1, "fields": "full"})["objects"][0]
    check("raw" in full and "props" in full and "bounds" in full, "full fields")
    one = m.sc("object_get", {"project": "burg", "id": full["id"]})
    check(one["prefab"] == "RAMP" and one["raw"]["i"] == 9, "object_get")
    many = m.sc("object_get", {"project": "burg", "ids": [1, 2], "defaults": True})
    check(len(many["objects"]) == 2 and "opacity" in many["objects"][0]["props"], "object_get ids + defaults")
    m.call("object_get", {"project": "burg", "id": 999999}, ok=False)
    pf = m.sc("prefabs", {"name": "TELEPORTER"})
    check(any(p["prop"] == "channel" for p in pf["prefabs"][0]["properties"]), "prefabs tool")


# ---------------------------------------------------------------- edits + undo

@test
def edits_and_undo(c):
    m = c.mcp
    p = m.sc("map_new", {"name": "Edit Test", "template": "blank"})
    pid = p["id"]
    c.edit_pid = pid
    created = m.sc("objects_create", {"project": pid, "objects": [
        {"prefab": "CUBE", "p": [0, 0, 0], "s": [10, 10, 10], "props": {"color": "#ff0000", "texture": "BRICK"}},
        {"prefab": "CRATE", "p": [20, 0, 0]},
        {"prefab": "RAMP", "p": [40, 0, 0], "s": [20, 10, 20], "props": {"direction": 1}},
    ]})["created"]
    check(len(created) == 3, "3 created")
    cube = m.sc("object_get", {"project": pid, "id": created[0]})
    check(cube["raw"].get("c") == "#ff0000" and cube["raw"]["t"] == 11, "color + texture written as Krunker keys")
    crate = m.sc("object_get", {"project": pid, "id": created[1]})
    check(crate["raw"]["i"] == 1 and crate["s"] == [12, 12, 12], "crate default size")
    # validation of edits
    m.call("objects_create", {"project": pid, "objects": [{"prefab": "NOPE"}]}, ok=False)
    m.call("objects_create", {"project": pid, "objects": [{"prefab": "CUBE", "props": {"channel": 3}}]}, ok=False)  # not on CUBE
    m.call("objects_create", {"project": pid, "objects": [{"prefab": "CUBE", "props": {"opacity": 7}}]}, ok=False)  # range
    m.call("objects_create", {"project": pid, "objects": [{"prefab": "CUBE", "s": [0, 1, 1]}]}, ok=False)
    m.call("objects_create", {"project": pid, "objects": [{"prefab": "CUBE", "props": {"bogus": 1}}]}, ok=False)
    cl = m.sc("objects_create", {"project": pid, "objects": [{"prefab": "CUBE", "p": [0, 0, 60], "props": {"opacity": 7}}], "clamp": True})["created"][0]
    check(m.sc("object_get", {"project": pid, "id": cl})["raw"].get("o") is None, "clamped opacity 1 = default omitted")
    fo = m.sc("objects_create", {"project": pid, "objects": [{"prefab": "CUBE", "p": [0, 0, 80], "props": {"channel": 3, "bogus": 1}}], "force": True})["created"][0]
    raw = m.sc("object_get", {"project": pid, "id": fo})["raw"]
    check(raw["ch"] == 3 and raw["bogus"] == 1, "force writes keys")
    before = m.sc("map_info", {"project": pid})["object_count"]
    # update
    u = m.sc("objects_update", {"project": pid, "updates": [
        {"id": created[0], "p": [5, 0, 5], "props": {"collidable": False, "visible": False, "color": None}},
        {"id": created[1], "prefab": "BARREL"},
    ], "return_objects": True})
    o0 = [o for o in u["objects"] if o["id"] == created[0]][0]
    check(o0["raw"].get("l") == 1 and o0["raw"].get("v") == 1 and "c" not in o0["raw"] and o0["p"] == [5, 0, 5], "update props")
    check(m.sc("object_get", {"project": pid, "id": created[1]})["prefab"] == "BARREL", "prefab change")
    m.call("objects_update", {"project": pid, "updates": [{"id": 424242, "p": [0, 0, 0]}]}, ok=False)
    # delete + undo / redo
    m.sc("objects_delete", {"project": pid, "ids": [created[2]]})
    check(m.sc("map_info", {"project": pid})["object_count"] == before - 1, "deleted")
    h = m.sc("history", {"project": pid})
    check(h["undo"][0].startswith("delete"), "history label")
    m.sc("undo", {"project": pid})
    check(m.sc("map_info", {"project": pid})["object_count"] == before, "undo restores")
    check(m.sc("object_get", {"project": pid, "id": created[2]})["prefab"] == "RAMP", "ids survive undo")
    m.sc("redo", {"project": pid})
    check(m.sc("map_info", {"project": pid})["object_count"] == before - 1, "redo")
    m.sc("undo", {"project": pid, "steps": 2})
    check(m.sc("object_get", {"project": pid, "id": created[1]})["prefab"] == "CRATE", "multi-step undo")
    m.call("redo", {"project": pid, "steps": 1})
    # undo to the very beginning restores the exact initial document
    first = os.path.join(c.tmp, "edit_first.json")
    p2 = m.sc("map_new", {"name": "Undo Exact", "template": "arena"})
    m.sc("map_save", {"project": p2["id"], "path": first, "overwrite": True})
    m.sc("objects_create", {"project": p2["id"], "objects": [{"prefab": "CUBE", "p": [1, 2, 3]}]})
    m.sc("map_settings_set", {"project": p2["id"], "set": {"fog": "#112233"}})
    m.sc("undo", {"project": p2["id"], "steps": 2})
    again = os.path.join(c.tmp, "edit_again.json")
    m.sc("map_save", {"project": p2["id"], "path": again, "overwrite": True})
    check(open(first).read() == open(again).read(), "undo back to the start is byte-identical")
    m.call("undo", {"project": p2["id"]}, ok=False)  # nothing left
    # duplicate
    d = m.sc("objects_duplicate", {"project": pid, "ids": [created[0]], "offset": [0, 0, 15], "count": 3})["created"]
    check(len(d) == 3, "3 duplicates")
    check(m.sc("object_get", {"project": pid, "id": d[2]})["p"] == [5, 0, 50], "duplicate offsets accumulate")


@test
def transforms(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Transform", "template": "empty"})["id"]
    ids = m.sc("objects_create", {"project": pid, "objects": [
        {"p": [0, 0, 0], "s": [4, 4, 4]}, {"p": [10, 2, 3], "s": [2, 2, 2]}, {"p": [30, 0, -5], "s": [6, 6, 6]},
        {"prefab": "LADDER", "p": [50, 0, 0], "props": {"direction": 1}}]})["created"]
    get = lambda i: m.sc("object_get", {"project": pid, "id": i})
    m.sc("objects_transform", {"project": pid, "ids": ids[:3], "op": "move", "delta": [1, 0, 0]})
    check(get(ids[0])["p"] == [1, 0, 0], "move delta")
    m.sc("objects_transform", {"project": pid, "ids": [ids[0]], "op": "move", "to": [100, 5, 100]})
    check(get(ids[0])["p"] == [100, 5, 100], "move to")
    m.sc("objects_transform", {"project": pid, "ids": [ids[1]], "op": "rotate", "degrees": [0, 90, 0]})
    check(abs(get(ids[1])["r"][1] - 1.5708) < 1e-3, "rotate yaw")
    m.sc("objects_transform", {"project": pid, "ids": [ids[1]], "op": "rotate", "degrees": [0, 0, 0], "mode": "absolute"})
    check("r" not in get(ids[1])["raw"], "absolute zero rotation drops r")
    m.sc("objects_transform", {"project": pid, "ids": [ids[2]], "op": "scale", "factor": 2, "pivot": "each"})
    check(get(ids[2])["s"] == [12, 12, 12], "scale factor")
    m.sc("objects_transform", {"project": pid, "ids": [ids[2]], "op": "scale", "size": [3, 4, 5]})
    check(get(ids[2])["s"] == [3, 4, 5], "scale size")
    m.sc("objects_create", {"project": pid, "objects": [{"p": [0.3, 0.6, 1.4], "s": [1, 1, 1]}]})
    j = m.sc("objects_query", {"project": pid, "where": {"p": [0.3, 0.6, 1.4]}})["objects"][0]["id"]
    m.sc("objects_transform", {"project": pid, "ids": [j], "op": "snap", "grid": 1})
    check(get(j)["p"] == [0, 1, 1], "snap")
    m.sc("objects_transform", {"project": pid, "ids": [ids[1], ids[2]], "op": "align", "axis": "y", "to": "min"})
    check(get(ids[1])["p"][1] == get(ids[2])["p"][1], "align y min")
    a, b, cc = [m.sc("objects_create", {"project": pid, "objects": [{"p": [x, 0, 200], "s": [2, 2, 2]}]})["created"][0] for x in (0, 3, 40)]
    m.sc("objects_transform", {"project": pid, "ids": [a, b, cc], "op": "distribute", "axis": "x"})
    check(get(b)["p"][0] == 20, "distribute evenly")
    m.sc("objects_transform", {"project": pid, "ids": [a, b, cc], "op": "distribute", "axis": "x", "gap": 1})
    check(get(b)["p"][0] == 3 and get(cc)["p"][0] == 6, "distribute gap")
    mir = m.sc("objects_transform", {"project": pid, "ids": [ids[3]], "op": "mirror", "axis": "z", "plane": 10, "copy": True})
    lad = get(mir["created"][0])
    check(lad["p"][2] == 20 and lad["raw"]["d"] == 3, "mirror copy flips position and direction")
    m.sc("objects_create", {"project": pid, "objects": [{"p": [500, 0, 500], "s": [50, 5, 50]}, {"p": [500, 40, 500], "s": [4, 4, 4]}]})
    top = m.sc("objects_query", {"project": pid, "where": {"p": [500, 40, 500]}})["objects"][0]["id"]
    m.sc("objects_transform", {"project": pid, "ids": [top], "op": "drop"})
    check(get(top)["p"][1] == 5, "drop onto the surface below")
    m.call("objects_transform", {"project": pid, "ids": [top], "op": "explode"}, ok=False)
    check(len(m.sc("history", {"project": pid})["undo"]) >= 10, "one history entry per transform")


@test
def cross_project_copy(c):
    m = c.mcp
    src = m.sc("objects_query", {"project": "burg", "prefab": "CUBE", "has": ["ci"], "limit": 2})["objects"]
    before = m.sc("map_info", {"project": "kanji"})["object_count"]
    res = m.sc("objects_copy_between_projects", {"from": "burg", "to": "kanji", "ids": [o["id"] for o in src], "offset": [0, 20, 0]})
    check(len(res["created"]) == 2 and res["objects"] == before + 2, "copied 2")
    for o, nid in zip(src, res["created"]):
        n = m.sc("object_get", {"project": "kanji", "id": nid})
        check(n["p"] == [o["p"][0], o["p"][1] + 20, o["p"][2]], "offset applied")
        check(n.get("color") == o.get("color"), "palette colour resolved into the target palette")
    m.sc("undo", {"project": "kanji"})
    check(m.sc("map_info", {"project": "kanji"})["object_count"] == before, "one undo step in the target")
    at = m.sc("objects_copy_between_projects", {"from": "burg", "to": c.edit_pid, "ids": [src[0]["id"]], "at": [0, 50, 0]})
    n = m.sc("object_get", {"project": c.edit_pid, "id": at["created"][0]})
    check(n["p"][1] == 50, "at places the bottom centre")


# ---------------------------------------------------------------- logic

@test
def spawns(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Spawns", "template": "blank"})["id"]
    ls = m.sc("spawns_list", {"project": pid})
    check(ls["count"] == 2 and ls["spawns"][0]["ground_y"] == 0, "template spawns on the floor")
    m.sc("spawns_add", {"project": pid, "spawns": [{"p": [10, 15, 10], "team": 1, "yaw_deg": 180}, {"p": [20, 5, 20], "team": 2, "comp": True}]})
    ls = m.sc("spawns_list", {"project": pid})
    check(ls["count"] == 4 and ls["spawns"][2]["p"][1] == 0 and ls["spawns"][2]["dir"] == 1 and ls["spawns"][2]["facing"] == "+z", "added, snapped to ground, dir from yaw (180 deg faces +Z)")
    m.sc("spawns_update", {"project": pid, "updates": [{"index": 3, "team": 1, "p": [25, 0, 25]}]})
    ls = m.sc("spawns_list", {"project": pid})
    check(ls["spawns"][3]["team"] == 1 and ls["spawns"][3]["p"] == [25, 0, 25] and ls["spawns"][3]["comp"], "updated, comp kept")
    m.sc("spawns_delete", {"project": pid, "indices": [0, 1]})
    check(m.sc("spawns_list", {"project": pid})["count"] == 2, "deleted")
    m.call("spawns_update", {"project": pid, "updates": [{"index": 9}]}, ok=False)
    m.call("spawns_add", {"project": pid, "spawns": [{"p": [0, 0, 0], "team": 5}]}, ok=False)


@test
def triggers(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Triggers", "template": "blank"})["id"]
    cat = m.sc("triggers_catalog")
    check(len(cat["events"]) == 32 and len(cat["actions"]) == 72, "v7 trigger catalog")
    gate = m.sc("objects_create", {"project": pid, "objects": [{"prefab": "GATE", "p": [0, 0, 30], "s": [20, 20, 2]}]})["created"][0]
    t = m.sc("triggers_create", {"project": pid, "triggers": [
        {"p": [0, 0, 0], "event": "onEnter", "action": "Toggle Interface Gate"},
        {"p": [20, 0, 0], "event": "onTimer", "event_value": 5, "action": "Broadcast Message", "text": "hello"}]})["created"]
    raw = m.sc("object_get", {"project": pid, "id": t[0]})["raw"]
    check(raw["i"] == 29 and raw["te"] == 3 and raw["ta"] == 5, "onEnter -> te 3, Toggle Interface Gate -> ta 5")
    link = m.sc("triggers_link", {"project": pid, "trigger": t[0], "targets": [gate]})
    check(link["interface_id"] >= 1 and link["trigger"]["targets"] == [gate], "linked")
    lst = m.sc("triggers_list", {"project": pid})
    check(lst["count"] == 2 and lst["triggers"][1]["action"]["text"] == "hello", "triggers_list decodes")
    m.sc("triggers_update", {"project": pid, "updates": [{"id": t[1], "action": "Give Player Score", "value": 50}]})
    check(m.sc("object_get", {"project": pid, "id": t[1]})["raw"]["tc"] == 50, "trigger update")
    ifs = m.sc("interfaces_list", {"project": pid})
    check(str(link["interface_id"]) in ifs["interfaces"] and ifs["dangling_targets"] == [], "interfaces")
    m.call("triggers_create", {"project": pid, "triggers": [{"event": "onNothing"}]}, ok=False)
    m.call("triggers_update", {"project": pid, "updates": [{"id": gate, "value": 1}]}, ok=False)
    # v7 action lists (tas, editor WP4): written as ObjectInstance.serialize() writes them
    v7 = m.sc("triggers_create", {"project": pid, "triggers": [{"p": [40, 0, 0], "event": "onInteractKey", "action_method": 2, "action_cost": 50, "actions": [
        {"action": "Give Player Score", "value": 25, "delay": 1.5},
        {"action": "Broadcast Sound", "sound": 5, "volume": 0.5, "rate": 2, "emit_from": 2},
        {"action": 0, "value_type": "Player Value", "key": "kills", "condition": "Player Value", "condition_values": ["score", "3", 7]},
        {"action": "Change Class", "value": 5, "condition": 4, "condition_values": ["2", "1", 3]},
        {"action": "Custom Action", "text": "door:open", "value2": 9}]}]})["created"]
    raw = m.sc("object_get", {"project": pid, "id": v7[0]})["raw"]
    check(raw["tas"] == [{"tc": 25, "tad": 1.5}, {"es": 5, "ev": 0.5, "er": 2, "ef": 2, "ta": 14},
                         {"tce": 2, "tcc": "kills", "cd": 1, "cv0": "score", "cv1": "3", "cv2": 7},
                         {"ta": 47, "tc": 5, "cd": 4, "cv0": "2", "cv1": "1", "cv2": 3}, {"ta": 58, "tc2": 9, "tct": "door:open"}]
          and raw["tam"] == 2 and raw["tac"] == 50 and "ta" not in raw, "actions -> tas (v7 trigger-actions golden)")
    lst = m.sc("triggers_list", {"project": pid})
    dec = [t for t in lst["triggers"] if t["id"] == v7[0]][0]["actions"]
    check(len(dec) == 5 and dec[2]["condition"]["name"] == "Player Value" and dec[2]["value_type"] == "Player Value" and dec[1]["sound"]["volume"] == 0.5,
          "triggers_list decodes the v7 list")
    m.sc("triggers_update", {"project": pid, "updates": [{"id": v7[0], "action": "Kill Player", "value": 3}]})
    raw = m.sc("object_get", {"project": pid, "id": v7[0]})["raw"]
    check(raw["tas"][0] == {"ta": 1, "tc": 3, "tad": 1.5} and len(raw["tas"]) == 5 and "ta" not in raw, "action / value edit tas[0] of a v7 trigger")
    m.call("triggers_update", {"project": pid, "updates": [{"id": v7[0], "actions": [{"action": 0}] * 6}]}, ok=False)
    m.call("triggers_update", {"project": pid, "updates": [{"id": v7[0], "actions": [{"condition": "Nope"}]}]}, ok=False)


@test
def settings(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Settings", "template": "blank"})["id"]
    m.sc("map_settings_set", {"project": pid, "set": {"fog": "#102030", "fogD": 900, "sky": "abc"}, "config": {"modes": ["Free for All", "Capture the Flag"], "settings": {"gravMlt": 1.5}}})
    g = m.sc("map_settings_get", {"project": pid})
    v = g["values"]
    check(v["fog"] == "#102030" and v["fogD"] == 900 and v["sky"] == "#aabbcc", "colours normalised")
    check(v["config"]["modes"] == [0, 3] and v["config"]["settings"]["gravMlt"] == 1.5, "config modes by name")
    m.call("map_settings_set", {"project": pid, "set": {"notAKey": 1}}, ok=False)
    m.call("map_settings_set", {"project": pid, "config": {"settings": {"gravMlt": 9}}}, ok=False)
    m.call("map_settings_set", {"project": pid, "set": {"objects": []}}, ok=False)
    m.sc("map_settings_set", {"project": pid, "set": {"notAKey": 1}, "force": True})
    check(m.sc("map_settings_get", {"project": pid, "group": "fog"})["values"].get("fogD") == 900, "group filter")
    # class / effect / weapon overrides (v7 Class / Effect / Weapon Config, editor WP4)
    m.sc("map_settings_set", {"project": pid, "config": {"cls": {"0": {"speed": 1.5, "camType": "Platformer (X)", "health": 100}, "Hunter": None},
                                                         "prks": {"Rooted": {"max": 3}}, "weps": {"Sniper Rifle": {"name": "Longshot", "dmg": 90}}}})
    cfg = m.sc("map_settings_get", {"project": pid})["values"]["config"]
    check(cfg["cls"] == {"0": {"speed": 1.5, "camType": "2"}} and cfg["prks"] == {"4": {"max": 3}} and cfg["weps"] == {"0": {"name": "Longshot", "dmg": 90}},
          "class / effect / weapon overrides (stock values dropped)")
    m.sc("map_settings_set", {"project": pid, "config": {"cls": {"Triggerman": None}, "prks": {"4": {"max": None}}}})
    cfg = m.sc("map_settings_get", {"project": pid})["values"]["config"]
    check("cls" not in cfg and "prks" not in cfg and "weps" in cfg, "null resets overrides")
    m.call("map_settings_set", {"project": pid, "config": {"cls": {"0": {"speed": 99}}}}, ok=False)
    m.call("map_settings_set", {"project": pid, "config": {"cls": {"0": {"nope": 1}}}}, ok=False)


# ---------------------------------------------------------------- build

@test
def groups_and_prefabs(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Groups", "template": "blank"})["id"]
    ids = m.sc("objects_create", {"project": pid, "objects": [{"p": [0, 0, 0], "s": [4, 8, 4]}, {"p": [6, 0, 0], "s": [4, 4, 4], "props": {"color": "#00ff00"}}]})["created"]
    m.sc("group_create", {"project": pid, "name": "pillar", "ids": ids})
    check(m.sc("group_list", {"project": pid})["groups"][0]["ids"] == ids, "group stored")
    st = m.sc("group_stamp", {"project": pid, "name": "pillar", "at": [0, 0, 50], "count": 3, "step": [20, 0, 0], "new_group": "copies"})
    check(len(st["created"]) == 6, "3 stamps of 2")
    first = m.sc("object_get", {"project": pid, "id": st["created"][0]})
    check(first["p"] == [-3, 0, 50], "stamp anchors the bottom centre")
    rot = m.sc("group_stamp", {"project": pid, "ids": ids, "at": [100, 0, 0], "rotate_y_deg": 90})
    r1 = m.sc("object_get", {"project": pid, "id": rot["created"][1]})
    check(abs(r1["p"][0] - 100) < 1e-6 and abs(r1["p"][2] + 3) < 1e-6, "quarter turn rotates offsets")
    m.sc("prefab_save", {"project": pid, "name": "test_pillar", "group": "pillar", "description": "two blocks"})
    check(any(p["name"] == "test_pillar" for p in m.sc("prefab_list")["prefabs"]), "library prefab listed")
    ps = m.sc("prefab_stamp", {"project": "burg", "name": "test_pillar", "at": [0, 100, 0]})
    check(len(ps["created"]) == 2, "stamped into another map")
    m.sc("undo", {"project": "burg"})
    m.sc("group_delete", {"project": pid, "name": "copies", "delete_objects": True})
    check(len(m.sc("group_list", {"project": pid})["groups"]) == 1, "group removed")


@test
def generators(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Gen", "template": "empty"})["id"]
    room = m.sc("generate_box_room", {"project": pid, "center": [0, 0, 0], "size": [100, 30, 80], "wall": 2,
                                      "doors": [{"side": "south", "width": 12, "height": 16}, {"side": "east", "offset": 10}],
                                      "props": {"texture": "BRICK"}})
    check(room["count"] == 9, "floor + 4 walls, 2 split around doors with lintels = 9 (%d)" % room["count"])
    check(m.sc("history", {"project": pid})["undo_count"] == 1, "one undo step")
    st = m.sc("generate_stairs", {"project": pid, "start": [0, 0, -30], "direction": "-z", "height": 12, "width": 8})
    check(st["steps"] == 5 and st["step_height"] <= 3, "stairs within climb height")
    rp = m.sc("generate_stairs", {"project": pid, "start": [20, 0, 0], "direction": "+x", "height": 10, "style": "ramp"})
    check(rp["count"] == 1, "ramp")
    ramp = m.sc("object_get", {"project": pid, "id": rp["created"][0]})
    check(ramp["prefab"] == "RAMP" and ramp["p"][0] > 20, "ramp placed along +x")
    m.call("generate_stairs", {"project": pid, "height": 10, "step_height": 6}, ok=False)
    pp = m.sc("generate_platform_path", {"project": pid, "start": [0, 50, 0], "end": [200, 70, 0], "count": 6})
    check(pp["count"] == 6 and len(pp["jumps"]) == 5, "platform path")
    hard = m.sc("generate_platform_path", {"project": pid, "points": [[0, 0, 300], [80, 0, 300]]})
    check(hard["too_hard"] == 1, "flags an impossible gap")
    m.sc("objects_create", {"project": pid, "objects": [{"p": [0, -5, 500], "s": [200, 5, 200]}]})
    fill = m.sc("fill_area", {"project": pid, "bbox": {"min": [-90, -10, 410], "max": [90, 40, 590]}, "prefab": "CRATE", "count": 12, "seed": 3})
    check(fill["placed"] == 12, "12 crates placed")
    crate = m.sc("object_get", {"project": pid, "id": fill["created"][0]})
    check(crate["p"][1] == 0, "crates stand on the floor")


# ---------------------------------------------------------------- checks

@test
def validation_catches_seeded_errors(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Broken", "template": "blank"})["id"]
    clean = m.sc("map_validate", {"project": pid})
    check(clean["ok"], "blank template validates clean: %s" % clean["diagnostics"])
    floor = m.sc("objects_query", {"project": pid, "limit": 1})["objects"][0]["id"]
    # seeded problems
    m.sc("objects_create", {"project": pid, "force": True, "objects": [
        {"p": [100, 0, 100], "s": [10, 10, 10], "raw": {"i": 500}},                 # unknown prefab
        {"p": [0, 0, 0], "s": [10, 10, 10], "raw": {"t": 99}},                      # bad texture
        {"p": [0, 0, 0], "s": [10, 10, 10], "raw": {"ci": 7}},                      # palette index without palette
        {"prefab": "TRIGGER", "p": [50, 0, 50], "props": {"target_interface": 77}},  # dangling target
        {"prefab": "TRIGGER", "p": [60, 0, 50], "raw": {"tct": "<script>"}},          # invalid text
        {"prefab": "TELEPORTER", "p": [70, 0, 50], "props": {"channel": 4}},          # sender without receiver
        {"p": [40, 0, 0], "s": [20, 20, 20]},                                         # block on spawn 1
        {"p": [3000, 0, 3000], "s": [20, 2, 20]},                                     # island
    ]})
    m.sc("spawns_add", {"project": pid, "spawns": [{"p": [3000, 2, 3000]}, {"p": [9000, 50, 9000]}], "snap_to_ground": False})
    m.sc("objects_update", {"project": pid, "updates": [{"id": floor, "raw": {"s": None, "si": 99}}], "force": True})
    v = m.sc("map_validate", {"project": pid})
    codes = v["codes"]
    for code in ("object.unknown_prefab", "object.bad_texture", "object.bad_color_index", "trigger.dangling_target", "trigger.bad_text",
                 "teleporter.no_receiver", "spawn.inside_solid", "spawn.over_void", "object.bad_size_index"):
        check(code in codes, "validation reports %s (got %s)" % (code, sorted(codes)))
    check(not v["ok"] and v["errors"] >= 6, "errors counted")
    diag = [d for d in v["diagnostics"] if d["code"] == "spawn.inside_solid"][0]
    check("object_ids" in diag and "spawn" in diag, "diagnostics carry object ids / spawn index")
    # reachability on a healthy map with an isolated spawn island
    pid2 = m.sc("map_new", {"name": "Islands", "template": "blank"})["id"]
    m.sc("objects_create", {"project": pid2, "objects": [{"p": [1000, -10, 1000], "s": [40, 10, 40]}]})
    m.sc("spawns_add", {"project": pid2, "spawns": [{"p": [1000, 0, 1000]}]})
    r = m.sc("map_validate", {"project": pid2})
    check("spawn.unreachable" in r["codes"], "isolated spawn found unreachable via the nav grid (%s)" % sorted(r["codes"]))
    big = m.sc("map_validate", {"project": "kanji", "min_severity": "error"})
    check(big["ok"], "kanji validates without errors")


@test
def compile_map(c):
    m = c.mcp
    out = os.path.join(c.tmp, "kanji_compiled.json")
    r = m.sc("map_compile", {"project": "kanji", "output": out})
    check(r["loads_natively"] and r["native_objects"] > 0, "compiled kanji loads natively")
    check(r["bytes_after"] < r["bytes_before"] and r["pooled_sizes"] > 0, "smaller, sizes pooled")
    d = json.load(open(out))
    check("xyz" in d and all("s" not in o for o in d["objects"]), "si/xyz form")
    check(r["krunkscript"]["status"], "krunkscript status reported")
    s = m.sc("map_compile", {"project": "burg", "inline": True, "pool_sizes": False, "scripts": {"client": "# hello"}})
    j = json.loads(s["json"])
    check(base64.b64decode(j["scripts"]["client"]).decode() == "# hello", "scripts attached base64")
    ks = m.sc("map_compile", {"project": "burg", "pool_sizes": False, "scripts": {"client": "num x = ;"}})
    if ks["krunkscript"]["compiler_available"]:
        e = ks["krunkscript"]["diagnostics"][0]
        check(not ks["publishable"] and e["code"] == "script.compile_error" and e["line"] == 1 and e["col"] == 9, "KrunkScript error with position: %s" % e)
    bad = m.sc("map_new", {"name": "x", "template": "empty"})["id"]
    m.call("map_compile", {"project": bad}, ok=False)  # no spawns -> validation error blocks
    # complex collision: the bake reproduces the triangles v7.1.4 exported with the map
    path = os.path.join(c.root, "assets", "maps", "shipment.json")
    src = json.load(open(path))
    sp = m.sc("map_open", {"path": path})["id"]
    t = m.sc("map_compile", {"project": sp, "inline": True, "force": True})
    tri = lambda a: sorted(tuple(a[i:i + 9]) for i in range(0, len(a), 9))
    check(t["bake"]["triangles"] == len(src["tris"]) // 9 and tri(json.loads(t["json"])["tris"]) == tri(src["tris"]),
          "map_compile bakes shipment's tris as v7 did (%s)" % t["bake"]["tris"])
    n = m.sc("map_compile", {"project": sp, "inline": True, "force": True, "bake_tris": False})
    check("tris" not in json.loads(n["json"]), "bake_tris:false leaves tris out")


@test
def scripts(c):
    """script_get / script_set: the editor's export form (UTF-8 base64, the template omitted),
    diagnostics as ksc reports them, one undo step per write."""
    m = c.mcp
    pid = m.sc("map_new", {"name": "scripted", "template": "empty"})["id"]
    g = m.sc("script_get", {"project": pid})
    for side in ("client", "server"):
        s = g["scripts"][side]
        check(s["is_template"] and not s["in_map"] and s["ok"] and s["source"].startswith("# "), "%s shows the template" % side)
    src = "# h\u00e9llo \u2713\npublic action start() {\n\tGAME.log(\"\u00fc\");\n}\n"
    w = m.sc("script_set", {"project": pid, "side": "client", "source": src})
    check(w["script"]["ok"] and w["script"]["in_map"] and not w["script"]["is_template"], "client script stored")
    check(m.sc("script_get", {"project": pid, "side": "client"})["scripts"]["client"]["source"] == src, "non-ASCII source round-trips")
    out = os.path.join(c.tmp, "scripted.json")
    m.sc("map_save", {"project": pid, "path": out})
    d = json.load(open(out, encoding="utf-8"))
    check(base64.b64decode(d["scripts"]["client"]).decode("utf-8") == src and "server" not in d["scripts"], "saved as UTF-8 base64, server omitted")
    bad = "public action start() {\n\tnum x = 1;\n\tx = \"s\";\n}\n"
    r = m.call("script_set", {"project": pid, "side": "server", "source": bad}, ok=False)
    diag = r["structuredContent"]["diagnostics"][0]
    check(diag["code"] == "script.compile_error" and diag["line"] == 3, "a compile error is refused with its position: %s" % diag)
    # the same position as ksc
    ksc = os.path.join(os.path.dirname(c.bin), "..", "..", "ksc")
    if os.access(ksc, os.X_OK):
        bad_path = os.path.join(c.tmp, "bad.krnk")
        with open(bad_path, "w") as f:
            f.write(bad)
        k = json.loads(subprocess.run([ksc, "check", "--json", "--side", "server", bad_path], capture_output=True, text=True).stdout)
        e = k["scripts"][0]["errors"][0]
        check((e["line"], e["col"]) == (diag["line"], diag["col"]) and e["message"] in diag["message"], "matches ksc: %s vs %s" % (e, diag))
    m.sc("script_set", {"project": pid, "side": "server", "source": bad, "force": True})
    gs = m.sc("script_get", {"project": pid, "side": "server"})["scripts"]["server"]
    check(not gs["ok"] and gs["diagnostics"][0]["line"] == 3, "forced script reports its error")
    m.sc("undo", {"project": pid})
    check(m.sc("script_get", {"project": pid, "side": "server"})["scripts"]["server"]["is_template"], "undo restores the template")
    m.sc("script_set", {"project": pid, "side": "client", "source": ""})
    check("scripts" not in json.loads(m.sc("map_compile", {"project": pid, "inline": True, "force": True})["json"]), "\"\" removes the script")
    m.call("script_set", {"project": pid, "side": "both", "source": "x"}, ok=False)


@test
def renders(c):
    if c.no_render:
        print("    (skipped: --no-render)")
        return
    m = c.mcp
    r = m.call("render_multiview", {"project": "burg", "width": 320, "height": 200}, ok=False if not c.client else True)
    if not c.client:
        print("    (skipped: no client_bin)")
        return
    imgs = images_of(r)
    check(len(imgs) == 4 and all(is_png(i["data"]) for i in imgs), "render_multiview returns 4 PNG images")
    check([v["view"] for v in r["structuredContent"]["views"]] == ["iso", "front", "top", "right"], "view order")
    v = m.call("render_view", {"project": "burg", "position": [0, 80, 200], "target": [0, 0, 0], "width": 320, "height": 180})
    check(len(images_of(v)) == 1 and is_png(images_of(v)[0]["data"]), "render_view PNG")
    t = m.call("render_topdown", {"project": "burg", "width": 400, "depth": 400, "pixels_per_unit": 0.5})
    check(len(images_of(t)) == 1, "render_topdown PNG")
    sel = m.call("render_multiview", {"project": "burg", "ids": [1, 2], "width": 200, "height": 200})
    check(len(images_of(sel)) == 4, "multiview framed on ids")


@test
def playtest(c):
    m = c.mcp
    pid = m.sc("map_new", {"name": "Play", "template": "arena"})["id"]
    r = m.call("playtest", {"project": pid, "seconds": 6, "bots": 3, "screenshot": c.client is not None and not c.no_render,
                            "path": [[-160, 0, 0], [-100, 0, 0], [-100, 0, 60]]})
    s = r["structuredContent"]
    check(s["bots"] == 3 and s["tick_ms"]["avg"] > 0, "bots played, timings")
    check(s["spawn_reachability"]["isolated_spawns"] == 0, "arena spawns connected")
    check(s["walker"]["reached"] >= 1, "walker reached a waypoint: %s" % s["walker"])
    check("fall_spots" in s and "stuck_spots" in s and "coverage" in s, "report fields")
    if c.client and not c.no_render:
        check(len(images_of(r)) == 1, "playtest screenshot")
    # falling map: spawns over the void
    pid2 = m.sc("map_new", {"name": "Fall", "template": "empty"})["id"]
    m.sc("spawns_add", {"project": pid2, "spawns": [{"p": [0, 10, 0]}, {"p": [50, 10, 0]}], "snap_to_ground": False})
    f = m.sc("playtest", {"project": pid2, "seconds": 4, "bots": 2, "screenshot": False})
    check(f["falls"] >= 1, "falls detected on a floorless map (%s)" % f.get("falls"))


@test
def logs_tool(c):
    m = c.mcp
    l = m.sc("logs", {"tail": 20})
    check(len(l["entries"]) > 0, "log entries")
    w = m.sc("logs", {"level": "warning", "tail": 50})
    check(all(e["level"] in ("warning", "error") for e in w["entries"]), "level filter")


@test
def http_transport(c):
    import urllib.request
    ann = os.path.join(c.tmp, "url.txt")
    p = subprocess.Popen([c.bin, "--root", c.root, "--workspace", c.ws, "--no-live", "--quiet", "--http", "127.0.0.1:0", "--announce", ann])
    try:
        for _ in range(50):
            if os.path.exists(ann) and open(ann).read().strip():
                break
            time.sleep(0.1)
        url = open(ann).read().strip()
        req = urllib.request.Request(url, data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "h", "version": "1"}}}).encode(), headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            sid = resp.headers.get("Mcp-Session-Id")
            body = json.loads(resp.read())
        check(body["result"]["serverInfo"]["name"] == "kn_editor_mcp" and sid, "HTTP initialize + session id")
        req = urllib.request.Request(url, data=json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "prefabs", "arguments": {"name": "CUBE"}}}).encode(), headers={"Content-Type": "application/json", "Mcp-Session-Id": sid})
        with urllib.request.urlopen(req, timeout=10) as resp:
            check(json.loads(resp.read())["result"]["structuredContent"]["prefabs"][0]["name"] == "CUBE", "HTTP tools/call")
    finally:
        p.terminate()
        p.wait(5)


@test
def live_editor(c):
    """Auto mode (no --live / --no-live): a headless editor with Kanji hosts the bus; the server lists
    file and editor projects together, copies between them, undoes through the editor's stack and
    renders with the editor. Skipped when client_bin has no editor bus."""
    if not c.client or b"--mcp-bus" not in open(c.client, "rb").read():
        print("    (skipped: client_bin without the editor bus)")
        return
    bus = os.path.join(c.tmp, "bus")
    cfg = os.path.join(c.tmp, "edcfg")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg})
    ed = subprocess.Popen([c.client, "--editor", "--mcp-bus", "--bus-dir", bus, "--mcp-auto-accept", "assets/maps/311_kanji.json"],
                          cwd=c.root, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    m = None
    try:
        for _ in range(150):
            if os.path.exists(os.path.join(bus, "editor-bus.sock")) and os.path.exists(os.path.join(bus, "editor-bus.token")):
                break
            time.sleep(0.1)
        check(os.path.exists(os.path.join(bus, "editor-bus.sock")), "editor hosts the bus")
        time.sleep(0.5)
        args = [c.bin, "--root", c.root, "--workspace", c.ws, "--allow", c.tmp, "--bus-dir", bus, "--quiet", "--open", "burg"]
        m = Mcp.__new__(Mcp)
        m.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        m.next_id = 1
        m.notifications = []
        m.initialize()
        pl = m.sc("projects_list")
        backends = sorted(p["backend"] for p in pl["projects"])
        check(backends == ["editor", "file"] and pl["editor"]["attached"], "file + editor projects in one list: %s" % backends)
        live = [p for p in pl["projects"] if p["backend"] == "editor"][0]["id"]
        before = m.sc("map_info", {"project": live})["object_count"]
        src = m.sc("objects_query", {"project": "burg", "prefab": "CRATE", "limit": 2})["objects"]
        cp = m.sc("objects_copy_between_projects", {"from": "burg", "to": live, "ids": [o["id"] for o in src], "offset": [0, 30, 0]})
        check(len(cp["created"]) == 2 and m.sc("map_info", {"project": live})["object_count"] == before + 2, "file -> editor copy")
        got = m.sc("object_get", {"project": live, "id": cp["created"][0]})
        check(got["p"] == [src[0]["p"][0], src[0]["p"][1] + 30, src[0]["p"][2]], "editor object at the offset position")
        m.sc("objects_update", {"project": live, "updates": [{"id": cp["created"][0], "props": {"color": "#123456"}}]})
        check(m.sc("object_get", {"project": live, "id": cp["created"][0]})["color"] == "#123456", "edit through the editor")
        h = m.sc("history", {"project": live})
        check(len(h["undo"]) >= 2, "editor history has the batches")
        m.sc("undo", {"project": live, "steps": 2})
        check(m.sc("map_info", {"project": live})["object_count"] == before, "editor undo")
        sp = m.sc("spawns_list", {"project": live})
        check(sp["count"] > 0 and "id" in sp["spawns"][0], "editor spawns are SPAWN_POINT objects")
        v = m.sc("map_validate", {"project": live, "reachability": False})
        check("errors" in v, "validate a live project")
        r = m.call("render_multiview", {"project": live, "width": 256, "height": 160})
        check(len(images_of(r)) == 4 and all(is_png(i["data"]) for i in images_of(r)), "editor multiview: 4 PNGs")
        t = m.call("render_topdown", {"project": live, "width": 300, "depth": 300})
        check(len(images_of(t)) == 1, "editor top-down")
        # KrunkScript through the editor: the bus map_set lands in the project's scripts, and the
        # editor's own export (the refreshed mirror) carries it back
        code = "# from MCP \u00e9\npublic action start() {\n}\n"
        m.sc("script_set", {"project": live, "side": "client", "source": code})
        got = m.sc("script_get", {"project": live, "side": "client"})["scripts"]["client"]
        check(got["source"] == code and got["in_map"], "script written through the editor: %r" % got.get("source"))
        m.sc("undo", {"project": live})
        check(m.sc("script_get", {"project": live, "side": "client"})["scripts"]["client"]["is_template"], "editor undo of the script")
    finally:
        if m:
            m.close()
        ed.terminate()
        try:
            ed.wait(10)
        except Exception:
            ed.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--bin", default=None)
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("-k", default=None, help="run tests whose name contains this")
    a = ap.parse_args()
    c = Ctx()
    c.root = os.path.abspath(a.root)
    c.bin = a.bin or os.path.join(c.root, "cmake-build-debug", "tools", "editor-mcp", "kn_editor_mcp")
    c.no_render = a.no_render
    c.client = None
    for d in ("cmake-build-release", "cmake-build-debug"):
        p = os.path.join(c.root, d, "client_bin")
        if os.access(p, os.X_OK):
            c.client = p
    # client_bin needs the game content (fonts, models, textures), which is gitignored: a checkout
    # without it (CI) runs the client-less paths, like a missing client_bin
    content = os.path.join(c.root, "assets", "css", "fonts", "font2.ttf")
    if c.client and not os.path.isfile(content):
        print("note: %s missing (game content not installed): renders, playtest screenshot and live editor skipped"
              % os.path.relpath(content, c.root))
        c.client = None
    c.tmp = tempfile.mkdtemp(prefix="kn-mcp-test-")
    c.ws = os.path.join(c.tmp, "ws")
    os.makedirs(c.ws)
    c.mcp = Mcp(c.bin, c.root, c.ws)
    failed = 0
    t0 = time.time()
    order = TESTS
    if a.k:
        # the file tests prepare projects later tests use
        order = [t for t in TESTS if t.__name__ in ("protocol", "round_trip", "edits_and_undo") or a.k in t.__name__]
    for t in order:
        t1 = time.time()
        try:
            t(c)
            print("ok   %-36s %6.2fs" % (t.__name__, time.time() - t1))
        except Exception as e:
            failed += 1
            print("FAIL %-36s %s" % (t.__name__, e))
            if os.environ.get("KN_MCP_TEST_TRACE"):
                traceback.print_exc()
    c.mcp.close()
    print("%d/%d passed in %.1fs (tmp %s)" % (len(order) - failed, len(order), time.time() - t0, c.tmp))
    if not failed and not os.environ.get("KEEP"):
        shutil.rmtree(c.tmp, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
