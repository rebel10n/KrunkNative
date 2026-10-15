"""Krunker's official map table: what the bundle says about it, and shared/include/krunker_maps.h.

Imported by extract_build.py (the bundle half goes into build.json["maps"]; the header is rendered
from that plus tools/krunker/maps.lock.json, which tools/krunker/fetch_maps.py writes from the API).

The bundle carries no static map table: the client builds it at startup from the API (anchors
below, game.deob.js 10.0.3 line numbers):
  - GAPI_URI's prod default (:16437) is the maps API origin: `new <Api>(GAPI_URI)` (:27615);
  - `'getMaps': function () { return req(api, '/maps', ...) }` (:26609) and
    `'getMap': function (i) { return req(api, '/maps/'.concat(i), {cache: 'no-cache'}) }`;
  - startup: `api.maps.getMaps().then(r => r.data.forEach(m => maps.push(m)))` (:223133), so the
    `init` mapIndex is a position in that list;
  - `init`: `maps[i].didLoad || custom && custom.data ? start : api.maps.getMap(i).then(r => maps[i] =
    r.data ...)` (:219292): an official map's JSON comes from GET <GAPI>/maps/<i>; a custom game's
    arrives inline (init arg 9 .data);
  - the bot FTUE map is a static file of the page: fetch('maps/' + m.fileName + '.json?v=...').
What the bundle does name, used here as drift anchors against the lock: the comp / zombies / raid
host presets (`allMaps: [names]` beside `maps: [indices]`), the monthly rotation pool
(`{0x2: 'Sandstorm', ...}`, hashed with 0x9e3779b1) and the FTUE entry {name, fileName}.
"""

import hashlib
import json
import os
import re

xb = None  # extract_build, bound by extract_build.maps_module() (this module uses its lexing helpers)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCK = os.path.join(ROOT, "tools", "krunker", "maps.lock.json")
OUT_HEADER = os.path.join(ROOT, "shared", "include", "krunker_maps.h")
SCHEMA = 1


def _num(tok):
    return int(tok, 16) if tok.lower().startswith("0x") else int(tok)


def _api_path(src, name):
    """`'<name>': function (a) { return req(api, '<path>'.concat(a) ...` -> ('<path>', takes_arg)."""
    m = re.search(xb.key(name) + r"\s*(?:async\s+)?function\s*\(\s*(" + xb.IDENT + r")?\s*\)\s*\{\s*return\s+" +
                  xb.IDENT + r"\s*\(\s*" + xb.IDENT + r"\s*,\s*(" + xb.STR + r")(\s*" + xb.prop("concat") +
                  r"\s*\(\s*(" + xb.IDENT + r")\s*\))?", src)
    if not m:
        xb.fail("maps api " + name, "no `'%s': function (..) { return req(api, '<path>'...` " % name)
    path = xb.js_unquote(m.group(2))
    takes = bool(m.group(3)) and m.group(1) is not None and m.group(4) == m.group(1)
    return path, takes, m.start()


def _gapi(src):
    m = re.search(xb.prop("GAPI_URI") + r"\s*\|\|\s*(" + xb.STR + ")", src)
    if not m:
        xb.fail("maps api origin", "no `env.GAPI_URI || '<url>'`")
    v = xb.js_unquote(m.group(1))
    if not re.fullmatch(r"https://[A-Za-z0-9.-]+(?::\d+)?", v):
        xb.fail("maps api origin", "%r is not an https origin" % v)
    return v


def _int_array(lit):
    items = [x.strip() for x in lit.strip()[1:-1].split(",") if x.strip()]
    if not items or not all(re.fullmatch(xb.NUM, x) for x in items):
        return None
    return [_num(x) for x in items]


def _presets(src):
    """[{title, names, indices}] of the host presets pairing `allMaps` with a same-length `maps`."""
    out = []
    starts = [m for m in re.finditer(xb.key("allMaps") + r"\s*(\[[^\]]*\])", src)]
    for k, m in enumerate(starts):
        names = [xb.js_unquote(x) for x in re.findall(xb.STR, m.group(1))]
        if not names:
            continue
        end = starts[k + 1].start() if k + 1 < len(starts) else len(src)
        window = src[m.end():min(end, m.end() + 3000)]
        back = src[max(0, m.start() - 400):m.start()]
        tm = None
        for tm_ in re.finditer(xb.key("title") + r"\s*(" + xb.STR + ")", back):
            tm = tm_
        for am in re.finditer(xb.key("maps") + r"\s*(\[[^\]]*\])", window):
            idx = _int_array(am.group(1))
            if idx and len(idx) == len(names):
                out.append({"title": xb.js_unquote(tm.group(1)) if tm else None, "names": names, "indices": idx})
                break
    return out


def _rotation(src):
    """The `{0x2: 'Sandstorm', ...}` pool the monthly map rotation draws from (0x9e3779b1 hash)."""
    pair = r"\s*(" + xb.NUM + r")\s*:\s*(" + xb.STR + r")\s*"
    for m in re.finditer(r"\{(?:" + pair + r",)+" + pair + r"\}", src):
        if "0x9e3779b1" not in src[m.end():m.end() + 1500] and "2654435761" not in src[m.end():m.end() + 1500]:
            continue
        pairs = re.findall(pair, m.group(0)[1:-1])
        return {str(_num(k)): xb.js_unquote(v) for k, v in pairs}
    return None


def _static(src):
    """fetch('maps/' + m.fileName + '.json?v=..') -> {prefix, suffix} and the FTUE entry."""
    m = re.search(r"fetch\s*\(\s*(" + xb.STR + r")\s*\+\s*" + xb.IDENT + r"\s*\[\s*" + xb.IDENT + r"\s*\]\s*" +
                  xb.prop("fileName") + r"\s*\+\s*(" + xb.STR + r")\s*\)", src)
    st = {"prefix": xb.js_unquote(m.group(1)), "suffix": xb.js_unquote(m.group(2))} if m else None
    fm = re.search(r"\{\s*" + xb.key("name") + r"\s*(" + xb.STR + r")\s*,\s*" + xb.key("fileName") + r"\s*(" +
                   xb.STR + r")\s*\}", src)
    ftue = {"name": xb.js_unquote(fm.group(1)), "fileName": xb.js_unquote(fm.group(2))} if fm else None
    return st, ftue


def extract_maps(src):
    gapi = _gapi(src)
    list_path, list_arg, _ = _api_path(src, "getMaps")
    map_path, map_arg, _ = _api_path(src, "getMap")
    if list_arg or not map_arg:
        xb.fail("maps api", "getMaps takes no argument and getMap appends its one (got %s / %s)" % (list_arg, map_arg))
    # startup: getMaps().then(r => r.data.forEach(e => list.push(e))) -- the index is the list position
    if not re.search(xb.prop("getMaps") + r"\s*\(\s*\)\s*" + xb.prop("then") + r"\s*\(\s*function\s*\(\s*(" + xb.IDENT +
                     r")\s*\)\s*\{\s*\1\s*" + xb.prop("data") + r"\s*" + xb.prop("forEach") + r"\s*\(\s*function\s*\(\s*(" +
                     xb.IDENT + r")\s*\)\s*\{\s*" + xb.IDENT + r"\s*" + xb.prop("push") + r"\s*\(\s*\2\s*\)", src):
        xb.fail("maps bootstrap", "no `getMaps().then(r => r.data.forEach(m => list.push(m)))`")
    # init: list[i].didLoad || custom && custom.data ? start : getMap(i).then(r => list[i] = r.data ...)
    im = re.search(r"(" + xb.IDENT + r")\s*\[\s*(" + xb.IDENT + r")\s*\]\s*" + xb.prop("didLoad") + r"\s*\|\|\s*(" +
                   xb.IDENT + r")\s*&&\s*\3\s*" + xb.prop("data") + r"[^;]{0,200}?" + xb.prop("getMap") + r"\s*\(\s*\2\s*\)",
                   src)
    if not im:
        xb.fail("maps init", "no `list[i].didLoad || custom && custom.data ? .. : api.maps.getMap(i)`")
    static, ftue = _static(src)
    return {
        "api_url": gapi,
        "list_path": list_path,
        "map_path": map_path,
        "response": "data",
        "custom_inline_arg": 9,
        "static_file": static,
        "ftue": ftue,
        "presets": _presets(src),
        "rotation": _rotation(src),
    }


# ----------------------------------------------------------------------------- lock + header

def read_lock(path=LOCK):
    try:
        with open(path, encoding="utf8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _norm(name):
    return re.sub(r"[^a-z0-9]+", "", name.lower())


def consistency(bundle, lock):
    """[problem strings]: the bundle's map hints against the fetched list."""
    out = []
    if not lock or not lock.get("maps"):
        return ["%s missing or empty; run tools/krunker/fetch_maps.py fetch" % os.path.relpath(LOCK, ROOT)]
    names = [m["name"] for m in lock["maps"]]
    if bundle.get("api_url") and lock.get("api") and bundle["api_url"].rstrip("/") != lock["api"].rstrip("/"):
        out.append("the bundle's maps API is %s, the lock was fetched from %s" % (bundle["api_url"], lock["api"]))
    if bundle.get("list_path") != "/maps" or bundle.get("map_path") != "/maps/":
        out.append("the bundle's map endpoints changed (%r, %r): fetch_maps.py follows /maps and /maps/<i>"
                   % (bundle.get("list_path"), bundle.get("map_path")))

    def want(index, name, where):
        if index >= len(names):
            out.append("%s names map %d (%s), the list has %d maps" % (where, index, name, len(names)))
        elif _norm(names[index]) != _norm(name):
            out.append("%s says map %d is %r, the list says %r" % (where, index, name, names[index]))

    for p in bundle.get("presets") or []:
        for i, n in zip(p["indices"], p["names"]):
            want(i, n, "preset %r" % p.get("title"))
    for k, n in (bundle.get("rotation") or {}).items():
        want(int(k), n, "the rotation pool")
    return out


def verify_files(lock):
    """[problem strings]: every locked map file present with its sha256 (no network)."""
    out = []
    for i, m in enumerate((lock or {}).get("maps", [])):
        if m.get("index") != i:
            out.append("maps.lock.json entry %d has index %r" % (i, m.get("index")))
        try:
            with open(os.path.join(ROOT, "assets", m["file"]), "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
        except OSError:
            out.append("map %d (%s): assets/%s missing" % (m["index"], m["name"], m["file"]))
            continue
        if digest != m["sha256"]:
            out.append("map %d (%s): assets/%s sha256 %s, maps.lock.json %s" % (
                m["index"], m["name"], m["file"], digest[:16], m["sha256"][:16]))
    return out


def problems(bundle, lock, header_path=OUT_HEADER):
    """Everything --check reports for the map table: header vs (build.json maps, lock), files, hints."""
    if not bundle:
        return ["build.json has no \"maps\" (the bundle's map API anchors); run `make krunker-build FORCE=1`"]
    out = consistency(bundle, lock)
    if lock and lock.get("maps"):
        out += verify_files(lock)
        try:
            with open(header_path, encoding="utf8") as f:
                cur = f.read()
        except OSError:
            cur = None
        if cur != render_header(bundle, lock):
            out.append("%s does not match build.json + maps.lock.json; run `make krunker-build`" %
                       os.path.relpath(header_path, ROOT))
    return out


def render_header(bundle, lock):
    L = []
    w = L.append
    maps = lock["maps"]
    w("/* GENERATED by tools/krunker/extract_build.py (tools/krunker/maps.py) -- do not edit.")
    w(" * Regenerate: make krunker-build      Check: make krunker-build-check")
    w(" * Maps: tools/krunker/fetch_maps.py fetch (KN_FETCH_PROXY=), then make krunker-build")
    w(" *")
    w(" * Krunker's official map list: the `init` packet's mapIndex is a position in it. The live client")
    w(" * fetches the list (GET <API><LIST_PATH>) at startup and each map's JSON on demand (GET")
    w(" * <API><MAP_PATH><index>, {\"data\": map}); a custom game sends its map inline in init arg %d."
      % bundle["custom_inline_arg"])
    w(" * Every map is shipped in assets/ (FILE, pinned by sha256 in tools/krunker/maps.lock.json);")
    w(" * the list was fetched %s from %s." % (lock.get("fetched") or "?", c_comment(lock.get("list_url") or "?")))
    w(" */")
    w("#ifndef KRUNKER_MAPS_H")
    w("#define KRUNKER_MAPS_H")
    w("")
    defs = [
        ("KRUNKER_MAPS_SCHEMA", "%d" % SCHEMA, None),
        ("KRUNKER_MAPS_API_URL", xb.c_str(bundle["api_url"]), "GAPI_URI's prod default"),
        ("KRUNKER_MAPS_LIST_PATH", xb.c_str(bundle["list_path"]), "api.maps.getMaps"),
        ("KRUNKER_MAP_PATH", xb.c_str(bundle["map_path"]), "api.maps.getMap: + the map index"),
        ("KRUNKER_MAPS_INIT_CUSTOM_ARG", "%d" % bundle["custom_inline_arg"], "init: {data, id, creator, ...} of a custom game"),
        ("KRUNKER_OFFICIAL_MAP_COUNT", "%d" % len(maps), None),
    ]
    width = max(len("#define %s %s" % (n, v)) for n, v, _ in defs) + 2
    for n, v, c in defs:
        line = "#define %s %s" % (n, v)
        w(line if not c else "%s%s/* %s */" % (line, " " * (width - len(line)), c))
    w("")
    w("/* X(index, name (the list's), file (assets-relative), objects) */")
    w("#define KRUNKER_OFFICIAL_MAPS(X) \\")
    for m in maps:
        w("    X(%d, %s, %s, %d) \\" % (m["index"], xb.c_str(m["name"]), xb.c_str(m["file"]), m["objects"]))
    w("")
    w("")
    w("#endif /* KRUNKER_MAPS_H */")
    return "\n".join(L) + "\n"


def c_comment(s):
    return s.replace("*/", "* /")
