#!/usr/bin/env python3
"""Krunker Strike (FRVR) reference extractor: the Strike client bundle -> strike_data.json.

Krunker Strike is FRVR's fork of the Krunker JavaScript engine (a Vite bundle with three.js,
served as the Discord Activity 1011683823555199066; build "krunkerfrvr"). Sources, all read as
TEXT ONLY:

  primary    the local snapshot of build 1.37.2 (2026-06-13), --build
             ~/src/KrunkerStrike_v1.37.2/public: the pristine minified assets/index-*.js (not the
             hand-renamed *.deob.js / game.js), assets/index-*.css, index.html, maps/*.json
  secondary  module S0's Internet Archive mirror, --live ~/.cache/kn-strike/live (provenance in
             live/README.md): build 1.27.0 (2024-02-08) for the build-to-build diff and what 1.37.2
             lacks, build 1.7.0 CSS / entry, and the 2025 /.proxy/maps captures (the map pool)

Output sections:

  meta      versions, source sha256 + size          summary   golden counts
  modes     mode enum + every mode object (function-valued keys listed as hooks)
  classes   ids, names, health, speed, regen, loadouts, unlocks, classConfig defaults applied;
            classes_vs_v10: per-field diff against shared/tests/data/v10/classes.json
  weapons   every weapon stat object; weapons_vs_v10: per-field diff against
            shared/tests/data/v10/weapons.json (what shared/tests/weapons_v10_test.c checks
            shared/src/rules/v10/weapons_v10.c against)
  maps      catalogue (index = server map index), the build's map files with per-map settings,
            the 7-map pool, the remote mapPool rotation rule
  packets   client->server / server->client name tables, literal frame names, wire framing
  gateway   FRVR gateway REST routes (method, path, body keys, error), directors, regions, headers,
            the Discord URL mappings
  items     item types, unlock origins, reward/compensation enums, rarities, packages, crate tiers
  config    constants (player cap, tick rates), custom-game settings (match minutes, warmup,
            rounds, intermission), class/weapon config tables, player cap, timing
  i18n      every English UI string (key -> text)
  ui        screens (component id, options, rendered CSS selectors), mobile-vs-desktop detection and
            switch sites, UI scale, stylesheet fonts / colour tokens / keyframes / transitions / BEM
            blocks per build, entry pages (loading-screen markup, default feature flags, FTUE flows)
  cdn       asset paths the bundle builds on cdn.frvr.com
  builds_diff  what changed from 1.27.0 to 1.37.2

Safety: every input is untrusted data. Nothing is executed or evaluated: JavaScript literals are
read by a recursive-descent literal reader, arithmetic like `1*.00095` by a tokenising calculator,
JSON by the json module. No subprocess, no network. Identifiers in the bundle are minified and
change per build, so every table is found from a string literal or field-name anchor and
identifiers are only resolved from those anchors.

usage:
  python3 -I tools/strike/extract_strike.py [--build DIR] [--live DIR] [--out FILE] [--cache-out FILE]
  python3 -I tools/strike/extract_strike.py --print            JSON to stdout, nothing written
  python3 -I tools/strike/extract_strike.py --check            committed strike_data.json current?

--out defaults to tools/strike/strike_data.json, --cache-out to <live>/strike.json (the superset:
also the 1.7.0 bundle classes and the raw @keyframes bodies). Any anchor that is not found fails
the run (exit 1) naming it; nothing is written then.
"""

import argparse
import hashlib
import json
import os
import re
import sys

GENERATOR_REV = 1

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_JSON = os.path.join(ROOT, "tools", "strike", "strike_data.json")
V10_WEAPONS = os.path.join(ROOT, "shared", "tests", "data", "v10", "weapons.json")
V10_CLASSES = os.path.join(ROOT, "shared", "tests", "data", "v10", "classes.json")
DEFAULT_LIVE = os.path.expanduser("~/.cache/kn-strike/live")
DEFAULT_BUILD = os.path.expanduser("~/src/KrunkerStrike_v1.37.2/public")

# the archive mirror (relative to --live)
ARCHIVE_BUNDLE = "1.27.0/index-2a449dcc.js"
ARCHIVE_CSS = "1.27.0/index-9a3ade8d.css"
ARCHIVE_ENTRY = "1.27.0/entry.html"
OLD_BUNDLE = "1.7.0/index-15c20563.js"
OLD_CSS = "1.7.0/index-29809c93.css"
OLD_ENTRY = "1.7.0/entry.html"
POOL_DIR = "maps"


class ExtractError(Exception):
    pass


def fail(anchor, msg):
    raise ExtractError("%s: %s" % (anchor, msg))


def log(msg):
    print("extract_strike: " + msg, file=sys.stderr)


# ----------------------------------------------------------------------------- JS literal reader

IDENT = r"(?:[A-Za-z_$][\w$]*)"
_ESC = {"n": "\n", "r": "\r", "t": "\t", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}
_REGEX_PREV = set("(,=:[!&|?{};+-*%<>~^")


def js_unquote(lit):
    """Value of a JS string literal including its quotes (UTF-16 escapes combined)."""
    body = lit[1:-1]
    units = []
    i, n = 0, len(body)
    while i < n:
        c = body[i]
        if c != "\\":
            units.extend(_utf16(c))
            i += 1
            continue
        i += 1
        if i >= n:
            break
        e = body[i]
        if e == "x":
            units.append(int(body[i + 1:i + 3], 16))
            i += 3
        elif e == "u":
            if body[i + 1:i + 2] == "{":
                j = body.index("}", i)
                units.extend(_utf16(chr(int(body[i + 2:j], 16))))
                i = j + 1
            else:
                units.append(int(body[i + 1:i + 5], 16))
                i += 5
        elif e in _ESC and not (e == "0" and i + 1 < n and body[i + 1].isdigit()):
            units.append(ord(_ESC[e]))
            i += 1
        elif e in "\r\n\u2028\u2029":
            i += 1
        else:
            units.extend(_utf16(e))
            i += 1
    return b"".join(u.to_bytes(2, "little") for u in units).decode("utf-16-le", "surrogatepass")


def _utf16(ch):
    o = ord(ch)
    if o < 0x10000:
        return [o]
    o -= 0x10000
    return [0xD800 + (o >> 10), 0xDC00 + (o & 0x3FF)]


def skip_string(s, i):
    """s[i] is a quote; index just past the closing quote."""
    q = s[i]
    i += 1
    n = len(s)
    while i < n and s[i] != q:
        if s[i] == "\\":
            i += 1
        elif s[i] == "\n" and q != "`":
            break
        i += 1
    return i + 1


def skip_template(s, i):
    """s[i] is a backquote; index past the closing one (nested ${...} skipped)."""
    i += 1
    n = len(s)
    while i < n:
        c = s[i]
        if c == "\\":
            i += 2
            continue
        if c == "`":
            return i + 1
        if c == "$" and s[i + 1:i + 2] == "{":
            i = skip_balanced(s, i + 1) + 1
            continue
        i += 1
    return n


def skip_regex(s, i):
    """s[i] is '/' starting a regex literal; index past it and its flags."""
    i += 1
    n = len(s)
    in_class = False
    while i < n:
        c = s[i]
        if c == "\\":
            i += 2
            continue
        if c == "\n":
            break
        if in_class:
            if c == "]":
                in_class = False
        elif c == "[":
            in_class = True
        elif c == "/":
            i += 1
            while i < n and (s[i].isalnum() or s[i] in "_$"):
                i += 1
            return i
        i += 1
    return i


def _prev_sig(s, i):
    j = i - 1
    while j >= 0 and s[j] in " \t\r\n":
        j -= 1
    if j < 0:
        return "", j
    return s[j], j


def _regex_allowed(s, i):
    c, j = _prev_sig(s, i)
    if c == "" or c in _REGEX_PREV:
        return True
    if c.isalnum() or c in "_$":
        k = j
        while k >= 0 and (s[k].isalnum() or s[k] in "_$"):
            k -= 1
        return s[k + 1:j + 1] in ("return", "typeof", "case", "in", "of", "void", "delete", "throw")
    return False


def skip_balanced(s, i):
    """s[i] is ( [ or {; index of the matching closer (strings, templates, regexes, comments skipped)."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack = []
    n = len(s)
    while i < n:
        c = s[i]
        if c in "'\"":
            i = skip_string(s, i)
            continue
        if c == "`":
            i = skip_template(s, i)
            continue
        if c == "/":
            nx = s[i + 1:i + 2]
            if nx == "/":
                e = s.find("\n", i)
                i = n if e < 0 else e
                continue
            if nx == "*":
                e = s.find("*/", i + 2)
                i = n if e < 0 else e + 2
                continue
            if _regex_allowed(s, i):
                i = skip_regex(s, i)
                continue
        if c in pairs:
            stack.append(pairs[c])
        elif c in ")]}":
            if not stack or stack.pop() != c:
                return -1
            if not stack:
                return i
        i += 1
    return -1


def skip_expr(s, i, stops=",}]);"):
    """Index of the first top-level character of `stops` at or after i (end of an expression)."""
    n = len(s)
    while i < n:
        c = s[i]
        if c in stops:
            return i
        if c in "'\"":
            i = skip_string(s, i)
        elif c == "`":
            i = skip_template(s, i)
        elif c in "([{":
            e = skip_balanced(s, i)
            if e < 0:
                return n
            i = e + 1
        elif c == "/" and _regex_allowed(s, i) and s[i + 1:i + 2] not in "/*":
            i = skip_regex(s, i)
        else:
            i += 1
    return n


def _ws(s, i):
    """Skip whitespace and comments (only used at token boundaries of literals)."""
    n = len(s)
    while i < n:
        c = s[i]
        if c in " \t\r\n":
            i += 1
        elif c == "/" and s[i + 1:i + 2] == "/":
            e = s.find("\n", i)
            i = n if e < 0 else e + 1
        elif c == "/" and s[i + 1:i + 2] == "*":
            e = s.find("*/", i + 2)
            i = n if e < 0 else e + 2
        else:
            break
    return i


_NUM_RE = re.compile(r"(?:0[xX][0-9a-fA-F]+|0[bB][01]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)")
_CALC_TOK = re.compile(r"\s*(?:(0[xX][0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)|"
                       r"(Math\.PI)|([-+*/()]))")


def num_value(tok):
    t = tok.strip()
    if t[:2] in ("0x", "0X"):
        return int(t, 16)
    if t[:2] in ("0b", "0B"):
        return int(t[2:], 2)
    v = float(t)
    return int(v) if v.is_integer() and "e" not in t.lower() and "." not in t else _norm(v)


def _norm(v):
    if isinstance(v, float) and v.is_integer() and abs(v) < 1e15:
        return int(v)
    if isinstance(v, float):
        return float(repr(round(v, 15)))
    return v


def calc(text):
    """Value of an arithmetic expression of number literals, Math.PI, + - * / and parentheses.
    Returns None for anything else. Tokenised and computed in Python; nothing is evaluated."""
    toks = []
    pos = 0
    text = text.strip()
    if not text:
        return None
    while pos < len(text):
        m = _CALC_TOK.match(text, pos)
        if not m or m.end() == pos:
            return None
        if m.group(1):
            toks.append(("n", float(num_value(m.group(1)))))
        elif m.group(2):
            toks.append(("n", 3.141592653589793))
        else:
            toks.append(("op", m.group(3)))
        pos = m.end()
    st = {"i": 0}

    def peek():
        return toks[st["i"]] if st["i"] < len(toks) else (None, None)

    def add():
        v = mul()
        while peek() in (("op", "+"), ("op", "-")):
            op = toks[st["i"]][1]
            st["i"] += 1
            r = mul()
            v = v + r if op == "+" else v - r
        return v

    def mul():
        v = unary()
        while peek() in (("op", "*"), ("op", "/")):
            op = toks[st["i"]][1]
            st["i"] += 1
            r = unary()
            if op == "/" and r == 0:
                raise ValueError("division by zero")
            v = v * r if op == "*" else v / r
        return v

    def unary():
        k = peek()
        if k == ("op", "-"):
            st["i"] += 1
            return -unary()
        if k == ("op", "+"):
            st["i"] += 1
            return unary()
        if k == ("op", "("):
            st["i"] += 1
            v = add()
            if peek() != ("op", ")"):
                raise ValueError("unbalanced")
            st["i"] += 1
            return v
        if k[0] == "n":
            st["i"] += 1
            return k[1]
        raise ValueError("bad token")

    try:
        v = add()
    except (ValueError, OverflowError):
        return None
    if st["i"] != len(toks):
        return None
    return _norm(v)


def scalar(text):
    """A primitive value from expression text, or a {"$expr": text} marker."""
    t = text.strip()
    if t in ("!0", "true"):
        return True
    if t in ("!1", "false"):
        return False
    if t in ("null",):
        return None
    if t in ("void 0", "undefined"):
        return {"$undefined": True}
    if len(t) >= 2 and t[0] in "'\"" and skip_string(t, 0) == len(t):
        return js_unquote(t)
    if len(t) >= 2 and t[0] == "`" and skip_template(t, 0) == len(t) and "${" not in t:
        return t[1:-1].replace("\\`", "`")
    v = calc(t)
    if v is not None:
        return v
    # string concatenation of literals: "a"+"b"
    parts = split_top(t, "+")
    if len(parts) > 1 and all(p.strip()[:1] in "'\"" and skip_string(p.strip(), 0) == len(p.strip())
                              for p in parts):
        return "".join(js_unquote(p.strip()) for p in parts)
    if len(t) > 240:
        t = t[:240] + "..."
    return {"$expr": t}


def split_top(t, sep):
    out = []
    depth = 0
    i = 0
    start = 0
    n = len(t)
    while i < n:
        c = t[i]
        if c in "'\"":
            i = skip_string(t, i)
            continue
        if c == "`":
            i = skip_template(t, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            out.append(t[start:i])
            start = i + 1
        i += 1
    out.append(t[start:])
    return out


def _is_function_start(s, i):
    if s.startswith("function", i) and not (s[i + 8:i + 9].isalnum() or s[i + 8:i + 9] in "_$"):
        return True
    if s.startswith("async", i) and s[i + 5:i + 6] in " (":
        return True
    if s[i] == "(":
        e = skip_balanced(s, i)
        return e > 0 and s[_ws(s, e + 1):_ws(s, e + 1) + 2] == "=>"
    m = re.compile(IDENT + r"\s*=>").match(s, i)
    return bool(m)


def parse_value(s, i):
    """Read the JS value at s[i]. Returns (value, end index). Objects -> dict, arrays -> list,
    primitives -> Python values, functions -> {"$fn": True}, anything else -> {"$expr": text}."""
    i = _ws(s, i)
    c = s[i:i + 1]
    if c == "{" or c == "[":
        v, e = (parse_object if c == "{" else parse_array)(s, i)
        j = _ws(s, e)
        if s[j:j + 1] in (",", "}", "]", ")", ";", ""):
            return v, e
        end = skip_expr(s, i)  # e.g. [..].map(..): keep it as an expression
        return {"$expr": s[i:end][:240]}, end
    if _is_function_start(s, i):
        end = skip_expr(s, i)
        return {"$fn": True}, end
    end = skip_expr(s, i)
    return scalar(s[i:end]), end


def parse_object(s, i):
    assert s[i] == "{"
    out = {}
    i += 1
    n = len(s)
    while i < n:
        i = _ws(s, i)
        c = s[i]
        if c == "}":
            return out, i + 1
        if c == ",":
            i += 1
            continue
        if s.startswith("...", i):
            e = skip_expr(s, i + 3)
            out.setdefault("$spread", []).append(s[i + 3:e].strip())
            i = e
            continue
        # key
        if c in "'\"":
            e = skip_string(s, i)
            k = js_unquote(s[i:e])
            i = e
        elif c == "[":
            e = skip_balanced(s, i)
            k = "[" + s[i + 1:e].strip() + "]"
            i = e + 1
        else:
            m = re.compile(r"(?:get |set |async )?(" + IDENT + r"|\d+(?:\.\d+)?)").match(s, i)
            if not m:
                fail("object literal", "unreadable key at %d: %r" % (i, s[i:i + 40]))
            k = m.group(1)
            i = m.end()
        i = _ws(s, i)
        if s[i] == ":":
            v, i = parse_value(s, i + 1)
            out[k] = v
        elif s[i] == "(":  # method shorthand
            e = skip_balanced(s, i)
            b = _ws(s, e + 1)
            if s[b] != "{":
                fail("object literal", "method %s without body at %d" % (k, i))
            i = skip_balanced(s, b) + 1
            out[k] = {"$fn": True}
        else:  # shorthand property {a, b}
            out[k] = {"$expr": k}
    fail("object literal", "unterminated object")


def parse_array(s, i):
    assert s[i] == "["
    out = []
    i += 1
    n = len(s)
    expect_value = True
    while i < n:
        i = _ws(s, i)
        c = s[i]
        if c == "]":
            return out, i + 1
        if c == ",":
            if expect_value:
                out.append(None)  # hole
            expect_value = True
            i += 1
            continue
        if s.startswith("...", i):
            e = skip_expr(s, i + 3)
            out.append({"$spread": s[i + 3:e].strip()})
            i = e
        else:
            v, i = parse_value(s, i)
            out.append(v)
        expect_value = False
    fail("array literal", "unterminated array")


def literal_at(s, pattern, anchor, group_pos=None):
    """Parse the object/array literal that starts where `pattern` matches (at the match's first
    { or [ after group `group_pos`). Exactly the first match is used; none -> fail(anchor)."""
    m = re.search(pattern, s)
    if not m:
        fail(anchor, "pattern %r not found" % pattern)
    start = m.end(group_pos) if group_pos else m.start()
    k = start
    while s[k] not in "{[":
        k += 1
    v, _ = parse_value(s, k)
    return v, m


_ASSIGN_RX = re.compile(r"(?:[,;{}()\s]|\b(?:let|const|var)\s)([A-Za-z_$][\w$]*)=(?![=>])")
_ASSIGN_INDEX = {}


def _assignments(s):
    """name -> positions just past `name=` (one scan per source, cached)."""
    key = (id(s), len(s))
    idx = _ASSIGN_INDEX.get(key)
    if idx is None:
        idx = {}
        for m in _ASSIGN_RX.finditer(s):
            idx.setdefault(m.group(1), []).append(m.end())
        _ASSIGN_INDEX.clear()
        _ASSIGN_INDEX[key] = idx
    return idx


def resolve_ident(s, name, before, depth=0):
    """The value assigned to identifier `name` closest before index `before` (`name=<value>`).
    Assignments of a one-letter identifier (a minified parameter, e.g. `maxP=i` in a setter) are
    skipped; an alias to another identifier is followed up to 3 levels. Returns (value, raw text)
    or None."""
    pos = [p for p in _assignments(s).get(name, ()) if p < before]
    for p in reversed(pos):
        v, e = parse_value(s, p)
        raw = s[p:e].strip()[:240]
        if isinstance(v, dict) and "$expr" in v and re.fullmatch(IDENT, v["$expr"]):
            if len(v["$expr"]) == 1:
                continue
            if depth < 3:
                r = resolve_ident(s, v["$expr"], p, depth + 1)
                if r is not None:
                    return r[0], raw
        return v, raw
    return None


def resolve_refs(s, v, before):
    """Replace bare identifier / member references inside a parsed literal by what they name:
    a `function NAME(` declaration -> {"$fn": true}, a literal -> its value, `E.KEY` of an object
    literal -> that value; anything else -> {"$ref": "unresolved"} (minified names change per
    build, so they are not kept)."""
    if isinstance(v, list):
        return [resolve_refs(s, x, before) for x in v]
    if not isinstance(v, dict):
        return v
    if "$expr" in v and len(v) == 1:
        t = v["$expr"]
        m = re.fullmatch(r"(" + IDENT + r")(?:\.(" + IDENT + r"))?", t)
        if not m:
            return v
        name, member = m.group(1), m.group(2)
        if name in ("Math", "Number", "JSON", "window", "this"):
            return v
        if member is None and re.search(r"function " + re.escape(name) + r"\(", s):
            return {"$fn": True}
        r = resolve_ident(s, name, before)
        if r is None:
            r = resolve_ident(s, name, len(s))
        if r is None:
            return {"$ref": "unresolved"}
        val = r[0]
        if member is not None:
            if isinstance(val, dict) and member in val and not (isinstance(val[member], dict) and "$expr" in val[member]):
                return val[member]
            return {"$ref": "unresolved"}
        if isinstance(val, dict) and ("$expr" in val or "$fn" in val):
            return {"$fn": True} if "$fn" in val else {"$ref": "unresolved"}
        if isinstance(val, (dict, list)):
            return {"$ref": "unresolved"}  # a shared table: not inlined
        return val
    return {k: resolve_refs(s, x, before) for k, x in v.items()}


# ----------------------------------------------------------------------------- helpers

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_text(path):
    with open(path, "rb") as f:
        data = f.read()
    return data.decode("utf-8", "replace"), data


def strip_fns(v):
    """Data fields only: function-valued keys removed (listed by the caller)."""
    if isinstance(v, dict):
        return {k: strip_fns(x) for k, x in v.items() if not (isinstance(x, dict) and "$fn" in x)}
    if isinstance(v, list):
        return [strip_fns(x) for x in v]
    return v


def fn_keys(v):
    return sorted(k for k, x in v.items() if isinstance(x, dict) and "$fn" in x)


def find_all_literals(s, pattern):
    """Parse every object literal whose text starts with `pattern` (regex starting at '{')."""
    out = []
    for m in re.finditer(pattern, s):
        v, _ = parse_object(s, m.start())
        out.append((m.start(), v))
    return out


# ----------------------------------------------------------------------------- sections

def extract_meta(s):
    m = re.search(r'(?<![\w$])([\w$]{1,40})="(\d+\.\d+\.\d+)",([\w$]+)="(\d+\.\d+\.\d+)",([\w$]+)="([A-Za-z0-9]{32})"', s)
    if not m:
        fail("meta", "no `X=\"a.b.c\",Y=\"a.b.c\",Z=\"<32 chars>\"` version triple")
    gv, ev, bv = m.group(1), m.group(3), m.group(5)
    obj = re.search(r"gameVersion:" + re.escape(gv) + r",editorVersion:" + re.escape(ev) +
                    r",buildVersion:" + re.escape(bv), s)
    if not obj:
        fail("meta", "constants object does not name gameVersion/editorVersion/buildVersion")
    three = re.search(r'REVISION\s*=\s*"(\d+)"|"(\d{3})"===?\s*[\w$.]*REVISION|const [\w$]+="(\d{3})"', s)
    return {"game_version": m.group(2), "editor_version": m.group(4), "build_version": m.group(6),
            "three_revision": next((g for g in three.groups() if g), None) if three else None}


def extract_constants(s):
    """The `le={updateMaxPlayers:...,gameVersion:...}` constants object, each identifier resolved."""
    m = re.search(r"(?<![\w$])([\w$]{1,40})=\{updateMaxPlayers:", s)
    if not m:
        fail("config.constants", "`X={updateMaxPlayers:` not found")
    start = m.end() - len("updateMaxPlayers:") - 1
    obj, _ = parse_object(s, start)
    out = {}
    wanted = ("maxPlayers", "maxBots", "maxPlayersTotal", "gamesPerServer", "gamesToCreate",
              "gameStartDelay", "serverSendFrequency", "serverTickFrequency", "serverSyncFrequency",
              "serverTickRate", "interpolation", "interpPDelay", "stateHistory", "kickTimer",
              "maxPayloadLength", "voteForfeitTimer", "pingCount", "serverBrowserRate",
              "cdnURL", "cdnURLAssets", "cdnURLAlt", "socialGameId", "movDirs", "isProd",
              "raidsInPubs", "raidMapId", "maxObfPing", "maxObfPad", "minObfPad")
    for k in wanted:
        v = obj.get(k)
        if isinstance(v, dict) and "$expr" in v and re.fullmatch(IDENT, v["$expr"]):
            r = resolve_ident(s, v["$expr"], start)
            if r is None:
                out[k] = {"$unresolved": v["$expr"]}
                continue
            val, raw = r
            if isinstance(val, dict) and "$expr" in val:
                # `Number({}.MAX_PLAYERS)||10` -> 10 (the env override is empty in the client)
                fb = re.search(r"\|\|\s*([-.\w]+)\s*$", val["$expr"])
                fbv = scalar(fb.group(1)) if fb else None
                if fbv is not None and not isinstance(fbv, dict):
                    out[k] = {"value": fbv, "raw": raw}
                    continue
                # a product of identifiers / numbers: resolve one level
                out[k] = {"value": None, "raw": raw}
                continue
            out[k] = val
        else:
            out[k] = v
    # S3 = 1e3 / Rk style: resolve identifiers inside `raw` one level
    for k, v in list(out.items()):
        if isinstance(v, dict) and v.get("value") is None and "raw" in v:
            expr = v["raw"]
            ids = set(re.findall(r"(?<![\w$.])([A-Za-z_$][\w$]*)", expr))
            sub = expr
            ok = True
            for ident in ids:
                r = resolve_ident(s, ident, start)
                if r is None:
                    ok = False
                    break
                val = r[0]
                if isinstance(val, dict) and "$expr" in val:
                    fb = re.search(r"\|\|\s*([-.\w]+)\s*$", val["$expr"])
                    val = scalar(fb.group(1)) if fb else None
                if not isinstance(val, (int, float)) or isinstance(val, bool):
                    ok = False
                    break
                sub = re.sub(r"(?<![\w$.])" + re.escape(ident) + r"(?![\w$])", repr(val), sub)
            if ok:
                c = calc(sub)
                if c is not None:
                    v["value"] = c
    return out, obj, start


def extract_settings_tables(s, const_obj, const_pos):
    """serverConfig (custom-game settings), classConfig, the custom-server presets."""
    out = {}
    for key in ("serverConfig", "classConfig", "weaponConfig"):
        v = const_obj.get(key)
        if not (isinstance(v, dict) and "$expr" in v):
            fail("config." + key, "not an identifier in the constants object")
        r = resolve_ident(s, v["$expr"], const_pos)
        if r is None or not isinstance(r[0], list):
            fail("config." + key, "identifier %s has no array literal" % v["$expr"])
        rows = []
        for row in resolve_refs(s, r[0], const_pos):
            if isinstance(row, dict):
                rows.append(strip_fns(row))
        out[key] = rows
    if re.search(r'=\[\{title:"Custom Game Setup"\}', s):
        presets, _ = literal_at(s, r'=\[\{title:"Custom Game Setup"\}', "config.serverPresets")
        out["serverPresets"] = strip_fns(presets)
    else:
        out["serverPresets"] = None  # 1.37.2 dropped the custom-server preset table
    return out


def extract_modes(s):
    enum, _ = literal_at(s, r"(?<![\w$])[\w$]{1,40}=\{FREE_FOR_ALL:0,TEAM_DEATHMATCH:1", "modes.enum")
    m = re.search(r"(?<![\w$])([\w$]{1,40})=\{\[([\w$]+)\.FREE_FOR_ALL\]:([\w$]+),", s)
    if not m:
        fail("modes.map", "`X={[E.FREE_FOR_ALL]:<mode>,` not found")
    table, _ = parse_object(s, m.end(1) + 1)
    by_ident = {}
    for k, v in table.items():
        km = re.fullmatch(r"\[[\w$]+\.([A-Z_]+)\]", k)
        if not km or not (isinstance(v, dict) and "$expr" in v):
            fail("modes.map", "unexpected entry %r" % k)
        by_ident[v["$expr"]] = km.group(1)
    modes = []
    for ident, ename in by_ident.items():
        dm = re.search(r"(?:[,;\s]|const |let |var )" + re.escape(ident) + r"=\{id:\"", s)
        if not dm:
            fail("modes." + ename, "definition of %s not found" % ident)
        obj, _ = parse_object(s, dm.end() - len('{id:"'))
        obj = resolve_refs(s, obj, dm.start())
        hooks = fn_keys(obj)
        data = strip_fns(obj)
        modes.append({"index": enum[ename], "enum": ename, "id": data.get("id"), "name": data.get("name"),
                      "party": bool(data.get("party")), "teams": bool(data.get("teams")),
                      "scoreLimit": data.get("scoreLimit"), "gameTime": data.get("gameTime"),
                      "hooks": hooks, "data": data})
    modes.sort(key=lambda x: x["index"])
    return {"enum": enum, "list": modes}


def extract_classes(s, class_config):
    v, m = literal_at(s, r'(?:let|const|var)\s+[\w$]+=\[\{id:0,name:"Triggerman"', "classes")
    v = resolve_refs(s, v, m.start())
    defaults = {r["varN"]: r.get("def") for r in class_config if isinstance(r, dict) and "varN" in r}
    out = []
    for c in v:
        c = strip_fns(c)
        eff = dict(defaults)
        eff.update({k: x for k, x in c.items()})
        out.append({"id": c["id"], "name": c["name"], "health": c.get("health"),
                    "speed": c.get("speed"), "regen": c.get("regen"), "loadout": c.get("loadout"),
                    "secondary": c.get("secondary"), "rarity": c.get("rarity"),
                    "minRec": c.get("minRec"), "unlockable": c.get("unlockable"),
                    "description": c.get("description"), "data": c, "effective": eff})
    return out


def extract_weapons(s):
    v, m = literal_at(s, r'(?:let|const|var)\s+[\w$]+=\[\{name:"Sniper Rifle",src:"weapon_1"', "weapons")
    v = resolve_refs(s, v, m.start())
    out = []
    for i, w in enumerate(v):
        if not isinstance(w, dict):
            fail("weapons", "entry %d is not an object" % i)
        out.append({"index": i, "name": w.get("name"), "data": strip_fns(w), "hooks": fn_keys(w)})
    return out


def _num_eq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))
    return a == b


def diff_objects(a, b):
    """Field diff a (Strike) vs b (v10): changed / strike_only / v10_only (top level; nested
    objects compared whole)."""
    changed = {}
    for k in sorted(set(a) & set(b)):
        if not _deep_eq(a[k], b[k]):
            changed[k] = {"strike": a[k], "v10": b[k]}
    return {"changed": changed,
            "strike_only": {k: a[k] for k in sorted(set(a) - set(b))},
            "v10_only": {k: b[k] for k in sorted(set(b) - set(a))}}


def _deep_eq(a, b):
    if isinstance(a, dict) and isinstance(b, dict):
        return set(a) == set(b) and all(_deep_eq(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_deep_eq(x, y) for x, y in zip(a, b))
    return _num_eq(a, b)


def norm_v10(v):
    """shared/tests/data/v10/*.json keep unevaluated expressions as "<expr:Math['PI'] / 2>"."""
    if isinstance(v, dict):
        return {k: norm_v10(x) for k, x in v.items()}
    if isinstance(v, list):
        return [norm_v10(x) for x in v]
    if isinstance(v, str):
        m = re.fullmatch(r"<expr:(.*)>", v)
        if m:
            c = calc(m.group(1).replace("Math['PI']", "Math.PI"))
            return c if c is not None else v
    return v


def weapons_vs_v10(weapons, v10):
    by_src = {}
    for i, w in enumerate(v10):
        by_src.setdefault((w.get("name"), w.get("src")), i)
    out = []
    for w in weapons:
        d = w["data"]
        j = by_src.get((d.get("name"), d.get("src")))
        if j is None and w["index"] < len(v10) and v10[w["index"]].get("name") == d.get("name"):
            j = w["index"]
        if j is None:
            out.append({"index": w["index"], "name": d.get("name"), "v10_index": None, "status": "strike_only"})
            continue
        df = diff_objects(d, {k: x for k, x in v10[j].items()})
        out.append({"index": w["index"], "name": d.get("name"), "v10_index": j,
                    "status": "same" if not (df["changed"] or df["strike_only"]) else "changed",
                    "changed": df["changed"], "strike_only": sorted(df["strike_only"]),
                    "v10_only": sorted(df["v10_only"])})
    strike_names = {(w["data"].get("name"), w["data"].get("src")) for w in weapons}
    missing = [{"v10_index": i, "name": w.get("name")} for i, w in enumerate(v10)
               if (w.get("name"), w.get("src")) not in strike_names]
    return {"per_weapon": out, "v10_not_in_strike": missing}


def classes_vs_v10(classes, v10):
    out = []
    by_id = {c.get("id", i): c for i, c in enumerate(v10)}
    for c in classes:
        ref = by_id.get(c["id"])
        if ref is None:
            out.append({"id": c["id"], "name": c["name"], "status": "strike_only"})
            continue
        df = diff_objects(c["data"], ref)
        out.append({"id": c["id"], "name": c["name"], "v10_name": ref.get("name"),
                    "changed": df["changed"], "strike_only": sorted(df["strike_only"])})
    return out


def _map_file_info(path, files, key):
    with open(path, "rb") as f:
        data = f.read()
    files[key] = {"sha256": sha256(data), "size": len(data)}
    try:
        mj = json.loads(data.decode("utf-8"))
    except ValueError as e:
        fail("maps", "%s: %s" % (key, e))
    if not isinstance(mj, dict):
        fail("maps", "%s: not a JSON object" % key)
    settings = {k: v for k, v in mj.items() if k not in ("objects", "spawns", "xyz", "colors", "tris")}
    spawns = mj.get("spawns") or []
    teams = {}
    for sp in spawns:
        t = sp[3] if isinstance(sp, list) and len(sp) > 3 else 0
        teams[str(t)] = teams.get(str(t), 0) + 1
    return {"name": mj.get("name"), "objects": len(mj.get("objects") or []), "spawns": len(spawns),
            "spawns_by_team": dict(sorted(teams.items())), "sha256": files[key]["sha256"],
            "size": len(data), "settings": settings}


def _game_config_refresh(s):
    m = re.search(r"const ([\w$]+)=([\d.e*]+),([\w$]+)=([\d.e*]+);this\.fetchGameConfig=async function", s)
    if not m:
        return None
    return {"ok": calc(m.group(2)), "retry": calc(m.group(4))}


def extract_maps(s, build, live, files):
    """Catalogue (index = the server's map index, the v7 maps.js order), the build's map files with
    their per-map settings, the archived pool captures and the remote mapPool mechanism."""
    cat, m = literal_at(s, r'(?:let|const|var)\s+([\w$]+)=\[\{name:"Burg",fileName:"', "maps.catalogue", 0)
    cat_ident = re.match(r'(?:let|const|var)\s+([\w$]+)=', m.group(0)).group(1)
    catalogue = [{"index": i, "name": x.get("name"), "fileName": x.get("fileName")} for i, x in enumerate(cat)]
    fm = re.search(r'fetch\("(maps/)"\+[\w$]+\[[\w$]+\]\.fileName\+"(\.json[^"]*)"', s)
    pool_rx = re.search(r'remoteFeatures\.mapPool,[\w$]+=\[\];if\([\w$]+\)\{const [\w$]+=[\w$]+\.isProd\?'
                        r'[\w$]+\.prod:[\w$]+\.staging;.{0,120}?' + re.escape(cat_ident) +
                        r'\.findIndex\([\w$]+=>[\w$]+\.name===[\w$]+\[[\w$]+\]\.mapName\).{0,80}?\.mapWeight', s)
    mobile = re.search(r'useMobileModes&&this\.map\.mobileModes', s)
    local = []
    mdir = os.path.join(build, "maps")
    if not os.path.isdir(mdir):
        fail("maps.local", "%s missing" % mdir)
    by_file = {}
    for fn in sorted(os.listdir(mdir)):
        if fn.endswith(".json"):
            info = _map_file_info(os.path.join(mdir, fn), files, "build:maps/" + fn)
            info["file"] = fn
            idx = [c["index"] for c in catalogue if c["fileName"] == fn[:-5]]
            info["catalogue_index"] = idx[0] if idx else None
            local.append(info)
            by_file[fn[:-5]] = info
    pool = []
    pdir = os.path.join(live, POOL_DIR)
    if not os.path.isdir(pdir):
        fail("maps.pool", "%s missing (run the S0 mirror)" % pdir)
    for fn in sorted(os.listdir(pdir)):
        if not fn.endswith(".json"):
            continue
        info = _map_file_info(os.path.join(pdir, fn), files, "archive:" + POOL_DIR + "/" + fn)
        same = [x["file"] for x in local if x["sha256"] == info["sha256"]]
        key = fn[:-5].lower()
        cat = [c for c in catalogue if c["name"].replace(" ", "").replace("_", "").lower() == key]
        match = by_file.get(cat[0]["fileName"]) if cat else None
        pool.append({"archive_file": fn, "name": info["name"], "archive_sha256": info["sha256"],
                     "objects": info["objects"], "spawns": info["spawns"],
                     "catalogue_index": cat[0]["index"] if cat else None,
                     "catalogue_name": cat[0]["name"] if cat else None,
                     "build_file": match["file"] if match else None,
                     "build_file_objects": match["objects"] if match else None,
                     "identical_to_build_file": same[0] if same else None,
                     "mobileModes": info["settings"].get("mobileModes")})
    return {"catalogue": catalogue, "build_files": local, "pool": pool,
            "pool_source": ("2025-07-22 Internet Archive captures of /.proxy/maps/<name>.json: the maps "
                            "Strike clients actually loaded; the weights are server-side"),
            "rotation": {"remote_map_pool": bool(pool_rx),
                         "rule": ("server init: gameConfig.remoteFeatures.mapPool[isProd ? 'prod' : 'staging'] is a "
                                  "list of {mapName, mapWeight}; each entry's catalogue index (by name) is "
                                  "pushed mapWeight times into config.maps, then a uniform random pick; "
                                  "without it config.maps applies") if pool_rx else None,
                         "mobile_modes": ("with useMobileModes the mode is re-picked from the map's "
                                          "mobileModes list when the chosen one is not in it") if mobile else None,
                         "game_config_refresh_ms": _game_config_refresh(s)},
            "fetch_path": (fm.group(1) + "<fileName>" + fm.group(2)) if fm else None}


def extract_packets(s):
    c2s, _ = literal_at(s, r'(?<![\w$])[\w$]{1,40}=\{RPC:"_0",VALIDATE_FRVR_AUTH_TOKEN:"_1"', "packets.client_to_server")
    s2c, _ = literal_at(s, r'(?<![\w$])[\w$]{1,40}=\{RPC_SUCCESS:"_0",RPC_ERROR:"_1"', "packets.server_to_client")
    lit = sorted(set(re.findall(r'==?"(io-init|cpt|cptR)"', s)))
    wire = {}
    m = re.search(r'decodeNetworkMessage:([\w$]+)', s)
    if not m:
        fail("packets.wire", "decodeNetworkMessage not exported")
    dm = re.search(re.escape(m.group(1)) + r"=[\w$]+=>\{[\w$]+=new Uint8Array\([\w$]+\);let [\w$]+=([\w$]+)\("
                   r"[\w$]+\[[\w$]+\.length-2\],[\w$]+\[[\w$]+\.length-1\]\),[\w$]+=[\w$]+\.slice\(0,[\w$]+\.length-([\w$]+)\)", s)
    if dm:
        r = resolve_ident(s, dm.group(2), len(s))
        wire["trailer_bytes"] = r[0] if r else None
    am = re.search(r'(?<![\w$])([\w$]{1,40})!="cptR"&&\(this\.ahNum=[\w$]+\.rotateNumber\(this\.ahNum,([\w$]+)\)\)', s)
    if am:
        r = resolve_ident(s, am.group(2), am.start())
        wire["ahnum_step"] = r[0] if r else None
        wire["ahnum_skip"] = "cptR"
    rm = re.search(r'rotateNumber:([\w$]+)', s)
    if rm:
        rr = re.search(re.escape(rm.group(1)) + r"=\(([\w$]+),([\w$]+)\)=>\1\+\2&255", s)
        wire["rotate"] = "(ahNum + step) & 255" if rr else None
    wire["ahnum_start"] = 0 if re.search(r"this\.ahNum=0", s) else None
    wire["frame"] = "msgpack([name, ...args]) + 2 trailer bytes (ahNum >> 4 & 15, ahNum & 15)"
    wire["decode_returns"] = "[ahNum, name, args]"
    ws = re.search(r'binaryType="arraybuffer"', s)
    wire["websocket_binary_type"] = "arraybuffer" if ws else None
    return {"client_to_server": c2s, "server_to_client": s2c, "literal_names": lit, "wire": wire}


_FN_BEFORE = re.compile(r'(?<![\w$])([\w$]{1,60}):(?:async\s*)?(?:\([^()]*\)|\(\{[^()]*\}(?:=\{\})?\)|[\w$]+)=>')


def _template_path(t):
    """`/v1/items/${e}` -> /v1/items/{}; a non-identifier substitution -> {expr}."""
    out = []
    i = 1
    while i < len(t) - 1:
        if t[i] == "$" and t[i + 1:i + 2] == "{":
            e = skip_balanced(t, i + 1)
            inner = t[i + 2:e].strip()
            out.append("{}" if re.fullmatch(IDENT, inner) else "{expr}")
            i = e + 1
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


def extract_routes(s):
    """Every gateway call: a "/vN/..." or `/vN/...` literal passed as the first or second argument
    of a call, with its options object ({method, body}) and error text when present."""
    routes = []
    for m in re.finditer(r'\((?:[\w$]{1,40},)?("/v\d/[^"\s]*"|`/v\d/[^`]*`)', s):
        lit = m.group(1)
        if lit[0] == '"':
            path, raw = js_unquote(lit), None
        else:
            path, raw = _template_path(lit), lit[1:-1]
        i = m.end()
        method, body, err = "GET", [], None
        if s[i:i + 1] == ",":
            j = _ws(s, i + 1)
            if s[j:j + 1] == "{":
                e = skip_balanced(s, j)
                opts = s[j:e + 1]
                mm = re.search(r'method:"([A-Z]+)"', opts)
                if mm:
                    method = mm.group(1)
                bm = re.search(r"body:(?:JSON\.stringify\()?\{", opts)
                if bm:
                    bs = bm.end() - 1
                    be = skip_balanced(opts, bs)
                    try:
                        bobj, _ = parse_object(opts, bs)
                        body = [k for k in bobj if not k.startswith("$")]
                    except ExtractError:
                        body = []
                    if be < 0:
                        body = []
                k = _ws(s, e + 1)
                if s[k:k + 1] == ",":
                    k2 = _ws(s, k + 1)
                    if s[k2:k2 + 1] == '"':
                        err = js_unquote(s[k2:skip_string(s, k2)])
        win = s[max(0, m.start() - 160):m.start() + 1]
        fns = list(_FN_BEFORE.finditer(win))
        fn = fns[-1].group(1) if fns and win.find("=>", fns[-1].end()) < 0 else None
        if fn is None:
            cm = re.search(r'(?:const|let|var|,)\s*[\w$]{1,40}=async(?:\s*\([^()]*\)|\s+[\w$]+)=>\{(?:const|let) [\w$]+=await [\w$]+\($', win)
            fn = "(module function)" if cm else None
        r = {"method": method, "path": path, "body": body, "error": err, "fn": fn}
        if raw is not None:
            r["template"] = raw
        routes.append(r)
    seen = set()
    uniq = []
    for r in routes:
        k = (r["method"], r["path"], r["fn"] or "")
        if k not in seen:
            seen.add(k)
            uniq.append(r)
    uniq.sort(key=lambda r: (r["path"], r["method"], r["fn"] or ""))
    return uniq


def extract_gateway(s, entry_cfg):
    uniq = extract_routes(s)
    lit_paths = sorted(set(js_unquote(x) for x in re.findall(r'"/v\d/[^"\s]*"', s)))
    covered = {r["path"] for r in uniq}
    if not uniq:
        fail("gateway.routes", "no `fn:(..)=>call(i,\"/vN/..\",{method:..})` route found")
    dm = re.search(r'(?<![\w$])([\w$]{1,40})=\{usc1a:"https://director\.', s)
    if not dm:
        fail("gateway.directors", "`X={usc1a:\"https://director.` not found")
    directors, _ = parse_object(s, dm.end(1) + 1)
    rn = re.search(r'(?<![\w$])([\w$]{1,40})=\{usc1a:"North America"', s)
    region_names = parse_object(s, rn.end(1) + 1)[0] if rn else {}
    gw = sorted(set(re.findall(r'https://gateway\.[\w.-]*krunker\.frvr\.com', s)))
    dirpaths = sorted(set(js_unquote(x) for x in re.findall(r'"(?:/v\d)?/director/[^"\s]*"|"/live"', s)))
    alloc = sorted(set(re.findall(r'[`"][^`"]{0,40}/v1/director/allocate[^`"]{0,40}[`"]', s)))
    hdrs = sorted(set(re.findall(r'"(x-[a-z-]+)"', s)))
    director = {}
    pm = re.search(r'body:JSON\.stringify\(\{capacity:([\w$]{1,40}),region:"([^"]+)",customId:[\w$]{1,40}\}\)', s)
    if pm:
        cap = None
        cm = re.search(r'\.allocateCustomPrivateServer\([\w$]{1,40},([\w$]{1,40})\)', s)
        if cm:
            r = resolve_ident(s, cm.group(1), len(s))
            cap = r[0] if r else None
        director["private_allocate"] = {"path": "/v1/director/allocate/private", "method": "POST",
                                        "body": {"capacity": cap, "region": pm.group(2), "customId": "<custom id>"}}
    fm = re.search(r'\?\{fleet:([\w$]{1,40})\}:', s)
    fleet = None
    if fm:
        r = resolve_ident(s, fm.group(1), len(s))
        fleet = r[0] if r else None
    director["public_allocate"] = {"path": "/v1/director/allocate", "method": "POST",
                                   "body_keys": [k for k, ok in (("fleet", bool(fm)),
                                                 ("openSlots", "{openSlots:" in s)) if ok],
                                   "beginner_fleet": fleet,
                                   "beginner_rule": "feature beginner_server: level <= forcedMaxLevel, or <= maxLevel "
                                                    "with chanceOfAllocation -> {fleet}" if fm else None}
    lm = re.search(r'const [\w$]{1,40}=(\d+),[\w$]{1,40}=\[\];for\(const [\w$]{1,40} in [\w$]{1,40}\)\{if\([\w$]{1,40}==="staging"', s)
    director["latency_probes_per_region"] = int(lm.group(1)) if lm else None
    director["response_fields"] = sorted(set(re.findall(r'\(await [\w$.]{1,60}\.allocateServer\([\w$]{1,40}\)\)\.(\w+)', s)))
    director["auth"] = "Bearer FRVR access token when logged in" if re.search(r'Authorization=`Bearer \$\{', s) else None
    return {"routes": uniq, "unmatched_literal_paths": [p for p in lit_paths if p not in covered],
            "director": director,
            "directors": directors, "region_names": region_names, "gateway_hosts": gw,
            "director_paths": dirpaths, "allocate_literals": alloc, "request_headers": hdrs,
            "url_mappings": (entry_cfg or {}).get("channels", {}).get("discord", {}).get("URLMappings", [])}


def extract_items(s):
    out = {}
    qm = re.search(r"(?<![\w$])([\w$]{1,40})=\{Weapon:0,Hat:1,", s)
    if not qm:
        fail("items.types", "`X={Weapon:0,Hat:1,` not found")
    out["item_types"] = parse_object(s, qm.end(1) + 1)[0]
    um = re.search(r'(?<![\w$])([\w$]{1,40})=\{SPIN_TICKETS:"spin_tickets"', s)
    if not um:
        fail("items.unlock_origins", "`X={SPIN_TICKETS:` not found")
    out["unlock_origins"] = parse_object(s, um.end(1) + 1)[0]
    # TypeScript string enums: var X=(i=>(i.A="a",...,i))(X||{})
    enums = {}
    for m in re.finditer(r'var ([\w$]+)=\(([\w$]+)=>\(((?:\2\.[\w$]+="[^"]*",)+)\2\)\)\(\1\|\|\{\}\)', s):
        vals = dict(re.findall(r'[\w$]+\.([\w$]+)="([^"]*)"', m.group(3)))
        enums[",".join(sorted(vals))] = vals
    for m in re.finditer(r'var ([\w$]+)=\(([\w$]+)=>\(((?:\2\[\2\.[\w$]+=-?\d+\]="[^"]*",)+)\2\)\)\(\1\|\|\{\}\)', s):
        vals = {k: int(v) for k, v in re.findall(r'\[[\w$]+\.([\w$]+)=(-?\d+)\]', m.group(3))}
        enums[",".join(sorted(vals))] = vals
    named = {}
    for vals in enums.values():
        if "Currency" in vals and "Booster" in vals:
            named["reward_types"] = vals
        elif "LoyaltyBundle" in vals:
            named["compensation_types"] = vals
        elif "game_load_start" in vals:
            named["analytics_events"] = vals
        elif "NorthAmerica" in vals:
            named["regions"] = vals
        elif "Percentage" in vals:
            named["discount_types"] = vals
    out["enums"] = named
    rv, _ = literal_at(s, r'=\[(?:\{name:"Common"[^\]]*?\},)?\{name:"[A-Za-z]+",language:"windows\.spin\.rarity\.', "items.rarities")
    out["rarities"] = strip_fns(rv)
    sm, _ = literal_at(s, r'=\[\{name:"Uncommon",color:"#', "items.rarity_colors")
    out["rarity_colors"] = sm
    tiers, _ = literal_at(s, r'=\[\{name:"Starter",icons:\d+,rarities:', "items.crate_tiers")
    out["crate_tiers"] = tiers
    pk, pm = literal_at(s, r'(?:var|let|const)\s+[\w$]+=\[\{name:"KR Package",', "items.packages")
    packages = []
    k = pm.end() - len('[{name:"KR Package",')
    while s[k] != "[":
        k += 1
    end = skip_balanced(s, k)
    body = s[k:end + 1]
    # the KR amount of each package is `var r=N` in its claim function
    pos = 1
    for p in resolve_refs(s, pk, pm.start()):
        ent = strip_fns(p)
        ent["hooks"] = fn_keys(p)
        packages.append(ent)
    amounts = [int(x) for x in re.findall(r'claim:function\([^)]*\)\{if\(!\([\w$]+&&[\w$]+\.account\)\)return!1;var [\w$]+=(\d+);', body)]
    kr = [p for p in packages if p.get("name") == "KR Package"]
    for p, a in zip(kr, amounts):
        p["kr"] = a
    out["packages"] = packages
    out["item_strings"] = sorted(set(js_unquote(x) for x in re.findall(r'"[A-Z][A-Za-z ]{1,24} Package"', s)) |
                                 set(js_unquote(x) for x in re.findall(r'"Spin Tickets?"', s)))
    return out


def extract_i18n(s):
    m = re.search(r"=`\n(menu\.btn\.[\w.]+ [^\n]*\n)", s)
    if not m:
        fail("i18n", "the `key text` table starting with menu.btn.* not found")
    start = m.start() + 1
    end = skip_template(s, start)
    body = s[start + 1:end - 1]
    if "${" in body:
        fail("i18n", "table contains substitutions")
    out = {}
    for line in body.split("\n"):
        if not line.strip():
            continue
        k, _, v = line.partition(" ")
        out[k] = v
    if len(out) < 500:
        fail("i18n", "only %d strings" % len(out))
    return out


_SELECTOR = re.compile(r"(?:(?:div|span|img|a|button|input|p|h[1-6]|ul|li|canvas|svg|video|label|select|"
                       r"option|textarea|table|tr|td|section|i|b|strong|small|form|iframe)?)"
                       r"[.#][A-Za-z][\w-]*(?:[.#][A-Za-z][\w-]*)*")
_NOT_SELECTOR = re.compile(r"#[0-9A-Fa-f]{3,8}|\.(?:png|svg|jpe?g|gif|webp|json|mp4|ogg|mp3)")


def extract_screens(s):
    """Screen / window components: `reg(Class,"id","NAME")` registrations, their options and the
    CSS selectors the component's class body renders (hyperscript selector strings, in order)."""
    out = []
    for m in re.finditer(r'[,;(){}\s]([\w$]{1,40})\(([\w$]{1,40}),"id","([A-Z_0-9]+)"\)', s):
        reg = m.group(1)
        ident, sid = m.group(2), m.group(3)
        cm = None
        for c in re.finditer(r"class " + re.escape(ident) + r"(?: extends [\w$.]+)?\{", s[:m.start()]):
            cm = c
        selectors = []
        root = None
        if cm:
            seg = s[cm.end():m.start()]
            v = seg.find("view(")
            for sm in re.finditer(r'"([^"\\\n]{2,100})"', seg):
                if (_SELECTOR.fullmatch(sm.group(1)) and not _NOT_SELECTOR.fullmatch(sm.group(1))
                        and sm.group(1) not in selectors):
                    selectors.append(sm.group(1))
                    if root is None and v >= 0 and sm.start() > v:
                        root = sm.group(1)
            if root is None and selectors:
                root = selectors[0]
        om = re.search(re.escape(reg) + r'\(' + re.escape(ident) + r',"opts",(\{[^{}]*\})\)', s)
        opts = parse_object(s, om.start(1))[0] if om else {}
        out.append({"id": sid, "root": root, "opts": opts, "selectors": selectors[:16]})
    out.sort(key=lambda x: x["id"])
    if not out:
        fail("ui.screens", 'no reg(X,"id","NAME") registration found')
    return out


_CSS_COMMENT = re.compile(r"/\*.*?\*/", re.S)


def css_blocks(css, at):
    """Every `@at name{...}` block (balanced braces) -> list of (prelude, body)."""
    out = []
    for m in re.finditer(r"@" + at + r"\s*([^{]*)\{", css):
        i = m.end() - 1
        depth = 0
        j = i
        while j < len(css):
            if css[j] == "{":
                depth += 1
            elif css[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((m.group(1).strip(), css[i + 1:j]))
    return out


def summarize_css(css, raw=False):
    css = _CSS_COMMENT.sub("", css)
    fonts = []
    for _, body in css_blocks(css, "font-face"):
        fam = re.search(r"font-family:\s*([^;]+)", body)
        w = re.search(r"font-weight:\s*([^;]+)", body)
        src = re.findall(r"url\(([^)]+)\)", body)
        fonts.append({"family": fam.group(1).strip().strip("'\"") if fam else None,
                      "weight": w.group(1).strip() if w else "400", "src": src})
    keyframes = {}
    for name, body in css_blocks(css, "keyframes"):
        keyframes[name] = body if raw else len(body)
    tokens = {}
    for m in re.finditer(r"(?<![\w-])(--[\w-]+)\s*:\s*([^;}]+)", css):
        tokens.setdefault(m.group(1), [])
        v = m.group(2).strip()
        if v not in tokens[m.group(1)]:
            tokens[m.group(1)].append(v)
    colors = {}
    for m in re.finditer(r"#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b|rgba?\([^)]*\)", css):
        c = m.group(0).lower().replace(" ", "")
        colors[c] = colors.get(c, 0) + 1
    top_colors = sorted(colors.items(), key=lambda x: (-x[1], x[0]))[:60]
    transitions = {}
    for m in re.finditer(r"transition:\s*([^;}]+)", css):
        t = m.group(1).strip()
        transitions[t] = transitions.get(t, 0) + 1
    animations = {}
    for m in re.finditer(r"animation(?:-name)?:\s*([^;}]+)", css):
        t = m.group(1).strip()
        animations[t] = animations.get(t, 0) + 1
    blocks = {}
    for m in re.finditer(r"\.([a-zA-Z][\w-]*?)(?:__|--)[\w-]+", css):
        blocks[m.group(1)] = blocks.get(m.group(1), 0) + 1
    font_families = {}
    for m in re.finditer(r"font-family:\s*([^;}]+)", css):
        f = m.group(1).strip()
        font_families[f] = font_families.get(f, 0) + 1
    media = sorted(set(p for p, _ in css_blocks(css, "media")))
    urls = sorted(set(u.strip("'\"") for u in re.findall(r"url\(([^)]+)\)", css) if not u.strip("'\"").startswith("data:")))
    easing = {}
    for m in re.finditer(r"cubic-bezier\([^)]*\)|\b(?:ease-in-out|ease-in|ease-out|linear|steps\([^)]*\))", css):
        easing[m.group(0)] = easing.get(m.group(0), 0) + 1
    return {"size": len(css), "fonts": fonts, "font_families": dict(sorted(font_families.items(), key=lambda x: -x[1])[:25]),
            "keyframes": keyframes, "tokens": tokens, "top_colors": top_colors,
            "transitions": dict(sorted(transitions.items(), key=lambda x: (-x[1], x[0]))[:80]),
            "animations": dict(sorted(animations.items(), key=lambda x: (-x[1], x[0]))[:120]),
            "easing": dict(sorted(easing.items(), key=lambda x: (-x[1], x[0]))),
            "bem_blocks": dict(sorted(blocks.items())), "media_queries": media, "urls": urls}


def extract_entry(html):
    """The entry page: FRVR.config (JSON, raw_decode), scripts, stylesheets, the loading screen."""
    out = {"scripts": re.findall(r'<script[^>]*\ssrc="([^"]+)"', html),
           "stylesheets": re.findall(r'<link[^>]*rel="stylesheet"[^>]*href="([^"]+)"', html) +
                          re.findall(r'<link[^>]*href="([^"]+)"[^>]*rel="stylesheet"', html),
           "title": (re.search(r"<title>([^<]*)</title>", html) or [None, None])[1]}
    cfg = None
    m = re.search(r"FRVR\.config\s*=\s*", html)
    if m:
        try:
            cfg, _ = json.JSONDecoder().raw_decode(html, m.end())
        except ValueError:  # a prettified page: JS object syntax
            try:
                cfg, _ = parse_value(html, m.end())
            except (ExtractError, IndexError):
                cfg = None
            if not isinstance(cfg, dict):
                cfg = None
    out["frvr_config"] = cfg
    body = re.search(r"<body[^>]*>(.*)</body>", html, re.S)
    if body:
        b = re.sub(r"<script.*?</script>", "", body.group(1), flags=re.S)
        out["body_markup"] = b.strip()
    out["inline_styles"] = re.findall(r"<style[^>]*>(.*?)</style>", html, re.S)
    return out


def _snip(s, a, b, before=90, after=110):
    return s[max(0, a - before):min(len(s), b + after)]


def extract_mobile(s, const_obj, const_pos):
    """How the client decides mobile vs desktop and what that switch changes.

    Detection: `window._isMobile = detect()` once at load, where detect() is true for an iPad that
    reports MacIntel with maxTouchPoints > 2, else a user-agent regex test. Two accessors return it;
    every behaviour switch calls one of them. The menu itself is not switched: `frvrMenu` (the FRVR
    mobile menu, root #frvrMenuMobile) is a build constant, so desktop shows the same mobile UI."""
    m = re.search(r"window\._isMobile=([\w$]{1,40})\(\);", s)
    if not m:
        fail("ui.mobile", "`window._isMobile=<detect>();` not found")
    det = m.group(1)
    dm = re.search(r"function " + re.escape(det) + r"\(\)\{", s)
    if not dm:
        fail("ui.mobile", "detect function %s not found" % det)
    body = s[dm.end() - 1:skip_balanced(s, dm.end() - 1) + 1]
    regexes = []
    for ident in re.findall(r"([\w$]{1,40})\.test\(", body):
        r = re.search(r"(?<![\w$])" + re.escape(ident) + r"=/", s)
        if r:
            e = skip_regex(s, r.end() - 1)
            regexes.append(s[r.end() - 1:e])
    ipad = None
    for f in re.findall(r"([\w$]{1,40})\(\)\?!0:", body):
        fm = re.search(r"function " + re.escape(f) + r"\(\)\{return ([^}]*)\}", s)
        if fm:
            ipad = fm.group(1)
    acc = []
    am = re.search(r"function ([\w$]{1,40})\(\)\{return window\._isMobile\}", s)
    if am:
        acc.append(am.group(1))
        bm = re.search(r"function ([\w$]{1,40})\(\)\{return " + re.escape(am.group(1)) + r"\(\)\}", s)
        if bm:
            acc.append(bm.group(1))
    uses = []
    if acc:
        rx = re.compile(r"(?<![\w$.])(?:" + "|".join(re.escape(a) for a in acc) + r")\(\)")
        for u in rx.finditer(s):
            ctx = s[max(0, u.start() - 40):u.end()]
            pre = ctx[:-len(u.group(0))]
            if pre.endswith("function ") or re.search(r"function [\w$]{1,40}\(\)\{return $", pre):
                continue
            uses.append(_snip(s, u.start(), u.end()))
    keys = {}
    for k in ("useMobileControls", "useMobileModes", "mobileModes", "isTouchServer", "mobileOnly",
              "mobileFriendly", "autoFireMbl", "aimAssist", "isMobileWrapper", "frvrMenuMobile",
              "frvrMenuMobileHolder", "frvrTapToPlayMobile", "mobileControls", "ontouchstart"):
        keys[k] = len(re.findall(r"(?<![\w$])" + re.escape(k) + r"(?![\w$])", s))
    fm = const_obj.get("frvrMenu")
    frvr_menu = None
    if isinstance(fm, dict) and "$expr" in fm:
        r = resolve_ident(s, fm["$expr"], const_pos)
        frvr_menu = r[0] if r else None
    sb = re.search(r"var [\w$]{1,40}=1,[\w$]{1,40},([\w$]{1,40})=(\d+),([\w$]{1,40})=(\d+),[\w$]{1,40},[\w$]{1,40};"
                   r"window\.scaleBase=function", s)
    sbody = None
    if sb:
        st = s.find("{", sb.end())
        sbody = s[sb.end() - len("window.scaleBase=function"):skip_balanced(s, st) + 1]
    ctrl = re.search(r"get controllerType\(\)\{return ([^}]*)\}", s)
    return {"detect": {"global": "window._isMobile", "computed": "once at load",
                       "user_agent_regexes": regexes, "ipad_rule": ipad,
                       "function_source": body[:600]},
            "accessor_count": len(acc), "switch_sites": uses,
            "controller_type": ctrl.group(1) if ctrl else None,
            "frvr_menu_constant": frvr_menu,
            "ui_scale": {"reference_width": int(sb.group(2)) if sb else None,
                         "reference_height": int(sb.group(4)) if sb else None,
                         "scaleBase_source": sbody},
            "keyword_counts": keys}


def extract_ui(s, css_text, entry_html, archive):
    """UI of the primary build; `archive` = {tag: (css_text|None, entry_html|None)} older builds."""
    ui = {"screens": extract_screens(s)}
    ui["images"] = sorted(set(re.findall(r'"\.?/?(img/[\w./-]+\.(?:png|svg|jpe?g|gif|webp))"', s)))
    te = {}
    for m in re.finditer(r"TWEEN\.Easing\.(\w+)\.(\w+)", s):
        k = m.group(1) + "." + m.group(2)
        te[k] = te.get(k, 0) + 1
    ui["tween_easings"] = dict(sorted(te.items()))
    flags = set()
    for a, b in re.findall(r'isFeatureEnabled\("([\w-]+)"\)|getFeature\("([\w-]+)"\)', s):
        flags.add(a or b)
    ui["feature_flags_in_bundle"] = sorted(flags)
    ui["ftue_ids"] = sorted(set(re.findall(r'"((?:[a-z]+_)+ftue)"', s)))
    ui["css"] = {"build": summarize_css(css_text)}
    e = extract_entry(entry_html)
    ui["entries"] = {"build": _entry_summary(e)}
    for tag, (ctext, etext) in sorted(archive.items()):
        if ctext is not None:
            ui["css"][tag] = summarize_css(ctext)
        if etext is not None:
            ui["entries"][tag] = _entry_summary(extract_entry(etext))
    return ui, e


def _entry_summary(e):
    cfg = e.get("frvr_config") or {}
    feats = (cfg.get("features") or {}).get("defaultFeatures")
    return {"title": e["title"], "scripts": e["scripts"], "stylesheets": e["stylesheets"],
            "body_markup": e.get("body_markup"), "inline_styles": e.get("inline_styles"),
            "default_features": feats, "ab_tests": cfg.get("abTests"),
            "frvr_config_keys": sorted(cfg) if cfg else None}


def extract_cdn(s, weapons, classes, item_types):
    """Asset paths the bundle builds with assetsUrl() (cdn.frvr.com/games/krunkerfrvr/<path>)."""
    paths = set(re.findall(r'assetsUrl\("(/(?:textures|img)/[\w./-]+\.(?:png|svg|jpg))"\)', s))
    for c in classes:
        paths.add("/textures/classes/icon_%d.png" % c["id"])
    previews = bool(re.search(r'"/textures/previews/weapons/"\+[^;]{0,80}?\.src\+"\.png"', s))
    for w in weapons:
        icon = w["data"].get("icon")
        if isinstance(icon, str) and icon:
            paths.add("/textures/weapons/%s.png" % icon)
        src = w["data"].get("src")
        if previews and isinstance(src, str) and re.fullmatch(r"weapon_\d+", src):
            paths.add("/textures/previews/weapons/%s.png" % src)
    for v in item_types.values():
        paths.add("/textures/previews/cosmetics/%d_default.png" % v)
    paths.add("/textures/melee/icon_0.png")
    pm = re.search(r'assetsUrl\("/textures/previews/weapons/"\+([^;]{0,80}?\.png")', s)
    base = re.search(r'"(https://cdn\.frvr\.com/games/[\w-]+/)"', s)
    return {"base": base.group(1) if base else None, "paths": sorted(paths),
            "weapon_preview_expr": pm.group(1) if pm else None}


def extract_bundle(s):
    """Every bundle-only section of one Strike build."""
    meta = extract_meta(s)
    constants, const_obj, const_pos = extract_constants(s)
    tables = extract_settings_tables(s, const_obj, const_pos)
    return {"meta": meta, "constants": constants, "tables": tables, "const_obj": const_obj, "const_pos": const_pos,
            "classes": extract_classes(s, tables["classConfig"]), "weapons": extract_weapons(s),
            "modes": extract_modes(s), "packets": extract_packets(s), "items": extract_items(s),
            "i18n": extract_i18n(s), "screens": extract_screens(s),
            "routes": extract_routes(s)}


def builds_diff(old, new):
    """What changed from an older build (the archived 1.27.0) to the primary one."""
    def by(lst, key):
        return {x[key]: x for x in lst}
    oc, nc = by(old["classes"], "id"), by(new["classes"], "id")
    classes = {}
    for k in sorted(set(oc) | set(nc)):
        if k not in oc or k not in nc:
            classes[str(k)] = "added" if k in nc else "removed"
            continue
        d = diff_objects(nc[k]["data"], oc[k]["data"])
        if d["changed"] or d["strike_only"] or d["v10_only"]:
            classes[str(k)] = {"changed": {f: {"new": v["strike"], "old": v["v10"]} for f, v in d["changed"].items()},
                               "added": sorted(d["strike_only"]), "removed": sorted(d["v10_only"])}
    ow = {w["data"].get("name"): w for w in old["weapons"]}
    weapons = {}
    for w in new["weapons"]:
        name = w["data"].get("name")
        if name not in ow:
            weapons[name] = "added"
            continue
        d = diff_objects(w["data"], ow[name]["data"])
        if d["changed"] or d["strike_only"] or d["v10_only"]:
            weapons[name] = {"changed": {f: {"new": v["strike"], "old": v["v10"]} for f, v in d["changed"].items()},
                             "added": sorted(d["strike_only"]), "removed": sorted(d["v10_only"])}
    for name in ow:
        if name not in {w["data"].get("name") for w in new["weapons"]}:
            weapons[name] = "removed"
    om, nm = by(old["modes"]["list"], "id"), by(new["modes"]["list"], "id")
    modes = {}
    for k in sorted(set(om) | set(nm)):
        if k not in om or k not in nm:
            modes[k] = "added" if k in nm else "removed"
            continue
        d = diff_objects(nm[k]["data"], om[k]["data"])
        if d["changed"] or d["strike_only"] or d["v10_only"]:
            modes[k] = {"changed": {f: {"new": v["strike"], "old": v["v10"]} for f, v in d["changed"].items()},
                        "added": sorted(d["strike_only"]), "removed": sorted(d["v10_only"])}

    def setdiff(a, b):
        return {"added": sorted(set(b) - set(a)), "removed": sorted(set(a) - set(b))}
    pk = {}
    for side in ("client_to_server", "server_to_client"):
        a = {"%s=%s" % (k, v) for k, v in old["packets"][side].items()}
        b = {"%s=%s" % (k, v) for k, v in new["packets"][side].items()}
        pk[side] = setdiff(a, b)
    routes = setdiff({"%s %s" % (r["method"], r["path"]) for r in old["routes"]},
                     {"%s %s" % (r["method"], r["path"]) for r in new["routes"]})
    screens = setdiff({x["id"] for x in old["screens"]}, {x["id"] for x in new["screens"]})
    i18n = setdiff(old["i18n"], new["i18n"])
    i18n["changed"] = sorted(k for k in set(old["i18n"]) & set(new["i18n"]) if old["i18n"][k] != new["i18n"][k])
    consts = {k: {"old": old["constants"].get(k), "new": v} for k, v in new["constants"].items()
              if not _deep_eq(old["constants"].get(k), v)}
    return {"from": old["meta"]["game_version"], "to": new["meta"]["game_version"],
            "classes": classes, "weapons": weapons, "modes": modes, "packets": pk, "routes": routes,
            "screens": screens, "i18n": i18n, "constants": consts}


# ----------------------------------------------------------------------------- main

def _one(dirpath, rx, what):
    if not os.path.isdir(dirpath):
        fail("source", "%s missing" % dirpath)
    hits = [f for f in sorted(os.listdir(dirpath)) if re.fullmatch(rx, f)]
    if len(hits) != 1:
        fail("source", "expected exactly one %s in %s, found %r" % (what, dirpath, hits))
    return hits[0]


def extract(build, live):
    files = {}

    def load(path, key):
        if not os.path.exists(path):
            fail("source", "%s missing" % path)
        text, data = read_text(path)
        files[key] = {"sha256": sha256(data), "size": len(data)}
        return text

    adir = os.path.join(build, "assets")
    js = _one(adir, r"index-[\w-]+\.js", "minified bundle (index-*.js, not *.deob.js)")
    css = _one(adir, r"index-[\w-]+\.css", "stylesheet")
    s = load(os.path.join(adir, js), "build:assets/" + js)
    css_text = load(os.path.join(adir, css), "build:assets/" + css)
    entry_html = load(os.path.join(build, "index.html"), "build:index.html")
    archive = {}
    for tag, b, c, e in (("1.27.0", ARCHIVE_BUNDLE, ARCHIVE_CSS, ARCHIVE_ENTRY),
                         ("1.7.0", None, OLD_CSS, OLD_ENTRY)):
        ct = load(os.path.join(live, c), "archive:" + c) if os.path.exists(os.path.join(live, c)) else None
        et = load(os.path.join(live, e), "archive:" + e) if os.path.exists(os.path.join(live, e)) else None
        archive[tag] = (ct, et)
    old_src = load(os.path.join(live, ARCHIVE_BUNDLE), "archive:" + ARCHIVE_BUNDLE)
    with open(V10_WEAPONS) as f:
        v10w = norm_v10(json.load(f))
    with open(V10_CLASSES) as f:
        v10c = norm_v10(json.load(f))

    new = extract_bundle(s)
    old = extract_bundle(old_src)
    ui, entry = extract_ui(s, css_text, entry_html, archive)
    ui["mobile"] = extract_mobile(s, new["const_obj"], new["const_pos"])
    ui["strings"] = {"i18n_section": "i18n (key -> English text, the client's own `key text` table)",
                     "count": len(new["i18n"])}
    gateway = extract_gateway(s, entry.get("frvr_config") or extract_entry(archive["1.27.0"][1] or "").get("frvr_config"))
    maps = extract_maps(s, build, live, files)
    cdn = extract_cdn(s, new["weapons"], new["classes"], new["items"]["item_types"])
    constants, tables, modes = new["constants"], new["tables"], new["modes"]

    server_cfg = {r["varN"]: r for r in tables["serverConfig"] if isinstance(r, dict) and "varN" in r}
    timing = {k: {f: server_cfg[k].get(f) for f in ("name", "def", "defC", "min", "max", "step") if f in server_cfg[k]}
              for k in ("gameTime", "warmupTime", "gamRounds", "intermTmr", "maxPlayers", "minPlayers")
              if k in server_cfg}
    timing["modes_gameTime_ms"] = {m["id"]: m["data"]["gameTime"] for m in modes["list"] if "gameTime" in m["data"]}
    timing["gameStartDelay_ms"] = constants.get("gameStartDelay")
    timing["note"] = ("match length = config.gameTime minutes (custom-game default `def`, gameTime*6e4 ms); "
                      "public lobbies take it from the server's config / /v1/game-config, not from this bundle")
    player_cap = {"client_default_maxPlayers": constants.get("maxPlayers"),
                  "client_updates_cap_from_server": bool(re.search(r"updateMaxPlayers:", s)),
                  "custom_game_setting": timing.get("maxPlayers"),
                  "private_match_capacity": (gateway["director"].get("private_allocate") or {}).get("body", {}).get("capacity"),
                  "discord_activity_max_participants": 12,
                  "discord_source": ("GET discord.com/api/v10/applications/1011683823555199066/rpc "
                                     "(STRIKE-REWRITE-PLAN 1.2), not the bundle")}
    out = {
        "generator": {"tool": "tools/strike/extract_strike.py", "rev": GENERATOR_REV},
        "meta": dict(new["meta"], sources=dict(sorted(files.items())),
                     primary="build " + new["meta"]["game_version"] + " (KrunkerStrike_v1.37.2/public, snapshot 2026-06-13)",
                     secondary="Internet Archive capture of build " + old["meta"]["game_version"] + " (2024-02-08)"),
        "summary": {
            "modes": len(modes["list"]), "classes": [c["name"] for c in new["classes"]],
            "class_ids": [c["id"] for c in new["classes"]], "weapons": len(new["weapons"]),
            "map_catalogue": len(maps["catalogue"]), "map_pool": [m["archive_file"][:-5] for m in maps["pool"]],
            "map_files_in_build": len(maps["build_files"]),
            "packets_c2s": len(new["packets"]["client_to_server"]),
            "packets_s2c": len(new["packets"]["server_to_client"]),
            "gateway_routes": len(gateway["routes"]), "i18n_strings": len(new["i18n"]),
            "screens": len(ui["screens"]),
        },
        "modes": modes,
        "classes": new["classes"],
        "classes_vs_v10": classes_vs_v10(new["classes"], v10c),
        "weapons": new["weapons"],
        "weapons_vs_v10": weapons_vs_v10(new["weapons"], v10w),
        "maps": maps,
        "packets": new["packets"],
        "gateway": gateway,
        "items": new["items"],
        "config": {"constants": constants, "player_cap": player_cap, "timing": timing,
                   "serverConfig": tables["serverConfig"], "classConfig": tables["classConfig"],
                   "weaponConfig": tables["weaponConfig"], "serverPresets": tables["serverPresets"]},
        "i18n": new["i18n"],
        "ui": ui,
        "cdn": cdn,
        "builds_diff": builds_diff(old, new),
    }
    return out, files


def cache_extras(build, live):
    extra = {}
    op = os.path.join(live, OLD_BUNDLE)
    if os.path.exists(op):
        src, data = read_text(op)
        extra["bundle_1_7_0"] = {"sha256": sha256(data), "size": len(data)}
        try:
            extra["bundle_1_7_0"]["meta"] = extract_meta(src)
            cv, _ = literal_at(src, r'(?:let|const|var)\s+[\w$]+=\[\{id:0,name:"Triggerman"', "classes")
            extra["bundle_1_7_0"]["classes"] = [{k: c.get(k) for k in ("id", "name", "health", "speed", "loadout")}
                                                for c in cv]
        except ExtractError as e:
            extra["bundle_1_7_0"]["error"] = str(e)
    raw = {}
    adir = os.path.join(build, "assets")
    css = _one(adir, r"index-[\w-]+\.css", "stylesheet")
    raw["build"] = summarize_css(read_text(os.path.join(adir, css))[0], raw=True)["keyframes"]
    p = os.path.join(live, ARCHIVE_CSS)
    if os.path.exists(p):
        raw["1.27.0"] = summarize_css(read_text(p)[0], raw=True)["keyframes"]
    extra["css_keyframes_raw"] = raw
    return extra


def dump(obj):
    return json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def write_atomic(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", default=DEFAULT_BUILD, help="the local Strike build's public/ directory")
    ap.add_argument("--live", default=DEFAULT_LIVE, help="the S0 archive mirror")
    ap.add_argument("--out", default=OUT_JSON)
    ap.add_argument("--cache-out", default=None)
    ap.add_argument("--print", action="store_true")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    try:
        data, _ = extract(a.build, a.live)
    except ExtractError as e:
        log("FAILED: %s" % e)
        return 1
    text = dump(data)
    if a.print:
        sys.stdout.write(text)
        return 0
    if a.check:
        try:
            with open(a.out, encoding="utf-8") as f:
                cur = f.read()
        except OSError:
            cur = ""
        if cur != text:
            log("%s is stale; rerun tools/strike/extract_strike.py" % a.out)
            return 1
        log("up to date")
        return 0
    full = dict(data)
    try:
        full["cache_only"] = cache_extras(a.build, a.live)
    except ExtractError as e:
        log("FAILED: %s" % e)
        return 1
    write_atomic(a.out, text)
    cache_out = a.cache_out or os.path.join(a.live, "strike.json")
    write_atomic(cache_out, dump(full))
    log("wrote %s (%d bytes) and %s" % (a.out, len(text), cache_out))
    log("summary: %s" % json.dumps(data["summary"], ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
