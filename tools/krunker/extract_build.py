#!/usr/bin/env python3
"""Krunker build-constant extractor: game.deob.js -> shared/include/krunker_build.h + build.json.

Reads one Krunker client bundle and pulls out everything the official-server protocol needs
that changes per Krunker build, by pure static parsing (no Krunker JS is ever executed):

  AHKEY          the 2nd operand of the `(ahNum + AHKEY) & 0xff` step guarding socket.send
                 (skipped for packet cptR)
  clientVersion  e.g. '10.0.3' + 'r1' -> "10.0.3r1"
  buildVersion   the 32-char `'buildVersion': "..."` literal of the constants object
  packet tables  client->server and server->client `{'NAME': 'wire', ...}` objects
  cheat check    the CHEAT_CHECK keystream function (verbatim source), its salt, its formula
                 shape, and the FNV-1a parameters of the ccr hash
  layouts        per-player strides and field offsets of ADD_PLAYERS ("0") and UPDATE_PLAYERS
                 ("k"), the SYNC_PLAYER ("l") state layout, and the argument positions of
                 KILL_PLAYER, DO_DAMAGE, CHANGE_HEALTH and GET_SCORE, read from the client's
                 handlers (fields named by the Krunker property each value is stored in)
  extras         matchmaker / api URLs, Turnstile site key, regions, clientRevision,
                 customBuildVersion (reported; validated when present, not required)

Garbled identifiers (`iiìíìíí`, `_7259`, ...) differ per build, so every anchor is a string
literal or a number; identifiers are only ever resolved dynamically from those anchors. The
one-line string-array-obfuscated bundles (game.debug.js, game.min.js, game.manifest.json's
"source") are first normalised by statically decoding the string array: the array literal is
parsed, the rotation loop's checksum expression is evaluated in Python (it only uses
parseInt and arithmetic) and every `decoder(0xNNN)` call is replaced by its string.

Every extracted value is validated; on any failure the tool exits non-zero naming the anchor
that was not found and writes nothing.

usage:
  python3 -I tools/krunker/extract_build.py [SOURCE]            regenerate the header + build.json
  python3 -I tools/krunker/extract_build.py SOURCE --print      extract and print JSON only
  python3 -I tools/krunker/extract_build.py --check [--strict]  committed files current? (CI)
  python3 -I tools/krunker/extract_build.py --from-json         re-render the header from build.json

SOURCE is a path or an http(s) URL of game.deob.js / game.debug.js / game.min.js or a
game.manifest.json. Default: https://kru.eli.gift/game.deob.js (conditional GET, cached by
ETag / Last-Modified under $XDG_CACHE_HOME/krunknative/krunker-build/), falling back to the
cache, then to ~/.cache/kn-official/game.deob.js when the URL is unreachable.

Exit status: 0 done, 1 the bundle does not extract (an anchor or a value changed shape: the message names
it), 3 the bundle could not be read (unreachable, no cache / local copy), 2 --check --strict unreachable.

Regeneration is skipped (exit 0, "up to date") when the bundle's sha256 and the generator
revision match the committed build.json; --force regenerates anyway.

environment: KN_FETCH_PROXY (e.g. http://127.0.0.1:8888), KN_FETCH_UA, KRUNKER_BUILD_URL.
"""

import argparse
import difflib
import hashlib
import json
import math
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request

GENERATOR_REV = 4  # bump when the output format or the extraction rules change
# (rev 4: the X-macros take X_, so a field named X (ADD_PLAYERS x) can be X_(X, ...): with X the field name
# itself was replaced by the caller's macro)
SCHEMA = 1         # KRUNKER_BUILD_SCHEMA: bump when the header's C interface changes

DEFAULT_URL = os.environ.get("KRUNKER_BUILD_URL", "https://kru.eli.gift/game.deob.js")
DEFAULT_LOCAL = os.path.expanduser("~/.cache/kn-official/game.deob.js")
DEFAULT_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/141.0.0.0 Safari/537.36")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_HEADER = os.path.join(ROOT, "shared", "include", "krunker_build.h")
OUT_JSON = os.path.join(ROOT, "tools", "krunker", "build.json")


def maps_module():
    """tools/krunker/maps.py: the official map table (build.json "maps" + shared/include/krunker_maps.h)."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import maps
    maps.xb = sys.modules[__name__]
    return maps


class ExtractError(Exception):
    pass


def fail(anchor, msg):
    raise ExtractError("%s: %s" % (anchor, msg))


def log(msg):
    print("extract_build: " + msg, file=sys.stderr)


# ------------------------------------------------------------------------------- JS lexing

STR = r"""(?:'(?:[^'\\\n]|\\.)*'|"(?:[^"\\\n]|\\.)*")"""
NUM = r"""(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)"""
IDENT = r"""(?:[^\W\d]|\$)(?:[\w$])*"""
STR_RE = re.compile(STR)
# a property access by name: ["name"], ['name'] or .name
def prop(name):
    return r"""(?:\[\s*(?:'%s'|"%s")\s*\]|\.\s*%s(?![\w$]))""" % (name, name, name)
def key(name):
    """An object-literal key: 'name':, "name":, name: or ['name']: (computed)."""
    return r"""(?:\[\s*)?(?:'%s'|"%s"|(?<![\w$.])%s)(?:\s*\])?\s*:""" % (name, name, name)


_ESC = {"n": "\n", "r": "\r", "t": "\t", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}


def js_unquote(lit):
    """Value of a JS string literal (with its quotes). UTF-16 escapes are combined."""
    body = lit[1:-1]
    units = []
    i = 0
    n = len(body)
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
        elif e in "\r\n  ":  # line continuation
            i += 1
        else:
            units.extend(_utf16(e))
            i += 1
    data = b"".join(u.to_bytes(2, "little") for u in units)
    return data.decode("utf-16-le", "surrogatepass")


def _utf16(ch):
    o = ord(ch)
    if o < 0x10000:
        return [o]
    o -= 0x10000
    return [0xD800 + (o >> 10), 0xDC00 + (o & 0x3FF)]


def js_quote(s):
    """A double-quoted JS literal for s (valid JSON too)."""
    return json.dumps(s, ensure_ascii=False)


def js_num(tok):
    t = tok.strip()
    if t[:2] in ("0x", "0X"):
        return int(t, 16)
    v = float(t)
    return int(v) if v.is_integer() else v


def matching_close(src, pos, open_ch="{", close_ch="}"):
    """Index of the bracket closing the one at src[pos]. Skips strings, templates, comments.
    Regex literals are not recognised: only use it on code without them (small functions)."""
    depth = 0
    i = pos
    n = len(src)
    while i < n:
        c = src[i]
        if c in "'\"`":
            q = c
            i += 1
            while i < n and src[i] != q:
                i += 2 if src[i] == "\\" else 1
        elif c == "/" and src[i + 1:i + 2] == "/":
            i = src.find("\n", i)
            if i < 0:
                return -1
        elif c == "/" and src[i + 1:i + 2] == "*":
            i = src.find("*/", i) + 1
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


# ----------------------------------------------------------- string-array de-obfuscation

DECODER_RE = re.compile(
    r"function\s+(" + IDENT + r")\s*\(\s*" + IDENT + r"\s*,\s*" + IDENT + r"\s*\)\s*\{\s*"
    r"(?:var|let|const)\s+(" + IDENT + r")\s*=\s*(" + IDENT + r")\s*\(\s*\)\s*;\s*"
    r"return\s+\1\s*=\s*function\s*\(\s*(" + IDENT + r")\s*,\s*" + IDENT + r"\s*\)\s*\{\s*"
    r"\4\s*=\s*\4\s*-\s*(" + NUM + r")\s*;\s*"
    r"(?:var|let|const)\s+(" + IDENT + r")\s*=\s*\2\s*\[\s*\4\s*\]\s*;\s*return\s+\6\s*;?\s*\}")


def js_parse_int(s):
    """JS parseInt(s) with no radix, as a float (NaN when there are no digits)."""
    m = re.match(r"[\s﻿\xa0]*([+-]?)(0[xX][0-9a-fA-F]+|\d+)", s)
    if not m:
        return float("nan")
    v = int(m.group(2), 16) if m.group(2)[:2].lower() == "0x" else int(m.group(2))
    return float(-v if m.group(1) == "-" else v)


class _Checksum:
    """Evaluates the rotation loop's checksum: + - * / unary, parens, numbers, parseInt(D(n))."""
    TOK = re.compile(r"\s*(?:(parseInt)\s*\(\s*" + IDENT + r"\s*\(\s*(" + NUM + r")\s*\)\s*\)|(" +
                     NUM + r")|([-+*/()]))")

    def __init__(self, expr):
        self.toks = []
        pos = 0
        expr = expr.strip()
        while pos < len(expr):
            m = self.TOK.match(expr, pos)
            if not m or m.end() == pos:
                raise ValueError("unsupported checksum expression near %r" % expr[pos:pos + 40])
            if m.group(1):
                self.toks.append(("pi", js_num(m.group(2))))
            elif m.group(3):
                self.toks.append(("n", float(js_num(m.group(3)))))
            else:
                self.toks.append(("op", m.group(4)))
            pos = m.end()

    def eval(self, lookup):
        self.i = 0
        self.lookup = lookup
        v = self._add()
        if self.i != len(self.toks):
            raise ValueError("trailing tokens in checksum expression")
        return v

    def _peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def _add(self):
        v = self._mul()
        while self._peek() in (("op", "+"), ("op", "-")):
            op = self.toks[self.i][1]
            self.i += 1
            r = self._mul()
            v = v + r if op == "+" else v - r
        return v

    def _mul(self):
        v = self._unary()
        while self._peek() in (("op", "*"), ("op", "/")):
            op = self.toks[self.i][1]
            self.i += 1
            r = self._unary()
            if op == "*":
                v = v * r
            elif r == 0:
                v = float("nan") if v == 0 or math.isnan(v) else math.copysign(math.inf, v)
            else:
                v = v / r
        return v

    def _unary(self):
        t = self._peek()
        if t == ("op", "-"):
            self.i += 1
            return -self._unary()
        if t == ("op", "+"):
            self.i += 1
            return self._unary()
        if t == ("op", "("):
            self.i += 1
            v = self._add()
            if self._peek() != ("op", ")"):
                raise ValueError("unbalanced checksum expression")
            self.i += 1
            return v
        self.i += 1
        if t[0] == "n":
            return t[1]
        if t[0] == "pi":
            return js_parse_int(self.lookup(int(t[1])))
        raise ValueError("bad checksum token %r" % (t,))


def deobfuscate(src):
    """Replace every string-array decoder call with its string. Returns (src, info) unchanged
    when the bundle has no string array (game.deob.js, v10.js)."""
    decoders = list(DECODER_RE.finditer(src))
    if not decoders:
        return src, None
    info = []
    for d in decoders:
        dec, arrfn, offset = d.group(1), d.group(3), js_num(d.group(5))
        # function ARRFN(){var X=[ 'a', "b", ... ];ARRFN=function(){return X;};return ARRFN();}
        am = re.compile(r"function\s+" + re.escape(arrfn) + r"\s*\(\s*\)\s*\{\s*(?:var|let|const)\s+" +
                        IDENT + r"\s*=\s*\[").search(src)
        if not am:
            fail("string-array", "decoder %s found but not its array function %s" % (dec, arrfn))
        arr = []
        pos = am.end()
        item = re.compile(r"\s*(" + STR + r")\s*(,|\])")
        while True:
            m = item.match(src, pos)
            if not m:
                fail("string-array", "array of %s is not a plain string list near offset %d" % (arrfn, pos))
            arr.append(js_unquote(m.group(1)))
            pos = m.end()
            if m.group(2) == "]":
                break
        # (function(GETARR, TARGET){ ... try{ var v = CHECKSUM; if (v === TARGET) break; ...}(ARRFN, 0xNNN))
        rm = re.compile(r"\}\s*\(\s*" + re.escape(arrfn) + r"\s*,\s*(" + NUM + r")\s*\)").search(src)
        if not rm:
            fail("string-array", "rotation call (%s, <checksum>) not found" % arrfn)
        target = float(js_num(rm.group(1)))
        loop_start = src.rfind("while", 0, rm.start())
        cm = re.compile(r"try\s*\{\s*(?:var|let|const)\s+(" + IDENT + r")\s*=\s*(.*?);\s*if\s*\(\s*\1\s*===\s*" +
                        IDENT + r"\s*\)\s*break", re.S).search(src, loop_start, rm.start())
        if loop_start < 0 or not cm:
            fail("string-array", "rotation checksum of %s not found" % arrfn)
        try:
            checksum = _Checksum(cm.group(2))
        except ValueError as e:
            fail("string-array", str(e))
        rot = list(arr)
        for shift in range(len(arr) + 1):
            try:
                v = checksum.eval(lambda i: rot[i - offset])
            except IndexError:
                v = float("nan")
            if v == target:
                break
            rot.append(rot.pop(0))
        else:
            fail("string-array", "no rotation of %s's %d strings matches checksum %d" %
                 (arrfn, len(arr), int(target)))
        # every alias of the decoder: `var X = DEC` / `X = DEC,` (transitively)
        assigned = {}  # rhs identifier -> identifiers assigned from it
        for m in re.finditer(r"(?<![\w$.])(" + IDENT + r")\s*=\s*(" + IDENT + r")(?=\s*[,;)}])", src):
            assigned.setdefault(m.group(2), set()).add(m.group(1))
        aliases = {dec}
        todo = [dec]
        while todo:
            for a2 in assigned.get(todo.pop(), ()):
                if a2 not in aliases:
                    aliases.add(a2)
                    todo.append(a2)
        lo, hi = offset, offset + len(rot)
        call = re.compile(r"(?<![\w$.])(" + IDENT + r")\s*\(\s*(" + NUM + r")\s*\)")
        count = [0]

        def sub(m):
            if m.group(1) not in aliases:
                return m.group(0)
            idx = js_num(m.group(2))
            if not isinstance(idx, int) or not lo <= idx < hi:
                return m.group(0)
            count[0] += 1
            return js_quote(rot[idx - offset])
        src = call.sub(sub, src)
        # drop the now-dead `var X = DECODER_ALIAS;` / `var X = DECODER_ALIAS,` declarations
        dead = re.compile(r"\bvar\s+(" + IDENT + r")\s*=\s*(" + IDENT + r")\s*([;,])")
        src = dead.sub(lambda m: m.group(0) if m.group(2) not in aliases or m.group(1) not in aliases
                       else ("" if m.group(3) == ";" else "var "), src)
        info.append({"decoder": "string-array", "strings": len(arr), "offset": offset,
                     "rotation": shift, "aliases": len(aliases), "calls": count[0]})
    return src, info


# ------------------------------------------------------------------ value resolution

def resolve_string(src, name, near, depth=0):
    """Resolve identifier `name` to a string by finding its assignment closest to `near`
    (preferring the last one before it) and evaluating `'lit' (+ ident | + 'lit')*`."""
    if depth > 4:
        return None
    pat = re.compile(r"(?<![\w$.])" + re.escape(name) + r"\s*=(?!=)\s*")
    before = after = None
    for m in pat.finditer(src):
        if m.start() <= near:
            before = m
        else:
            after = m
            break
    best = before or after
    if not best:
        return None
    return eval_concat(src, best.end(), depth)


def eval_concat(src, pos, depth=0):
    term = re.compile(r"\s*(?:(" + STR + r")|(" + IDENT + r"))\s*")
    out = []
    while True:
        m = term.match(src, pos)
        if not m:
            return None
        if m.group(1):
            out.append(js_unquote(m.group(1)))
        else:
            v = resolve_string(src, m.group(2), m.start(), depth + 1)
            if v is None:
                return None
            out.append(v)
        pos = m.end()
        if src.startswith("+", pos):
            pos += 1
            continue
        if pos >= len(src) or src[pos] in ";,)}\n":
            return "".join(out)
        return None


def find_function(src, name, near=None):
    """[(start, body_open, end)] of `function NAME(` / `NAME = function (` definitions."""
    pat = re.compile(r"(?:function\s+" + re.escape(name) + r"|(?<![\w$.])" + re.escape(name) +
                     r"\s*=\s*function(?:\s+" + IDENT + r")?)\s*\(")
    out = []
    for m in pat.finditer(src):
        ob = src.find("{", m.end())
        cb = matching_close(src, ob) if ob >= 0 else -1
        if cb > 0:
            out.append((m.start(), ob, cb + 1))
    if near is not None:
        out.sort(key=lambda t: abs(t[0] - near))
    return out


# ------------------------------------------------------------------------- extractors

AHNUM = prop("ahNum")
NUMG = "(" + NUM + ")"
ADD_MASK = r"(?:&\s*(?:0xff|0xFF|255)|%\s*(?:256|0x100))"


def extract_ahkey(src):
    cands = []
    for g in re.finditer(r"""(?:'cptR'|"cptR")""", src):
        win = src[g.end():g.end() + 400]
        m = re.search(r"this\s*" + AHNUM + r"\s*=\s*(" + IDENT + r")\s*\(\s*(?:this\s*" + AHNUM + r"\s*,\s*" +
                      NUMG + r"|" + NUMG + r"\s*,\s*this\s*" + AHNUM + r")\s*\)", win)
        inline = re.search(r"this\s*" + AHNUM + r"\s*=\s*\(?\s*this\s*" + AHNUM + r"\s*\+\s*" + NUMG +
                           r"\s*\)?\s*" + ADD_MASK, win)
        after = src[g.end():g.end() + 1200]
        if not re.search(r"socket" + r"""['"]?\s*\]?\s*""" + r"(?:\[\s*['\"]send['\"]\s*\]|\.\s*send)", after):
            continue
        if m:
            helper = m.group(1)
            val = js_num(m.group(2) or m.group(3))
            ok = False
            for start, ob, end in find_function(src, helper, g.start()):
                body = src[start:end]
                if re.search(r"\(\s*(" + IDENT + r")\s*,\s*(" + IDENT + r")\s*\)\s*\{\s*return\s*\(?\s*(?:\1\s*\+\s*\2|\2\s*\+\s*\1)"
                             r"\s*\)?\s*" + ADD_MASK + r"\s*;?\s*\}$", body):
                    ok = True
                    break
            if not ok:
                fail("AHKEY", "ahNum step calls %s(this.ahNum, %s) but no definition of %s is "
                     "`return a + b & 0xff`" % (helper, m.group(2) or m.group(3), helper))
            cands.append((val, "helper", g.start()))
        elif inline:
            cands.append((js_num(inline.group(1)), "inline", g.start()))
    vals = sorted(set(c[0] for c in cands))
    if not vals:
        fail("AHKEY", "no `this.ahNum = add(this.ahNum, N)` / `(this.ahNum + N) & 0xff` after a 'cptR' "
             "guard before socket.send")
    if len(vals) > 1:
        fail("AHKEY", "ambiguous: %s" % ", ".join(hex(v) for v in vals))
    v = vals[0]
    if not isinstance(v, int) or not 0 <= v <= 0xFFFFF:
        fail("AHKEY", "value %r out of range 0..0xfffff" % (v,))
    return v, cands[0][1]


def extract_trailer(src):
    """The 2-byte ahNum trailer must still be [n >> 4 & 0xf, n & 0xf] (wire-format check)."""
    m = re.search(r"\[\s*(" + IDENT + r")\s*>>\s*(?:0x4|4)\s*&\s*(?:0xf|15)\s*,\s*(?:(?:0xf|15)\s*&\s*\1|\1\s*&\s*(?:0xf|15))\s*\]", src)
    if not m:
        fail("ahNum trailer", "no `[n >> 4 & 0xf, 0xf & n]` encoder (the 2-byte frame trailer changed?)")
    return "nibbles"


def extract_client_version(src):
    vals = set()
    for m in re.finditer(key("clientVersion") + r"\s*", src):
        v = eval_concat(src, m.end())
        if v is not None:
            vals.add(v)
    if not vals:
        fail("clientVersion", "no `'clientVersion': <'x.y.z' + 'rN' | ident>` that resolves to a string")
    vals = sorted(vals)
    good = [v for v in vals if re.fullmatch(r"\d+\.\d+\.\d+[A-Za-z0-9]*", v)]
    if len(good) != 1:
        fail("clientVersion", "expected one x.y.z[rN] value, got %r" % vals)
    return good[0]


def extract_build_version(src):
    vals = set()
    for m in re.finditer(key("buildVersion") + r"\s*", src):
        v = eval_concat(src, m.end())
        if v is not None:
            vals.add(v)
    good = sorted(v for v in vals if re.fullmatch(r"[A-Za-z0-9]{32}", v))
    if len(good) != 1:
        fail("buildVersion", "expected one 32-char alphanumeric `'buildVersion': \"...\"`, got %r" % sorted(vals))
    return good[0]


PAIR = re.compile(r"\s*(?:\[\s*)?(" + STR + r"|" + IDENT + r")(?:\s*\])?\s*:\s*(" + STR + r")\s*(,|\})")
C2S_ANCHORS = ("ENTER_GAME", "CHEAT_CHECK_RESPONSE", "cptR")
S2C_ANCHORS = ("IO_INIT", "CHEAT_CHECK", "cpt")


def parse_table(src, open_pos):
    entries = []
    pos = open_pos + 1
    while True:
        m = PAIR.match(src, pos)
        if not m:
            if re.match(r"\s*\}", src[pos:pos + 64]) and entries:
                return entries
            return None
        k = m.group(1)
        entries.append((js_unquote(k) if k[0] in "'\"" else k, js_unquote(m.group(2))))
        pos = m.end()
        if m.group(3) == "}":
            return entries


def dedupe(entries):
    """JS object-literal semantics: first position, last value."""
    order, vals = [], {}
    for k, v in entries:
        if k not in vals:
            order.append(k)
        vals[k] = v
    return [(k, vals[k]) for k in order]


def extract_tables(src):
    found = {"c2s": [], "s2c": []}
    seen = set()
    for anchor in ("ENTER_GAME", "IO_INIT"):
        for m in re.finditer(key(anchor), src):
            ob = src.rfind("{", max(0, m.start() - 20000), m.start())
            if ob < 0 or ob in seen:
                continue
            seen.add(ob)
            ent = parse_table(src, ob)
            if not ent or len(ent) < 20:
                continue
            keys = set(k for k, _ in ent)
            if all(a in keys for a in C2S_ANCHORS):
                found["c2s"].append(dedupe(ent))
            elif all(a in keys for a in S2C_ANCHORS):
                found["s2c"].append(dedupe(ent))
    out = {}
    for side, anchors in (("c2s", C2S_ANCHORS), ("s2c", S2C_ANCHORS)):
        tabs = found[side]
        if not tabs:
            fail("packet table " + side, "no `{'NAME': 'wire', ...}` object with keys %s" % ", ".join(anchors))
        if any(t != tabs[0] for t in tabs[1:]):
            fail("packet table " + side, "%d different candidate tables" % len(tabs))
        out[side] = tabs[0]
    return out


def c_ident(name):
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return "_" + s if s[:1].isdigit() else s


def check_table_idents(tables):
    for side, ent in tables.items():
        ids = {}
        for k, _ in ent:
            ci = c_ident(k)
            if ci in ids:
                fail("packet table " + side, "names %r and %r map to the same C identifier %s" % (ids[ci], k, ci))
            ids[ci] = k


FNV_OFFSET = r"(?:0x811c9dc5|0x811C9DC5|2166136261)"
FNV_PRIME = r"(?:0x1000193|16777619)"
JS_GLOBALS = {"Uint8Array", "Array", "Math", "String", "JSON", "Buffer", "undefined", "Number", "Object",
              "TextEncoder", "TextDecoder", "atob", "btoa", "window", "eval", "this", "new", "var", "let",
              "const", "for", "return", "function", "if", "else", "void", "typeof", "null", "true", "false",
              "try", "catch", "throw", "in", "of", "async", "await", "while", "do", "break", "continue"}

# Shapes of the cheat-check code, as canonical_js() prints them: identifiers renamed in order of
# first appearance (P* the function's params, SALT the salt, KS the keystream function, V* the
# rest), properties as .name, numbers in decimal, strings double-quoted, braces / semicolons
# dropped (minifiers differ on them). A Krunker build that changes either shape fails the
# extraction instead of producing a header the cc handler would answer wrongly.
#
# Keystream v1:
#   ks[i] = ((nonce.charCodeAt(i % nonce.length) ^ salt[i % salt.length]) + i) & 0xff, i < len
KS_FORMULA_V1 = ('function(P0,P1)for(varV0=SALT,V1=newUint8Array(P1),V2=0V2<P1V2++)'
                 'V1[V2]=(P0.charCodeAt(V2%P0.length)^V0[V2%V0.length])+V2&255returnV1')
# Handler v1, function (nonce, unused, ciphertextB64), also dropping `var` and `,`:
#   plain = JSON.parse(utf8decode(b64decode(ciphertext) ^ KS(nonce, n)))
#   R = (0, eval)(plain), or {r: null, e: {_eval: message || "eval_error"}} when eval throws
#   send(CHEAT_CHECK_RESPONSE, nonce, b64(utf8(JSON.stringify(R)) ^ KS(nonce, n)),
#        hex8(fnv(nonce + JSON.stringify(R))))
CC_HANDLER_V1 = (
    'function(P0P1P2)tryV0V1=function(V2V3)V4V4="undefined"!=typeofBuffer?newUint8Array(Buffer.from(V2"base64"))'
    ':newUint8Array(atob(V2).split("").map(function(V5)returnV5.charCodeAt(0)))for(V6=KS(V3V4.length)V7=newUint8Array'
    '(V4.length)V8=0V8<V4.lengthV8++)V7[V8]=V4[V8]^V6[V8]V9=newTextDecoder().decode(V7)returnJSON.parse(V9)(P2P0)'
    'tryV0=(0eval)(V1)catch(V10)V0="r":null"e":"_eval":V10.message||"eval_error"V11=function(V12V13)for(V14=JSON.'
    'stringify(V12)V15=newTextEncoder().encode(V14)V16=KS(V13V15.length)V17=newUint8Array(V15.length)V18=0V18<V15.'
    'lengthV18++)V17[V18]=V15[V18]^V16[V18]return"undefined"!=typeofBuffer?Buffer.from(V17).toString("base64"):btoa'
    '(String.fromCharCode.apply(StringV19(V17)))(V0P0)V20=function(V21V22)for(V23=V22+JSON.stringify(V21)V24=2166136261'
    'V25=0V25<V23.lengthV25++)V24=16777619*(V24^=V23.charCodeAt(V25))|0return(V24>>>0).toString(16).padStart(8"0")'
    '(V0P0)V26.send(V27.CHEAT_CHECK_RESPONSEP0V11V20)catch(V28)')

_TOK = re.compile(r"\s+|(" + STR + r")|(" + NUM + r")|(" + IDENT + r")|(.)", re.S)


def canonical_js(code, names, drop=("{", "}", ";")):
    """Identifier-independent form of a JS snippet (see KS_FORMULA_V1). `names` pre-maps
    identifiers (params, salt, keystream)."""
    toks = []
    for m in _TOK.finditer(code):
        if m.group(1):
            toks.append(("s", js_unquote(m.group(1))))
        elif m.group(2):
            v = js_num(m.group(2))
            toks.append(("n", str(v)))
        elif m.group(3):
            toks.append(("i", m.group(3)))
        elif m.group(4):
            toks.append(("p", m.group(4)))
    # ["prop"] -> .prop
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if (t == ("p", "[") and i + 2 < len(toks) and toks[i + 1][0] == "s" and toks[i + 2] == ("p", "]")
                and re.fullmatch(r"[A-Za-z_$][\w$]*", toks[i + 1][1])):
            out += [("p", "."), ("prop", toks[i + 1][1])]
            i += 3
            continue
        out.append(t)
        i += 1
    names = dict(names)
    nloc = 0
    res = []
    for j, (kind, v) in enumerate(out):
        if kind == "i" and j and out[j - 1] == ("p", "."):
            kind = "prop"
        if kind == "i" and j == 1 and out[0] == ("i", "function"):
            continue  # the function's own (garbled) name
        if kind == "i" and v not in JS_GLOBALS:
            if v not in names:
                names[v] = "V%d" % nloc
                nloc += 1
            v = names[v]
        elif kind == "s":
            v = json.dumps(v, ensure_ascii=False)
        if v in drop and kind in ("p", "i"):
            continue
        res.append(v)
    return "".join(res)


def extract_cheat(src):
    sends = [m for m in re.finditer(r"(?:\[\s*(?:'CHEAT_CHECK_RESPONSE'|\"CHEAT_CHECK_RESPONSE\")\s*\]|"
                                    r"\.\s*CHEAT_CHECK_RESPONSE(?![\w$]))", src)]
    if not sends:
        fail("cheat check", "no send of packet CHEAT_CHECK_RESPONSE")
    results = []
    for s in sends:
        lo = max(0, s.start() - 6000)
        win = src[lo:s.start()]
        fo = list(re.finditer(FNV_OFFSET, win))
        fp = list(re.finditer(FNV_PRIME, win))
        if not fo or not fp or "eval" not in win:
            continue
        # the handler: innermost `function (` whose body encloses both eval and the send
        handler = None
        for fm in re.finditer(r"function\s*(?:" + IDENT + r")?\s*\(([^)]*)\)\s*\{", win):
            ob = lo + fm.end() - 1
            cb = matching_close(src, ob)
            ev = src.find("eval", ob, s.start())
            if cb > s.start() and ev > 0:
                handler = (lo + fm.start(), cb + 1, [p.strip() for p in fm.group(1).split(",") if p.strip()])
        if not handler:
            continue
        hsrc = src[handler[0]:handler[1]]
        # keystream calls: KS(nonce, buf.length) -- the name used at least twice (decrypt + encrypt)
        calls = {}
        for cm in re.finditer(r"(?<![\w$.])(" + IDENT + r")\s*\(\s*(" + IDENT + r")\s*,\s*" + IDENT + r"\s*" +
                              prop("length") + r"\s*\)", hsrc):
            calls.setdefault(cm.group(1), []).append(cm.group(2))
        ks_names = [n for n, a in calls.items() if len(a) >= 2 and n not in JS_GLOBALS]
        if len(ks_names) != 1:
            fail("cheat keystream", "expected one function called as KS(nonce, buf.length) twice in the "
                 "cc handler, found %r" % sorted(calls))
        ks = ks_names[0]
        defs = []
        for start, ob, end in find_function(src, ks, handler[0]):
            body = src[start:end]
            if "charCodeAt" in body and "^" in body and re.search(r"&\s*(?:0xff|255)", body):
                defs.append((start, ob, end))
        if not defs:
            fail("cheat keystream", "no definition of %s with charCodeAt / ^ / & 0xff" % ks)
        start, ob, end = defs[0]
        ks_src = src[start:end]
        params = [p.strip() for p in re.match(r"[^(]*\(([^)]*)\)", ks_src).group(1).split(",")]
        # salt: a free identifier of the body bound to an int array or string literal, or an inline literal
        body = src[ob:end]
        free = []
        for im in re.finditer(r"(?<![\w$.'\"])" + IDENT, body):
            w = im.group(0)
            if w in JS_GLOBALS or w in params or w in free:
                continue
            if re.search(r"(?:var|let|const|,)\s*" + re.escape(w) + r"\s*=", body):
                continue  # local
            free.append(w)
        salt = salt_name = salt_decl = None
        for w in free:
            dm = None
            for dm_ in re.finditer(r"(?:\b(?:var|let|const)\s+|,\s*)" + re.escape(w) + r"\s*=\s*(\[[^\]]*\]|" + STR +
                                   r")\s*[;,]", src[max(0, start - 200000):start]):
                dm = dm_
            if not dm:
                continue
            lit = dm.group(1)
            if lit.startswith("["):
                items = [x.strip() for x in lit[1:-1].split(",") if x.strip()]
                if not all(re.fullmatch(NUM, x) for x in items):
                    continue
                vals = [js_num(x) for x in items]
                if not all(isinstance(v, int) and 0 <= v <= 255 for v in vals):
                    continue
                salt = "".join(map(chr, vals))
            else:
                salt = js_unquote(lit)
            salt_name = w
            salt_decl = "var %s = %s;" % (w, lit)
            break
        if salt is None:
            inl = [js_unquote(x) for x in STR_RE.findall(body) if js_unquote(x) not in ("charCodeAt", "length")]
            if len(inl) == 1:
                salt = inl[0]
        if salt is None:
            fail("cheat salt", "keystream %s uses no int-array / string salt literal" % ks)
        if not (1 <= len(salt) <= 64 and all(0x20 <= ord(c) < 0x7F for c in salt)):
            fail("cheat salt", "salt %r is not 1-64 printable ASCII chars" % salt)
        pre = {p: "P%d" % i for i, p in enumerate(params)}
        if salt_name:
            pre[salt_name] = "SALT"
        canon = canonical_js(ks_src, pre)
        if canon != KS_FORMULA_V1:
            fail("cheat keystream", "formula shape changed (update krunker_cheat and KS_FORMULA_*):\n  %s\n  "
                 "expected %s" % (canon, KS_FORMULA_V1))
        hpre = {p: "P%d" % i for i, p in enumerate(handler[2])}
        hpre[ks] = "KS"
        hcanon = canonical_js(hsrc, hpre, drop=("{", "}", ";", ",", "var"))
        if hcanon != CC_HANDLER_V1:
            k = next((i for i, (x, y) in enumerate(zip(hcanon, CC_HANDLER_V1)) if x != y),
                     min(len(hcanon), len(CC_HANDLER_V1)))
            fail("cheat handler", "cc handler shape changed (update krunker_cheat and CC_HANDLER_*) at "
                 "canonical offset %d:\n  got      ...%s\n  expected ...%s\n  full: %s" % (
                     k, hcanon[max(0, k - 60):k + 60], CC_HANDLER_V1[max(0, k - 60):k + 60], hcanon))
        fnv_off = js_num(fo[-1].group(0))
        fnv_prime = js_num(fp[-1].group(0))
        hm = re.search(r"(" + IDENT + r")\s*=\s*" + FNV_PRIME + r"\s*\*\s*\(\s*\1\s*\^=", hsrc) or \
            re.search(r"(" + IDENT + r")\s*=\s*\(\s*\1\s*\^=[^)]*\)\s*\*\s*" + FNV_PRIME, hsrc)
        if re.search(r"Math\s*" + prop("imul") + r"\s*\(", hsrc):
            mul = "imul"
        elif hm:
            mul = "double"
        else:
            fail("cheat fnv", "FNV step is neither `h = PRIME * (h ^= c) | 0` nor Math.imul")
        results.append({
            "keystream_name": ks, "keystream_js": ks_src, "salt": salt, "salt_js": salt_decl,
            "formula": "v1", "fnv_offset": fnv_off, "fnv_prime": fnv_prime, "fnv_multiply": mul,
            "handler_js": hsrc,
            "standalone_js": "(function () {\n  %s\n  return %s;\n})()" % (
                salt_decl or "", ks_src) if salt_decl else "(function () {\n  return %s;\n})()" % ks_src,
        })
    if not results:
        fail("cheat check", "no CHEAT_CHECK_RESPONSE send preceded by eval + FNV-1a constants "
             "0x811c9dc5 / 0x1000193")
    if any(r["salt"] != results[0]["salt"] or r["keystream_js"] != results[0]["keystream_js"] for r in results):
        fail("cheat check", "%d differing cheat-check handlers" % len(results))
    r = results[0]
    if r["fnv_offset"] != 0x811C9DC5 or r["fnv_prime"] != 0x1000193:
        fail("cheat fnv", "unexpected FNV parameters")
    return r


# ------------------------------------------------- per-player layouts and argument positions
#
# The S2C handlers are found through the client's handler map, `define(map, NAMES['ADD_PLAYERS'],
# handler)` (the only place a packet name is followed by a bare identifier and `)`), and their
# bodies are read as text. Field and argument names are Krunker's own property names, taken from
# what the handler does with each value (`player['xDire'] = a[i + 0x12]`), so a reordering moves
# the generated constant instead of silently desynchronising krunker_link.c.

ANYPROP = r"""(?:\[\s*(?:'([\w$]+)'|"([\w$]+)")\s*\]|\.\s*([A-Za-z_$][\w$]*))"""
ANYPROP_RE = re.compile(ANYPROP)
ZERO = r"(?:0x0|0)"


def _pname(m, base=1):
    return m.group(base) or m.group(base + 1) or m.group(base + 2)


def _word(name):
    return r"(?<![\w$.])" + re.escape(name) + r"(?![\w$])"


def _alt(names):
    return r"(?<![\w$.])(" + "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True)) + r")(?![\w$])"


def split_args(src, open_pos):
    """Top-level comma-separated texts of the call / array whose bracket is at src[open_pos],
    and the index of its closing bracket."""
    close = {"(": ")", "[": "]", "{": "}"}[src[open_pos]]
    end = matching_close(src, open_pos, src[open_pos], close)
    if end < 0:
        return None, -1
    out, depth, start, i = [], 0, open_pos + 1, open_pos + 1
    while i < end:
        c = src[i]
        if c in "'\"`":
            q = c
            i += 1
            while i < end and src[i] != q:
                i += 2 if src[i] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(src[start:i].strip())
            start = i + 1
        i += 1
    last = src[start:end].strip()
    if last or out:
        out.append(last)
    return out, end


def s2c_handler(src, tables, name, want):
    """(params, body) of the client handler of S2C packet `name`; `want(body)` must hold."""
    if name not in dict(tables["s2c"]):
        fail("%s handler" % name, "no packet %s in the server -> client table" % name)
    found = {}
    for m in re.finditer(prop(name) + r"\s*,\s*(" + IDENT + r")\s*\)", src):
        for start, ob, end in find_function(src, m.group(1), m.start()):
            pm = re.match(r"[^(]*\(([^)]*)\)\s*\{", src[start:ob + 1])
            body = src[ob:end]
            if pm and want(body):
                found[body] = [p.strip() for p in pm.group(1).split(",") if p.strip()]
                break
    if len(found) != 1:
        fail("%s handler" % name, "expected one handler registered as `%s, fn)` whose body matches, "
             "found %d" % (name, len(found)))
    (body, params), = found.items()
    return params, body


def _snake(name):
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name.strip("_$"))
    return re.sub(r"[^A-Za-z0-9]+", "_", s).upper()


class _Fields:
    """offset -> Krunker property name, first writer wins; a name read from two offsets keeps the
    first (the handler's own assignments are collected first: they run after players.add)."""

    def __init__(self, label):
        self.label, self.by_off, self.by_name, self.shadowed = label, {}, {}, []

    def add(self, off, name, via):
        if name in ("length",):
            return
        if off in self.by_off:
            if self.by_off[off]["name"] != name:
                self.shadowed.append({"offset": off, "name": name, "via": via})
            return
        if name in self.by_name:
            self.shadowed.append({"offset": off, "name": name, "via": via})
            return
        self.by_off[off] = {"offset": off, "name": name, "via": via}
        self.by_name[name] = off

    def require(self, names, anchor):
        missing = [n for n in names if n not in self.by_name]
        if missing:
            fail(anchor, "fields not found: %s (found %s)" % (", ".join(missing), ", ".join(
                "%s@%d" % (f["name"], f["offset"]) for f in self.list())))

    def list(self):
        return [self.by_off[k] for k in sorted(self.by_off)]


def _elem_fields(body, elem, fields, groups=1):
    """Read-site rules shared by the "0" / "k" / "l" handlers. `elem` matches one element read
    with its offset in group 1 (or `groups` offset groups). Records into `fields`."""
    num = lambda m, k=1: js_num(m.group(k))  # noqa: E731
    # player['prop'] = [wrap(] [null == E ? .. :] E   (wrap: parseInt, a deg->rad helper, ...)
    for m in re.finditer(ANYPROP + r"\s*=\s*(?:" + IDENT + r"\s*\(\s*|null\s*==\s*)?" + elem, body):
        fields.add(num(m, 4), _pname(m), "assign")
    # players.swapWeapon(p, .., E) / setBusy(p, E): the method's noun
    for m in re.finditer(ANYPROP + r"\s*\(", body):
        meth = _pname(m)
        vm = re.fullmatch(r"(?:set|swap|update)([A-Z][\w$]*)", meth or "")
        if not vm:
            continue
        args, _ = split_args(body, m.end() - 1)
        for a in args or ():
            em = re.fullmatch(elem, a)
            if em:
                fields.add(num(em), vm.group(1)[0].lower() + vm.group(1)[1:], meth)
    # players.findBySid(E) / findById(E)
    for m in re.finditer(prop("findBySid") + r"\s*\(\s*" + elem + r"\s*\)", body):
        fields.add(num(m), "sid", "findBySid")
    for m in re.finditer(prop("findById") + r"\s*\(\s*" + elem + r"\s*\)", body):
        fields.add(num(m), "id", "findById")
    # E == SOCKET['socketId']: the socket id of "you"
    for m in re.finditer(elem + r"\s*==\s*" + IDENT + r"\s*" + prop("socketId"), body):
        fields.add(num(m), "id", "socketId")
    # var v = E; ... v ? / v && player['prop']
    for m in re.finditer(r"(?<![\w$.\]])(" + IDENT + r")\s*=\s*" + elem + r"\s*[;,]", body):
        v = m.group(1)
        um = re.compile(_word(v) + r"\s*(?:\?|&&)\s*" + IDENT + r"\s*" + ANYPROP).search(body, m.end())
        if um:
            fields.add(js_num(m.group(2)), _pname(um, 1), "local")


def extract_add_players(src, tables):
    anchor = "ADD_PLAYERS layout"
    params, body = s2c_handler(src, tables, "ADD_PLAYERS",
                               lambda b: re.search(prop("players") + r"\s*" + prop("add") + r"\s*\(", b))
    arr = params[0] if params else None
    lm = arr and re.search(r"for\s*\(\s*(?:var|let)\s+(" + IDENT + r")\s*=\s*" + ZERO + r"\s*;\s*\1\s*<\s*" +
                           re.escape(arr) + r"\s*" + prop("length") + r"\s*;", body)
    if not lm:
        fail(anchor, "no `for (var i = 0; i < list.length;)` over the handler's first argument")
    idx = lm.group(1)
    strides = set(js_num(x) for x in re.findall(_word(idx) + r"\s*\+=\s*(" + NUM + r")\s*[;,)]", body))
    if len(strides) != 1:
        fail(anchor, "expected one `i += STRIDE`, got %r" % sorted(strides))
    stride = strides.pop()
    f = _Fields(anchor)
    body0 = re.sub(re.escape(arr) + r"\s*\[\s*" + re.escape(idx) + r"\s*\]", arr + "[" + idx + " + 0x0]", body)
    elem = re.escape(arr) + r"\s*\[\s*" + re.escape(idx) + r"\s*\+\s*(" + NUM + r")\s*\]"
    _elem_fields(body0, elem, f)
    # players.add(a[i], a[i + 1], ...): argument j -> players.add's parameter j
    am = re.search(prop("players") + r"\s*" + prop("add") + r"\s*\(", body0)
    args, _ = split_args(body0, am.end() - 1)
    pos = {}
    for j, a in enumerate(args or ()):
        em = re.fullmatch(elem, a)
        if em:
            pos[j] = js_num(em.group(1))
    defs = []
    for dm in re.finditer(r"this\s*" + prop("add") + r"\s*=\s*function\s*\(([^)]*)\)\s*\{", src):
        ps = [p.strip() for p in dm.group(1).split(",") if p.strip()]
        if len(ps) < len(args):
            continue
        cb = matching_close(src, dm.end() - 1)
        b = src[dm.end() - 1:cb + 1]
        if re.search(prop("sid") + r"\s*=", b):
            defs.append((ps, b, dm.start()))
    if len(defs) != 1:
        fail(anchor, "expected one `this.add = function (<%d+ params>)` assigning .sid, found %d" % (len(args), len(defs)))
    ps, abody, apos = defs[0]
    pmap = {ps[j]: off for j, off in pos.items()}
    pr = _alt(pmap)
    # obj['prop'] = PARAM [|| ..] / obj['id'] == PARAM
    for m in re.finditer(ANYPROP + r"\s*=\s*" + pr + r"(?=\s*(?:\|\||[;,)]))", abody):
        f.add(pmap[m.group(4)], _pname(m), "players.add")
    for m in re.finditer(ANYPROP + r"\s*===?\s*" + pr, abody):
        f.add(pmap[m.group(4)], _pname(m), "players.add")
    # player.init(z, x, y, name, ..) / player.setClass(game, classIndex, ..): the player class's methods
    cm = re.search(r"\(\s*(" + IDENT + r")\s*=\s*new\s+(" + IDENT + r")\s*\(", abody)
    if cm:
        pv, cls = cm.group(1), cm.group(2)
        cdef = re.search(r"(?:function\s+" + re.escape(cls) + r"|(?<![\w$.])" + re.escape(cls) + r"\s*=\s*function)\s*\(", src)
        for mm in re.finditer(_word(pv) + r"\s*" + ANYPROP + r"\s*\(", abody):
            meth = _pname(mm)
            margs, _ = split_args(abody, mm.end() - 1)
            if not cdef or not margs or not any(a in pmap for a in margs):
                continue
            md = re.compile(r"this\s*" + prop(meth) + r"\s*=\s*function\s*\(([^)]*)\)\s*\{").search(src, cdef.start())
            if not md:
                continue
            qs = [q.strip() for q in md.group(1).split(",") if q.strip()]
            mb = src[md.end() - 1:matching_close(src, md.end() - 1) + 1]
            for j, a in enumerate(margs):
                if a in pmap and j < len(qs):
                    am2 = re.search(r"this\s*" + ANYPROP + r"\s*=\s*" + _word(qs[j]) + r"(?=\s*(?:\|\||[;,)]))", mb)
                    if am2:
                        f.add(pmap[a], _pname(am2), "player." + meth)
    f.require(("id", "sid", "x", "y", "z", "name", "classIndex", "health", "maxHealth", "team", "secIndex",
               "xDire", "attachIndex"), anchor)
    bad = [x for x in f.list() if not 0 <= x["offset"] < stride]
    if bad:
        fail(anchor, "offsets outside the %d stride: %r" % (stride, bad))
    # the server half of the bundle packs the same list: getPlayerAddData returns one array per player
    server = None
    gm = re.search(prop("getPlayerAddData") + r"\s*=\s*function\s*\([^)]*\)\s*\{\s*return\s*\[", src)
    if gm:
        items, _ = split_args(src, gm.end() - 1)
        server = len(items or ())
        if server != stride:
            fail(anchor, "the client reads %d values per player but getPlayerAddData packs %d" % (stride, server))
    return {"stride": stride, "server_stride": server, "fields": f.list(), "shadowed": f.shadowed}


def extract_update_players(src, tables):
    anchor = "UPDATE_PLAYERS layout"
    params, body = s2c_handler(src, tables, "UPDATE_PLAYERS",
                               lambda b: re.search(prop("findBySid") + r"\s*\(", b) and re.search(r"\+=\s*" + NUM, b))
    arr = params[0] if params else None
    lm = arr and re.search(r"for\s*\(\s*(?:var|let)\s+(" + IDENT + r")\s*=\s*" + ZERO + r"\s*;\s*\1\s*<\s*" +
                           re.escape(arr) + r"\s*" + prop("length") + r"\s*;", body)
    if not lm:
        fail(anchor, "no `for (var i = 0; i < list.length;)` over the handler's first argument")
    idx = lm.group(1)
    strides = set(js_num(x) for x in re.findall(_word(idx) + r"\s*\+=\s*(" + NUM + r")\s*[;,)]", body))
    if len(strides) != 1:
        fail(anchor, "expected one `i += STRIDE`, got %r" % sorted(strides))
    stride = strides.pop()
    body0 = re.sub(re.escape(arr) + r"\s*\[\s*" + re.escape(idx) + r"\s*\]", arr + "[" + idx + " + 0x0]", body)
    elem = re.escape(arr) + r"\s*\[\s*" + re.escape(idx) + r"\s*\+\s*(" + NUM + r")\s*\]"
    f = _Fields(anchor)
    _elem_fields(body0, elem, f)
    f.require(("sid", "x", "y", "z", "xDire", "yDire", "onGround", "crouchVal", "weapon", "aimVal", "grapple", "ping"),
              anchor)
    bad = [x for x in f.list() if not 0 <= x["offset"] < stride]
    if bad:
        fail(anchor, "offsets outside the %d stride: %r" % (stride, bad))
    return {"stride": stride, "args": {"list": 0, "sendRate": 1, "serverTime": 2} if len(params) >= 3 else None,
            "fields": f.list(), "shadowed": f.shadowed}


SYNC_REQUIRED = ("inputSN", "ping", "x", "y", "z", "velY", "velX", "velZ", "xDire", "onGround", "didJump", "didJumpW",
                 "wallJumpC", "onWall", "onLadder", "aimVal", "crouchVal", "weapon", "slideTimer", "canSlide",
                 "onTerrain", "xVelC", "zVelC", "grapple", "canSlideT", "_protected")


def extract_sync_player(src, tables):
    anchor = "SYNC_PLAYER layout"
    params, body = s2c_handler(src, tables, "SYNC_PLAYER", lambda b: re.search(prop("PONG"), b))
    if not params:
        fail(anchor, "the handler takes no argument")
    st = params[0]
    elem = re.escape(st) + r"\s*\[\s*(" + NUM + r")\s*\]"
    f = _Fields(anchor)
    # the input sequence: pending inputs up to it are dropped (`tmpInpts[k][0] <= s[0]`)
    im = re.search(prop("tmpInpts") + r"[^;]{0,120}?<=\s*" + elem, body)
    if im:
        f.add(js_num(im.group(1)), "inputSN", "tmpInpts")
    # the ping: `p = s[1]` shown in pingText
    for m in re.finditer(r"(?<![\w$.\]])(" + IDENT + r")\s*=\s*" + elem + r"\s*[;,]", body):
        if re.search(r"pingText", body[m.end():m.end() + 600]) and re.search(
                r"pingText\s*" + prop("textContent") + r"\s*=[^;]*?" + _word(m.group(1)), body[m.end():m.end() + 900]):
            f.add(js_num(m.group(2)), "ping", "pingText")
    # velocity.set(s[x], s[y], s[z])
    vm = re.search(prop("velocity") + r"\s*" + prop("set") + r"\s*\(", body)
    if vm:
        vargs, _ = split_args(body, vm.end() - 1)
        for axis, a in zip("XYZ", vargs or ()):
            em = re.fullmatch(elem, a)
            if em:
                f.add(js_num(em.group(1)), "vel" + axis, "velocity.set")
    _elem_fields(body, elem, f)
    # grapple = s[k] || null
    for m in re.finditer(ANYPROP + r"\s*=\s*" + elem + r"\s*\|\|\s*null", body):
        f.add(js_num(m.group(4)), _pname(m), "assign")
    # movement-schema values from s[BASE + k] (BASE + count: the controlled-entity sync)
    bases = set(js_num(x) for x in re.findall(re.escape(st) + r"\s*\[\s*(" + NUM + r")\s*\+\s*" + IDENT + r"\s*\]", body))
    if len(bases) != 1 or not re.search(prop("sendVals"), body):
        fail(anchor, "expected one `s[BASE + k]` read of the movement schema's sendVals, got %r" % sorted(bases))
    base = bases.pop()
    f.require(SYNC_REQUIRED, anchor)
    offs = sorted(f.by_off)
    if offs != list(range(base)):
        fail(anchor, "fields do not cover 0..%d exactly once before sendVals at %d: %r" % (base - 1, base, offs))
    return {"send_vals": base, "fields": f.list(), "shadowed": f.shadowed}


def _roles(anchor, params, found, required):
    """found: role -> param name. Validates and returns role -> argument index."""
    out = {}
    for role, p in found.items():
        if p is None:
            continue
        if p not in params:
            fail(anchor, "role %s resolved to %r, not a handler parameter" % (role, p))
        out[role] = params.index(p)
    missing = [r for r in required if r not in out]
    if missing:
        fail(anchor, "arguments not found: %s (found %r)" % (", ".join(missing), out))
    if len(set(out.values())) != len(out):
        fail(anchor, "two roles share one argument: %r" % out)
    return {"argc": len(params), "args": dict(sorted(out.items(), key=lambda kv: kv[1]))}


def _first(pattern, body, group=1):
    m = re.search(pattern, body)
    return m.group(group) if m else None


def extract_kill_player(src, tables):
    anchor = "KILL_PLAYER arguments"
    params, body = s2c_handler(src, tables, "KILL_PLAYER", lambda b: re.search(r"deathsVal", b))
    P = _alt(params)
    found = {}
    sids = [(m.group(1), m.group(2)) for m in re.finditer(
        r"(?<![\w$.\]])(" + IDENT + r")\s*=\s*[^;=]*?" + prop("findBySid") + r"\s*\(\s*" + P + r"\s*\)", body)]
    victim = killer = None
    for v, p in sids:
        if victim is None and re.search(prop("players") + r"\s*" + prop("kill") + r"\s*\(\s*" + _word(v) + r"\s*\)", body):
            victim = v
            found["victim"] = p
        km = re.search(prop("countSkinKill") + r"\s*\(\s*" + _word(v) + r"\s*,\s*" + P + r"\s*\)", body)
        if killer is None and km:
            killer = v
            found["killer"] = p
            found["info"] = km.group(1)
    found["deaths"] = _first(r"deathsVal\s*" + prop("textContent") + r"\s*=\s*" + P, body)
    found["hideFeed"] = _first(r"!\s*" + P + r"\s*&&[^;]{0,80}?" + prop("showKills"), body)
    if victim and killer:
        found["stats"] = _first(r"\}\s*\(\s*" + _word(victim) + r"\s*,\s*" + _word(killer) + r"\s*,\s*" + P + r"\s*,", body)
    return _roles(anchor, params, found, ("victim", "deaths", "killer", "stats", "info", "hideFeed"))


def extract_do_damage(src, tables):
    anchor = "DO_DAMAGE arguments"
    params, body = s2c_handler(src, tables, "DO_DAMAGE", lambda b: re.search(r"""['"]crit_0['"]""", b))
    P = _alt(params)
    found = {"target": _first(prop("findBySid") + r"\s*\(\s*" + P + r"\s*\)", body),
             "damage": _first(P + r"\s*=\s*Math\s*" + prop("ceil") + r"\s*\(\s*\1\s*\)", body),
             "crit": _first(P + r"\s*\?\s*['\"]crit_0['\"]", body),
             "silent": _first(P + r"\s*\|\|\s*" + IDENT + r"\s*" + prop("play") + r"\s*\(\s*" + P + r"\s*\?\s*['\"]crit_0", body)}
    m = re.search(P + r"\s*\?\s*" + P + r"\s*&&[^;]{0,200}?['\"]aikills['\"]", body)
    if m:
        found["ai"], found["kill"] = m.group(1), m.group(2)
    return _roles(anchor, params, found, ("target", "damage", "crit", "kill", "ai", "silent"))


def extract_change_health(src, tables):
    anchor = "CHANGE_HEALTH arguments"
    params, body = s2c_handler(src, tables, "CHANGE_HEALTH", lambda b: re.search(prop("maxHealth"), b) and
                               re.search(prop("findBySid"), b))
    P = _alt(params)
    found = {"health": _first(prop("health") + r"\s*=\s*" + P + r"\s*[;,]", body),
             "sid": _first(prop("findBySid") + r"\s*\(\s*" + P + r"\s*\)", body),
             "srcX": _first(r"null\s*!=\s*" + P + r"\s*&&\s*null\s*!=\s*\1\s*&&\s*function", body),
             "noHitMarker": _first(r"<\s*" + IDENT + r"\s*&&\s*!\s*" + P + r"\s*&&\s*\(\s*null\s*!=", body),
             "resetMax": _first(r"\|\|\s*" + P + r"\s*\)\s*&&\s*\(\s*" + IDENT + r"\s*" + prop("maxHealth") + r"\s*=", body)}
    if found["srcX"]:
        found["srcZ"] = _first(r"\}\s*\(\s*" + _word(found["srcX"]) + r"\s*,\s*" + P + r"\s*\)", body)
    return _roles(anchor, params, found, ("health", "sid", "srcX", "srcZ", "noHitMarker", "resetMax"))


def extract_get_score(src, tables):
    anchor = "GET_SCORE arguments"
    params, body = s2c_handler(src, tables, "GET_SCORE", lambda b: re.search(prop("classScores"), b))
    P = _alt(params)
    found = {"classIndex": _first(prop("classScores") + r"\s*\[\s*" + P + r"\s*\|\|", body),
             "relay": _first(r"!\s*" + P + r"\s*&&\s*" + IDENT + r"\s*&&\s*\(\s*" + IDENT + r"\s*" + prop("score") + r"\s*=", body)}
    m = re.search(prop("score") + r"\s*=\s*" + P + r"\s*\+\s*\(\s*" + P + r"\s*\?\s*" + ZERO + r"\s*:", body)
    if m:
        found["delta"], found["absolute"] = m.group(1), m.group(2)
    return _roles(anchor, params, found, ("delta", "relay", "absolute", "classIndex"))


def extract_show_bombardment(src, tables):
    """SHOW_BOMBARDMENT ("bmb"): the handler's one argument is the flat list of bombs (its physObj loop
    reads them 5 values at a time, :220832). The stride is the client's own constant."""
    anchor = "SHOW_BOMBARDMENT arguments"
    params, body = s2c_handler(src, tables, "SHOW_BOMBARDMENT", lambda b: re.search(prop("physObj"), b) and re.search(prop("stored"), b))
    P = _alt(params)
    found = {"bombs": _first(P + r"\s*" + prop("length"), body)}
    return _roles(anchor, params, found, ("bombs",))


def extract_layouts(src, tables):
    out = {
        "add_players": extract_add_players(src, tables),
        "update_players": extract_update_players(src, tables),
        "sync_player": extract_sync_player(src, tables),
        "kill_player": extract_kill_player(src, tables),
        "do_damage": extract_do_damage(src, tables),
        "change_health": extract_change_health(src, tables),
        "get_score": extract_get_score(src, tables),
    }
    try:  # optional: a build without the bombardment handler has no bomb list role (the client draws none)
        out["show_bombardment"] = extract_show_bombardment(src, tables)
    except ExtractError:
        pass
    return out


def _opt(name, fn, src, warnings):
    try:
        return fn(src)
    except ExtractError as e:
        warnings.append(str(e))
        return None


def x_matchmaker(src):
    m = re.search(prop("MATCHMAKER_URI") + r"\s*\|\|\s*(" + STR + ")", src)
    if not m:
        fail("matchmaker URL", "no `env.MATCHMAKER_URI || '<url>'`")
    v = js_unquote(m.group(1))
    if not re.fullmatch(r"https://[A-Za-z0-9.-]+(?::\d+)?", v):
        fail("matchmaker URL", "%r is not an https origin" % v)
    return v


def x_api(src):
    m = re.search(prop("API_URI") + r"\s*\|\|\s*(" + STR + ")", src)
    if not m:
        fail("api URL", "no `env.API_URI || '<url>'`")
    v = js_unquote(m.group(1))
    if not re.fullmatch(r"https://[A-Za-z0-9.-]+(?::\d+)?", v):
        fail("api URL", "%r is not an https origin" % v)
    return v


def x_turnstile(src):
    vals = sorted(set(js_unquote(x) for x in re.findall(r"""(?:'0x4AAAA[A-Za-z0-9_-]+'|"0x4AAAA[A-Za-z0-9_-]+")""", src)))
    if len(vals) != 1 or not re.fullmatch(r"0x4AAAA[A-Za-z0-9_-]{10,40}", vals[0]):
        fail("turnstile site key", "expected one '0x4AAAA...' literal, got %r" % vals)
    return vals[0]


def _str_array(lit):
    items = re.findall(STR, lit)
    if not items or re.sub(STR, "", lit[1:-1]).replace(",", "").strip():
        return None
    return [js_unquote(x) for x in items]


def x_regions(src):
    """`X = ['de-fra', ...], ... : X = ['local']` (the prod list vs the dev fallback)."""
    for m in re.finditer(r"""(?<![\w$.])(""" + IDENT + r""")\s*=\s*\[\s*(?:'local'|"local")\s*\]""", src):
        name = m.group(1)
        back = src[max(0, m.start() - 600):m.start()]
        dm = None
        for dm_ in re.finditer(r"(?<![\w$.])" + re.escape(name) + r"\s*=\s*(\[[^\]]*\])", back):
            dm = dm_
        if dm:
            arr = _str_array(dm.group(1))
            if arr and all(re.fullmatch(r"[a-z]{2,4}(?:-[a-z]+)*", r) for r in arr):
                extra = re.findall(re.escape(name) + prop("push") + r"\s*\(\s*(" + STR + r")\s*\)",
                                   back[dm.end():])
                return {"regions": arr, "conditional": [js_unquote(x) for x in extra]}
    fail("regions", "no `X = [regions] ... : X = ['local']`")


def x_dedicated_regions(src):
    m = re.search(key("dedicatedOnlyRegions") + r"\s*(" + IDENT + r"|\[[^\]]*\])", src)
    if not m:
        fail("dedicatedOnlyRegions", "key not found")
    lit = m.group(1)
    if not lit.startswith("["):
        dm = None
        for dm_ in re.finditer(r"(?<![\w$.])" + re.escape(lit) + r"\s*=\s*(\[[^\]]*\])", src[:m.start()]):
            dm = dm_
        if not dm:
            fail("dedicatedOnlyRegions", "%s does not resolve to an array" % lit)
        lit = dm.group(1)
    arr = _str_array(lit)
    if not arr:
        fail("dedicatedOnlyRegions", "not a string array")
    return arr


def x_client_revision(src):
    m = re.search(key("clientRevision") + r"\s*(" + STR + r")", src)
    if not m:
        fail("clientRevision", "no `'clientRevision': '<n>'`")
    return js_unquote(m.group(1))


def x_custom_build_version(src):
    m = re.search(key("customBuildVersion") + r"\s*", src)
    if not m:
        fail("customBuildVersion", "key not found")
    v = eval_concat(src, m.end())
    if v is not None:
        return {"value": v}
    im = re.match(IDENT, src[m.end():])
    if im:
        dm = re.search(r"(?<![\w$.])" + re.escape(im.group(0)) + r"\s*=\s*process\s*" + prop("env") +
                       r"\s*(\[\s*" + STR + r"\s*\]|\.\s*" + IDENT + ")", src)
        if dm:
            env = re.sub(r"""[\[\]\s'".]""", "", dm.group(1))
            return {"value": None, "env": env}
    return {"value": None}


def extract(src):
    norm, deob = deobfuscate(src)
    ahkey, ahform = extract_ahkey(norm)
    out = {
        "ahkey": ahkey,
        "ahkey_hex": "0x%x" % ahkey,
        "ahnum_step": ahkey & 0xFF,
        "ahkey_form": ahform,
        "ahnum_trailer": extract_trailer(norm),
        "client_version": extract_client_version(norm),
        "build_version": extract_build_version(norm),
    }
    tables = extract_tables(norm)
    check_table_idents(tables)
    # the AHKEY anchor is the `!= C2S['cptR']` guard, so the exempt packet is C2S cptR
    out["ahnum_exempt"] = {"name": "cptR", "wire": dict(tables["c2s"])["cptR"]}
    cheat = extract_cheat(norm)
    layouts = extract_layouts(norm, tables)
    warnings = []
    for side, ent in tables.items():
        seen = {}
        for k, v in ent:
            if v in seen:
                warnings.append("packet table %s: wire %r is both %s and %s (decode is ambiguous)" % (side, v, seen[v], k))
            seen.setdefault(v, k)
    extras = {
        "matchmaker_url": _opt("matchmaker", x_matchmaker, norm, warnings),
        "api_url": _opt("api", x_api, norm, warnings),
        "turnstile_site_key": _opt("turnstile", x_turnstile, norm, warnings),
        "regions": _opt("regions", x_regions, norm, warnings),
        "dedicated_only_regions": _opt("dedicated", x_dedicated_regions, norm, warnings),
        "client_revision": _opt("clientRevision", x_client_revision, norm, warnings),
        "custom_build_version": _opt("customBuildVersion", x_custom_build_version, norm, warnings),
    }
    maps = _opt("maps", maps_module().extract_maps, norm, warnings)
    out.update({
        "packets_c2s": [{"name": k, "wire": v} for k, v in tables["c2s"]],
        "packets_s2c": [{"name": k, "wire": v} for k, v in tables["s2c"]],
        "cheat": {
            "keystream_formula": cheat["formula"],
            "salt": cheat["salt"],
            "salt_bytes": [ord(c) for c in cheat["salt"]],
            "fnv_offset": "0x%08x" % cheat["fnv_offset"],
            "fnv_prime": "0x%x" % cheat["fnv_prime"],
            "fnv_multiply": cheat["fnv_multiply"],
            "handler": "v1",
            "cc_args": {"count": 3, "nonce": 0, "ciphertext": 2},
        },
        "layouts": layouts,
        "cheat_keystream_js": cheat["keystream_js"],
        "cheat_salt_js": cheat["salt_js"],
        "cheat_keystream_standalone_js": cheat["standalone_js"],
        "cheat_handler_js": cheat["handler_js"],
        "extras": extras,
        "maps": maps,
        "warnings": warnings,
        "deobfuscation": deob,
    })
    return out


# ----------------------------------------------------------------------- source loading

def cache_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return os.path.join(base, "krunknative", "krunker-build")


def _atomic_write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        umask = os.umask(0)
        os.umask(umask)
        os.chmod(tmp, 0o666 & ~umask)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def fetch(url):
    """(bytes, origin) with a conditional GET against the cache. origin: 'url' | 'cache'.
    Raises OSError when neither the URL nor a cached copy is usable."""
    cdir = cache_dir()
    key_ = hashlib.sha256(url.encode()).hexdigest()[:16]
    body_path = os.path.join(cdir, key_ + ".js")
    meta_path = os.path.join(cdir, key_ + ".json")
    meta = {}
    if os.path.exists(meta_path) and os.path.exists(body_path):
        try:
            with open(meta_path, encoding="utf8") as f:
                meta = json.load(f)
        except (OSError, ValueError):
            meta = {}
    headers = {"User-Agent": os.environ.get("KN_FETCH_UA", DEFAULT_UA), "Accept": "*/*"}
    if meta.get("etag"):
        headers["If-None-Match"] = meta["etag"]
    if meta.get("last_modified"):
        headers["If-Modified-Since"] = meta["last_modified"]
    proxy = os.environ.get("KN_FETCH_PROXY", "")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {}))
    try:
        with opener.open(urllib.request.Request(url, headers=headers), timeout=60) as r:
            data = r.read()
            et, lm = r.headers.get("ETag"), r.headers.get("Last-Modified")
        sha = hashlib.sha256(data).hexdigest()
        _atomic_write(body_path, data)
        _atomic_write(meta_path, json.dumps({"url": url, "etag": et, "last_modified": lm, "sha256": sha,
                                             "bytes": len(data)}, indent=1).encode())
        return data, "url"
    except urllib.error.HTTPError as e:
        if e.code == 304 and meta:
            with open(body_path, "rb") as f:
                data = f.read()
            if hashlib.sha256(data).hexdigest() == meta.get("sha256"):
                return data, "url (304, cached)"
            raise OSError("%s: 304 but the cached copy is corrupt" % url)
        err = "HTTP %d" % e.code
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        err = str(getattr(e, "reason", e))
    if meta:
        with open(body_path, "rb") as f:
            data = f.read()
        if hashlib.sha256(data).hexdigest() == meta.get("sha256"):
            log("%s unreachable (%s); using the cached copy from the last fetch" % (url, err))
            return data, "cache"
    raise OSError("%s: %s" % (url, err))


def load_source(arg):
    """-> (text, provenance dict)."""
    if arg is None:
        try:
            data, origin = fetch(DEFAULT_URL)
            prov = {"url": DEFAULT_URL, "origin": origin}
        except OSError as e:
            if not os.path.exists(DEFAULT_LOCAL):
                raise ExtractError("source: %s, and no local fallback %s" % (e, DEFAULT_LOCAL))
            log("%s; falling back to %s" % (e, DEFAULT_LOCAL))
            with open(DEFAULT_LOCAL, "rb") as f:
                data = f.read()
            prov = {"url": None, "origin": "local", "file": os.path.basename(DEFAULT_LOCAL)}
    elif re.match(r"https?://", arg):
        try:
            data, origin = fetch(arg)
        except OSError as e:
            raise ExtractError("source: %s" % e)
        prov = {"url": arg, "origin": origin}
    else:
        with open(arg, "rb") as f:
            data = f.read()
        prov = {"url": None, "origin": "local", "file": os.path.basename(arg)}
    prov["sha256"] = hashlib.sha256(data).hexdigest()
    prov["bytes"] = len(data)
    text = data.decode("utf-8")
    if text.lstrip().startswith("{"):
        try:
            man = json.loads(text)
        except ValueError:
            man = None
        if isinstance(man, dict) and isinstance(man.get("source"), str):
            text = man["source"]
            prov["manifest"] = True
    return text, prov


# ------------------------------------------------------------------------------ output

def c_str(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= o < 0x7F and ch != "?":
            out.append(ch)
        elif ch == "?":
            out.append("\\?")  # no trigraphs
        else:
            for b in ch.encode("utf-8", "surrogatepass"):
                out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def c_comment(s):
    return s.replace("*/", "* /")


def render_header(b):
    src = b["source"]
    L = []
    w = L.append
    w("/* GENERATED by tools/krunker/extract_build.py (generator rev %d) -- do not edit." % b["generator_rev"])
    w(" * Regenerate: make krunker-build      Check: make krunker-build-check")
    w(" *")
    w(" * Build constants of the official Krunker client bundle the krunker_proto codec speaks to.")
    w(" * Source: %s" % c_comment(src.get("url") or src.get("file") or "?"))
    w(" * sha256: %s (%d bytes)" % (src["sha256"], src["bytes"]))
    w(" */")
    w("#ifndef KRUNKER_BUILD_H")
    w("#define KRUNKER_BUILD_H")
    w("")
    w("/* Bumped when this header's C interface changes; consumers: #if KRUNKER_BUILD_SCHEMA != N #error */")
    w("#define KRUNKER_BUILD_SCHEMA %d" % SCHEMA)
    w("#define KRUNKER_BUILD_SOURCE_URL %s" % c_str(src.get("url") or ""))
    w("#define KRUNKER_BUILD_SOURCE_SHA256 %s" % c_str(src["sha256"]))
    w("")
    w("/* Every frame is msgpack([name, ...args]) + 2 trailer bytes {ahNum >> 4 & 0xf, ahNum & 0xf}.")
    w(" * Before every send except packet cptR: ahNum = (ahNum + KRUNKER_AHKEY) & 0xff, ahNum")
    w(" * starting at 0, so only KRUNKER_AHNUM_STEP (the low byte) matters. */")
    w("#define KRUNKER_AHKEY %su" % b["ahkey_hex"])
    w("#define KRUNKER_AHNUM_STEP 0x%02xu" % b["ahnum_step"])
    w("/* The packet sent without the step (the Turnstile captcha reply), as name / wire / enum. */")
    w("#define KRUNKER_AHNUM_EXEMPT_NAME %s" % c_str(b["ahnum_exempt"]["name"]))
    w("#define KRUNKER_AHNUM_EXEMPT_WIRE %s" % c_str(b["ahnum_exempt"]["wire"]))
    w("#define KRUNKER_AHNUM_EXEMPT KRUNKER_C2S_%s" % c_ident(b["ahnum_exempt"]["name"]))
    w("")
    w("/* Echoed to the matchmaker (seek-game) and the game server. buildVersion is also the")
    w(" * dataQuery's \"v\". */")
    w("#define KRUNKER_CLIENT_VERSION %s" % c_str(b["client_version"]))
    w("#define KRUNKER_BUILD_VERSION %s" % c_str(b["build_version"]))
    ex = b["extras"]
    if ex.get("client_revision") is not None:
        w("#define KRUNKER_CLIENT_REVISION %s" % c_str(ex["client_revision"]))
    if ex.get("matchmaker_url"):
        w("#define KRUNKER_MATCHMAKER_URL %s" % c_str(ex["matchmaker_url"]))
    if ex.get("api_url"):
        w("#define KRUNKER_API_URL %s" % c_str(ex["api_url"]))
    if ex.get("turnstile_site_key"):
        w("#define KRUNKER_TURNSTILE_SITE_KEY %s" % c_str(ex["turnstile_site_key"]))
    if ex.get("regions"):
        w("/* Matchmaker region ids (prod list; the dev build uses \"local\"). */")
        w("#define KRUNKER_REGIONS %s" % ", ".join(c_str(r) for r in ex["regions"]["regions"]))
        w("#define KRUNKER_REGION_COUNT %d" % len(ex["regions"]["regions"]))
    if ex.get("dedicated_only_regions"):
        w("#define KRUNKER_DEDICATED_ONLY_REGIONS %s" % ", ".join(c_str(r) for r in ex["dedicated_only_regions"]))
    w("")
    ch = b["cheat"]
    w("/* CHEAT_CHECK (S->C \"cc\" [nonce, unused, ciphertext]): ciphertext is base64; plaintext =")
    w(" * JSON.parse(utf8(cipher ^ ks(nonce))), a JS source string that is eval'd to R.")
    w(" * Keystream (formula %s): ks[i] = ((nonce.charCodeAt(i %% nonce.length) ^ salt[i %% saltlen]) + i) & 0xff" % ch["keystream_formula"])
    w(" * (charCodeAt: UTF-16 code units of the nonce string).")
    w(" * Reply CHEAT_CHECK_RESPONSE: [nonce, base64(utf8(JSON.stringify(R)) ^ ks(nonce)), hash], hash =")
    w(" * 8 lowercase hex of FNV-1a 32 over the UTF-16 code units of String(nonce) + JSON.stringify(R).")
    if ch["fnv_multiply"] == "double":
        w(" * NOT plain FNV-1a: the step is `h = PRIME * (h ^= c) | 0` in doubles, and PRIME * h (|h| < 2^31)")
        w(" * exceeds 2^53, so the product is rounded to 53 bits before ToInt32. Reproduce it as")
        w(" * h = (int32_t)(uint32_t)(int64_t)((double)KRUNKER_CHEAT_FNV_PRIME * (double)(int32_t)(h ^ c)). */")
    else:
        w(" * The step uses Math.imul (exact 32-bit FNV-1a). */")
    w("#define KRUNKER_CHEAT_KS_FORMULA 1")
    w("#define KRUNKER_CHEAT_SALT %s" % c_str(ch["salt"]))
    w("#define KRUNKER_CHEAT_SALT_LEN %d" % len(ch["salt"]))
    w("#define KRUNKER_CHEAT_FNV_OFFSET %su" % ch["fnv_offset"])
    w("#define KRUNKER_CHEAT_FNV_PRIME %su" % ch["fnv_prime"])
    w("#define KRUNKER_CHEAT_FNV_DOUBLE_MUL %d" % (1 if ch["fnv_multiply"] == "double" else 0))
    w("")
    render_layouts(w, b["layouts"])
    for side, title, pre in (("packets_c2s", "client -> server", "KRUNKER_C2S"),
                             ("packets_s2c", "server -> client", "KRUNKER_S2C")):
        ents = b[side]
        w("/* Packet names, %s (%d). X(ident, \"wire\", \"KrunkerName\") */" % (title, len(ents)))
        w("#define %s_PACKETS(X_) \\" % pre)
        for i, e in enumerate(ents):
            w("    X_(%s, %s, %s)%s" % (c_ident(e["name"]), c_str(e["wire"]), c_str(e["name"]),
                                    " \\" if i + 1 < len(ents) else ""))
        w("")
        w("typedef enum {")
        for e in ents:
            w("    %s_%s," % (pre, c_ident(e["name"])))
        w("    %s_COUNT" % pre)
        w("} %s;" % ("KrunkerC2S" if pre.endswith("C2S") else "KrunkerS2C"))
        w("")
    w("#endif /* KRUNKER_BUILD_H */")
    return "\n".join(L) + "\n"


LAYOUT_DOCS = (
    ("add_players", "ADD", "KrunkerAddField",
     "ADD_PLAYERS (\"0\" [list, ...]): a flat list, KRUNKER_ADD_STRIDE values per player. The offsets the\n"
     " * client reads (its handler and players.add), named by the Krunker property each lands in; [id]\n"
     " * is the socket id (\"you\" when it equals io-init's), health null means maxHealth."),
    ("update_players", "K", "KrunkerKField",
     "UPDATE_PLAYERS (\"k\" [list, sendRate, serverTime]): KRUNKER_K_STRIDE values per player; xDire /\n"
     " * yDire in degrees, weapon a loadout index, grapple [x, y, z] or null."),
    ("sync_player", "SYNC", "KrunkerSyncField",
     "SYNC_PLAYER (\"l\" [state]): your authoritative state, answered with PONG. velocity.set(velX, velY,\n"
     " * velZ); slideTimer / canSlideT in ms; _protected a flat [key, value, ...] list. From\n"
     " * KRUNKER_SYNC_SEND_VALS on: the movement schema's sendVals, then the controlled-entity sync."),
)
ARG_DOCS = (
    ("kill_player", "KILL", "KILL_PLAYER (\"3\"): info is {wId, fs, hs, ...} or a weapon index"),
    ("do_damage", "DAMAGE", "DO_DAMAGE (\"4\"): a hit you dealt; ai: the target is an AI sid"),
    ("change_health", "HEALTH", "CHANGE_HEALTH (\"h\"): sid null means you; srcX / srcZ the hit's source"),
    ("get_score", "SCORE", "GET_SCORE (\"5\"): relay = a spectated player's score, not yours"),
    ("show_bombardment", "BOMB", "SHOW_BOMBARDMENT (\"bmb\"): bombs, a flat list of 5 values a bomb (x, y, z, sid, pjSp)"),
)


def _c_names(prefix, names, anchor):
    out = {}
    for n in names:
        ci = "KRUNKER_%s_%s" % (prefix, _snake(n))
        if ci in out.values() or _snake(n) in ("STRIDE", "FIELDS", "SEND_VALS", "ARGC"):
            fail(anchor, "field %r gives the C name %s twice / a reserved name" % (n, ci))
        out[n] = ci
    return out


def render_layouts(w, lay):
    for key_, prefix, enum, doc in LAYOUT_DOCS:
        L_ = lay[key_]
        fields = L_["fields"]
        names = _c_names(prefix, [f["name"] for f in fields], key_)
        w("/* %s" % doc)
        w(" * X(C_SUFFIX, offset, \"krunkerName\") */")
        if "stride" in L_:
            w("#define KRUNKER_%s_STRIDE %d" % (prefix, L_["stride"]))
        if "send_vals" in L_:
            w("#define KRUNKER_%s_SEND_VALS %d" % (prefix, L_["send_vals"]))
        w("#define KRUNKER_%s_FIELDS(X_) \\" % prefix)
        for i, f in enumerate(fields):
            w("    X_(%s, %d, %s)%s" % (names[f["name"]][len("KRUNKER_%s_" % prefix):], f["offset"], c_str(f["name"]),
                                    " \\" if i + 1 < len(fields) else ""))
        w("")
        w("typedef enum {")
        for f in fields:
            w("    %s = %d," % (names[f["name"]], f["offset"]))
        w("} %s;" % enum)
        w("")
    w("/* Argument positions of the per-event packets (the handler's parameters, by role). */")
    for key_, prefix, doc in ARG_DOCS:
        if key_ not in lay:  # an optional packet whose layout did not extract
            continue
        A_ = lay[key_]
        names = _c_names(prefix + "_ARG", list(A_["args"]), key_)
        w("/* %s */" % doc)
        w("#define KRUNKER_%s_ARGC %d" % (prefix, A_["argc"]))
        for role, i in A_["args"].items():
            w("#define %s %d" % (names[role], i))
        w("")


def render_json(b):
    return json.dumps(b, indent=1, ensure_ascii=False, sort_keys=False) + "\n"


def build_doc(values, prov):
    src = {"url": prov.get("url"), "sha256": prov["sha256"], "bytes": prov["bytes"]}
    if prov.get("file"):
        src["file"] = prov["file"]
    if prov.get("manifest"):
        src["manifest"] = True
    doc = {"generator_rev": GENERATOR_REV, "schema": SCHEMA, "source": src}
    doc.update(values)
    return doc


def summary(doc):
    ex = doc["extras"]
    lay = doc.get("layouts") or {}
    return ("AHKEY %s (step 0x%02x)  clientVersion %s  buildVersion %s  c2s %d  s2c %d  salt %r  fnv %s\n"
            "  layouts: \"0\" %s/player  \"k\" %s/player  \"l\" sendVals at %s\n"
            "  matchmaker %s  turnstile %s  regions %s" % (
                doc["ahkey_hex"], doc["ahnum_step"], doc["client_version"], doc["build_version"],
                len(doc["packets_c2s"]), len(doc["packets_s2c"]), doc["cheat"]["salt"], doc["cheat"]["fnv_multiply"],
                (lay.get("add_players") or {}).get("stride"), (lay.get("update_players") or {}).get("stride"),
                (lay.get("sync_player") or {}).get("send_vals"),
                ex.get("matchmaker_url"), ex.get("turnstile_site_key"),
                ",".join(ex["regions"]["regions"]) if ex.get("regions") else None))


def write_maps_header(doc, path):
    """shared/include/krunker_maps.h from build.json's "maps" + tools/krunker/maps.lock.json (written only
    when it changes); 1 when the bundle's map anchors and the fetched list disagree."""
    mm = maps_module()
    lock = mm.read_lock()
    if not doc.get("maps") or not lock or not lock.get("maps"):
        log("FAILED: maps: %s" % ("the bundle's map API anchors did not extract" if not doc.get("maps") else
                                  "tools/krunker/maps.lock.json missing; run tools/krunker/fetch_maps.py fetch"))
        return 1
    rc = 0
    for p in mm.consistency(doc["maps"], lock) + mm.verify_files(lock):
        log("maps: " + p)
        rc = 1
    text = mm.render_header(doc["maps"], lock)
    if read_text(path) != text:
        _atomic_write(path, text.encode())
        log("wrote %s (%d official maps)" % (os.path.relpath(path, ROOT), len(lock["maps"])))
    return rc


def read_text(path):
    try:
        with open(path, encoding="utf8") as f:
            return f.read()
    except OSError:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("source", nargs="?", help="path or URL (default %s, fallback %s)" % (DEFAULT_URL, DEFAULT_LOCAL))
    ap.add_argument("--print", action="store_true", help="print the extracted JSON, write nothing")
    ap.add_argument("--check", action="store_true", help="exit 1 when the committed header / build.json are stale")
    ap.add_argument("--strict", action="store_true", help="with --check: an unreachable source is an error")
    ap.add_argument("--from-json", action="store_true", help="re-render the header from the committed build.json")
    ap.add_argument("--force", action="store_true", help="regenerate even when the source sha256 is unchanged")
    ap.add_argument("--header", default=OUT_HEADER)
    ap.add_argument("--json", default=OUT_JSON)
    ap.add_argument("--maps-header", help="default: shared/include/krunker_maps.h, or krunker_maps.h beside --header")
    a = ap.parse_args(argv)
    maps_header = a.maps_header or (maps_module().OUT_HEADER if os.path.abspath(a.header) == os.path.abspath(OUT_HEADER)
                                    else os.path.join(os.path.dirname(os.path.abspath(a.header)), "krunker_maps.h"))

    old_json = read_text(a.json)
    old = json.loads(old_json) if old_json else None

    if a.from_json:
        if not old:
            log("%s missing" % a.json)
            return 1
        _atomic_write(a.header, render_header(old).encode())
        log("wrote %s" % a.header)
        return write_maps_header(old, maps_header)

    if a.check:
        rc = 0
        if not old:
            log("STALE: %s missing; run `make krunker-build`" % os.path.relpath(a.json, ROOT))
            return 1
        if read_text(a.header) != render_header(old):
            log("STALE: %s does not match %s; run `make krunker-build`" % (
                os.path.relpath(a.header, ROOT), os.path.relpath(a.json, ROOT)))
            rc = 1
        mm = maps_module()
        try:
            text, prov = load_source(a.source)
        except (ExtractError, OSError) as e:
            log("cannot read the bundle (%s): checked header <-> build.json only" % e)
            for p in mm.problems(old.get("maps"), mm.read_lock(), maps_header):
                log("STALE: maps: " + p)
                rc = 1
            return 2 if a.strict else rc
        try:
            doc = build_doc(extract(text), prov)
        except ExtractError as e:
            log("the current bundle does not extract: %s" % e)
            return 1
        # the map table is checked against the bundle that is there now (its anchors, the lock's files and
        # the header), whether or not the build constants changed
        for p in mm.problems(doc.get("maps"), mm.read_lock(), maps_header):
            log("STALE: maps: " + p)
            rc = 1
        if prov["sha256"] != old["source"]["sha256"]:
            log("STALE: the bundle changed (sha256 %s -> %s); run `make krunker-build`\n  committed: %s\n  current:   %s"
                % (old["source"]["sha256"][:16], prov["sha256"][:16], summary(old), summary(doc)))
            return 1
        # same bundle: regenerate into a temp file (with the committed provenance, so a local copy of
        # the bundle compares equal to the URL it was generated from) and diff it with the header
        doc["source"] = old["source"]
        fd, tmp = tempfile.mkstemp(prefix="krunker_build-", suffix=".h")
        try:
            with os.fdopen(fd, "w", encoding="utf8") as f:
                f.write(render_header(doc))
            fresh = read_text(tmp)
        finally:
            os.unlink(tmp)
        cur = read_text(a.header)
        if fresh != cur:
            diff = list(difflib.unified_diff((cur or "").splitlines(), fresh.splitlines(), "committed", "regenerated",
                                             n=0, lineterm=""))
            log("STALE: regenerating %s from the same bundle differs (generator rev %s -> %d); run "
                "`make krunker-build FORCE=1`\n  %s" % (os.path.relpath(a.header, ROOT), old.get("generator_rev"),
                                                        GENERATOR_REV, "\n  ".join(diff[:40])))
            return 1
        if json.loads(render_json(doc)) != old:
            log("STALE: %s differs from a regeneration from the same bundle; run `make krunker-build FORCE=1`" %
                os.path.relpath(a.json, ROOT))
            return 1
        if rc == 0:
            log("maps: %d official maps (%s), every bundle anchor agrees, files match maps.lock.json" % (
                len(mm.read_lock()["maps"]), os.path.relpath(maps_header, ROOT)))
            log("up to date (bundle sha256 %s, regenerated header identical)" % prov["sha256"][:16])
        return rc

    try:
        text, prov = load_source(a.source)
    except ExtractError as e:
        log("FAILED: %s" % e)
        return 3  # the bundle could not be read at all: not a change of its shape (krunker-build.yml)
    try:
        if (not a.print and not a.force and old and old["source"]["sha256"] == prov["sha256"]
                and old.get("generator_rev") == GENERATOR_REV and read_text(a.header) == render_header(old)):
            log("up to date (bundle sha256 %s, %s)" % (prov["sha256"][:16], prov["origin"]))
            return write_maps_header(old, maps_header)
        doc = build_doc(extract(text), prov)
    except ExtractError as e:
        log("FAILED: %s" % e)
        return 1
    for w_ in doc["warnings"]:
        log("warning: " + w_)
    if not doc.get("maps") and not a.print:
        log("FAILED: maps: the bundle's map API anchors did not extract (see the warning above)")
        return 1
    if a.print:
        sys.stdout.write(render_json(doc))
        return 0
    _atomic_write(a.json, render_json(doc).encode())
    _atomic_write(a.header, render_header(doc).encode())
    log("wrote %s and %s from %s (%s)\n  %s" % (os.path.relpath(a.header, ROOT), os.path.relpath(a.json, ROOT),
                                                prov.get("url") or prov.get("file"), prov["origin"], summary(doc)))
    return write_maps_header(doc, maps_header)


if __name__ == "__main__":
    sys.exit(main())
