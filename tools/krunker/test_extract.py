#!/usr/bin/env python3
"""Regression test of tools/krunker/extract_build.py.

1. Cross-build check (the main one): every Krunker bundle found locally must extract, with the
   right AHKEY / versions / salt for its build, and the one-line obfuscated variants
   (game.debug.js, game.min.js = game.manifest.json's "source", the manifest itself) must give
   exactly the values of the readable game.deob.js. Bundles that are not on this machine (CI)
   are skipped; KRUNKER_BUNDLES_REQUIRED=1 makes a missing one a failure.
2. Synthetic bundles (always run, no Krunker files): a readable fixture, the same fixture
   string-array obfuscated into one line, and one fixture per anchor with that anchor broken,
   which must fail naming the anchor and write nothing.
3. The committed shared/include/krunker_build.h matches tools/krunker/build.json and compiles
   as C11 with -Wall -Wextra -Werror (when a C compiler is installed).

usage: python3 -I tools/krunker/test_extract.py [-v]
"""

import hashlib
import io
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract_build as E  # noqa: E402

ROOT = E.ROOT
HOME = os.path.expanduser("~")
OFFICIAL = os.path.join(HOME, ".cache", "kn-official")
V10 = os.path.join(HOME, ".cache", "kn-v10", "v10.js")

# The live build (2026-10-14, sha256 e21ede6a... of game.deob.js) and the older v10.js copy.
LIVE = {"ahkey": 0x17153, "client_version": "10.0.2r1", "build_version": "Def6phpLKsJTIw2XpAeEicZWURGypSce",
        "salt": "KRUNK3R_AC", "c2s": 138, "s2c": 262}
OLD_V10 = {"ahkey": 1597, "client_version": "10.0.1r1", "build_version": "FArgZqd2pNs7O8lWk4TblHSdDWfMUfD2",
           "salt": "KRUNK3R_AC", "c2s": 138, "s2c": 262}

VERBOSE = "-v" in sys.argv
failures = []
passed = [0]


def check(cond, what):
    if cond:
        passed[0] += 1
        if VERBOSE:
            print("  ok   " + what)
    else:
        failures.append(what)
        print("  FAIL " + what)


def run_extract(text):
    try:
        return E.extract(text), None
    except E.ExtractError as e:
        return None, str(e)


def check_values(label, doc, exp):
    check(doc["ahkey"] == exp["ahkey"], "%s: AHKEY %s == %s" % (label, doc["ahkey_hex"], hex(exp["ahkey"])))
    check(doc["ahnum_step"] == exp["ahkey"] & 0xFF, "%s: AHNUM step" % label)
    check(doc["client_version"] == exp["client_version"], "%s: clientVersion %s" % (label, doc["client_version"]))
    check(doc["build_version"] == exp["build_version"], "%s: buildVersion %s" % (label, doc["build_version"]))
    check(doc["cheat"]["salt"] == exp["salt"], "%s: cheat salt %r" % (label, doc["cheat"]["salt"]))
    check(len(doc["packets_c2s"]) == exp["c2s"], "%s: %d c2s packets" % (label, len(doc["packets_c2s"])))
    check(len(doc["packets_s2c"]) == exp["s2c"], "%s: %d s2c packets" % (label, len(doc["packets_s2c"])))
    c2s = {e["name"]: e["wire"] for e in doc["packets_c2s"]}
    s2c = {e["name"]: e["wire"] for e in doc["packets_s2c"]}
    if "en" in exp.get("anchors", ("en",)):
        check(c2s.get("ENTER_GAME") == "en" and c2s.get("CHEAT_CHECK_RESPONSE") == "ccr" and c2s.get("cptR") == "cptR",
              "%s: c2s anchors" % label)
        check(s2c.get("IO_INIT") == "io-init" and s2c.get("CHEAT_CHECK") == "cc", "%s: s2c anchors" % label)
    check(doc["cheat"]["fnv_multiply"] == "double", "%s: FNV step in doubles" % label)


def comparable(doc):
    """The values that must not depend on the bundle's form (deob vs debug vs min)."""
    d = dict(doc)
    for k in ("deobfuscation", "cheat_keystream_js", "cheat_salt_js", "cheat_keystream_standalone_js",
              "cheat_handler_js"):
        d.pop(k, None)
    return d


# ------------------------------------------------------------------ 1. real bundles

def test_real_bundles(tmp):
    required = os.environ.get("KRUNKER_BUNDLES_REQUIRED") == "1"
    deob_path = os.path.join(OFFICIAL, "game.deob.js")
    variants = [("game.deob.js", deob_path), ("game.debug.js", os.path.join(OFFICIAL, "game.debug.js")),
                ("game.manifest.json", os.path.join(OFFICIAL, "game.manifest.json"))]
    man = variants[2][1]
    if os.path.exists(man):
        # game.min.js as served = the manifest's "source" (game.debug.js minus its 106-byte prelude)
        with open(man, encoding="utf8") as f:
            src = json.load(f)["source"]
        minp = os.path.join(tmp, "game.min.js")
        with open(minp, "w", encoding="utf8") as f:
            f.write(src)
        variants.append(("game.min.js", minp))
    ref = None
    for label, path in variants:
        if not os.path.exists(path):
            print("  SKIP %s (not on this machine)" % label)
            check(not required, "%s present" % label)
            continue
        text, prov = E.load_source(path)
        doc, err = run_extract(text)
        check(err is None, "%s extracts (%s)" % (label, err))
        if not doc:
            continue
        check_values(label, doc, LIVE)
        if label == "game.deob.js":
            ref = doc
            check(doc["deobfuscation"] is None, "game.deob.js needs no string-array decoding")
        else:
            check(doc["deobfuscation"] is not None, "%s was string-array decoded" % label)
            if ref:
                same = comparable(doc) == comparable(ref)
                check(same, "%s: every value identical to game.deob.js" % label)
                check(E.canonical_js(doc["cheat_keystream_js"], {}) == E.canonical_js(ref["cheat_keystream_js"], {}),
                      "%s: same keystream function" % label)
        print("  %-19s %s" % (label, E.summary(E.build_doc(doc, prov)).split("\n")[0]))
    if os.path.exists(V10):
        text, prov = E.load_source(V10)
        doc, err = run_extract(text)
        check(err is None, "v10.js extracts (%s)" % err)
        if doc:
            check_values("v10.js", doc, OLD_V10)
            print("  %-19s %s" % ("v10.js", E.summary(E.build_doc(doc, prov)).split("\n")[0]))
    else:
        print("  SKIP v10.js (not on this machine)")
        check(not required, "v10.js present")


# --------------------------------------------------------------- 2. synthetic bundles

def fixture_tables():
    c2s = [("PING", "po"), ("ENTER_GAME", "en"), ("INPUT", "q"), ("CHAT_MESSAGE", "ct"), ("cptR", "cptR"),
           ("CHEAT_CHECK_RESPONSE", "ccr")] + [("C2S_X%d" % i, "x%d" % i) for i in range(20)]
    s2c = [("RPC_SUCCESS", "_0"), ("cpt", "cpt"), ("cptR", "cptR"), ("IO_INIT", "io-init"), ("ADD_PLAYERS", "0"),
           ("CHEAT_CHECK", "cc"), ("ENTER_GAME", "frs"), ("ERROR", "error")] + [("S2C_Y%d" % i, "y%d" % i) for i in range(20)]
    return c2s, s2c


def table_js(var, entries):
    return "  var %s = {\n%s\n  };\n" % (var, ",\n".join("    '%s': '%s'" % kv for kv in entries))


# A readable bundle with the same structures as Krunker's (written for this test, not copied):
# the identifiers are arbitrary on purpose; only strings and numbers are anchors.
FIXTURE = r"""!function () {
  'use strict';
  function qq1(a1, a2) {
    return a1 + a2 & 0xff;
  }
  function qq2(a3) {
    return [a3 >> 0x4 & 0xf, 0xf & a3];
  }
  var vv1 = '9.8.7';
  var vv2 = vv1 + 'r3';
  var vv3 = process["env"]['CUSTOM_VERSION'];
  zz1 = process['env']["MATCHMAKER_URI"] || "https://mm.example.test", zz2 = process["env"]["API_URI"] || "https://api.example.test";
  var vv4 = zz9 ? "0x4AAAAAAAtestSiteKey000" : "1x00000000000000000000BB";
  zz9 ? (vv5 = ['de-fra', "us-nj"], zz8 || vv5['push']("sss")) : vv5 = ["local"];
  var vv6 = ['us-wa'];
  var cfg = {
    'buildVersion': "AbCdEfGhIjKlMnOpQrStUvWxYz012345",
    'clientRevision': '7',
    'clientVersion': vv2,
    'customBuildVersion': vv3,
    'dedicatedOnlyRegions': vv6,
    'turnstileSiteKey': vv4
  };
@@C2S@@@@S2C@@  var sock = {
    'ahNum': 0x0,
    'send': function (p1) {
      if (this["socket"] && this["socket"]["readyState"] === WebSocket['OPEN']) {
        p1 != tc2s['cptR'] && (this["ahNum"] = qq1(this["ahNum"], @@AHKEY@@));
        var p2 = qq9(p1, this["ahNum"]);
        this["socket"]["send"](p2);
      }
    }
  };
  var salt1 = [@@SALT@@];
  function ks1(n1, n2) {
    for (var k1 = salt1, k2 = new Uint8Array(n2), k3 = 0x0; k3 < n2; k3++) {
      k2[k3] = (n1['charCodeAt'](k3 % n1['length']) ^ k1[k3 % k1["length"]]) + k3 & 0xff;
    }
    return k2;
  }
  function cc1(h1, h2, h3) {
    try {
      var h4;
      var h5 = function (b1, b2) {
        var b3;
        b3 = "undefined" != typeof Buffer ? new Uint8Array(Buffer['from'](b1, "base64")) : new Uint8Array(atob(b1)["split"]('')["map"](function (b4) {
          return b4["charCodeAt"](0x0);
        }));
        for (var b5 = ks1(b2, b3["length"]), b6 = new Uint8Array(b3['length']), b7 = 0x0; b7 < b3["length"]; b7++) {
          b6[b7] = b3[b7] ^ b5[b7];
        }
        var b8 = new TextDecoder()["decode"](b6);
        return JSON["parse"](b8);
      }(h3, h1);
      try {
        h4 = (0x0, eval)(h5);
      } catch (e1) {
        h4 = {
          'r': null,
          'e': {
            '_eval': e1["message"] || "eval_error"
          }
        };
      }
      var h6 = function (c1, c2) {
        for (var c3 = JSON["stringify"](c1), c4 = new TextEncoder()["encode"](c3), c5 = ks1(c2, c4["length"]), c6 = new Uint8Array(c4["length"]), c7 = 0x0; c7 < c4["length"]; c7++) {
          c6[c7] = c4[c7] ^ c5[c7];
        }
        return "undefined" != typeof Buffer ? Buffer["from"](c6)["toString"]("base64") : btoa(String["fromCharCode"]["apply"](String, qq8(c6)));
      }(h4, h1);
      var h7 = function (d1, d2) {
        for (var d3 = d2 + JSON["stringify"](d1), d4 = 0x811c9dc5, d5 = 0x0; d5 < d3['length']; d5++) {
          d4 = 0x1000193 * (d4 ^= d3["charCodeAt"](d5)) | 0x0;
        }
        return (d4 >>> 0x0)["toString"](0x10)['padStart'](0x8, '0');
      }(h4, h1);
      sock["send"](tc2s["CHEAT_CHECK_RESPONSE"], h1, h6, h7);
    } catch (e2) {}
  }
}();
"""

FIX_EXP = {"ahkey": 0x2A5F1, "client_version": "9.8.7r3", "build_version": "AbCdEfGhIjKlMnOpQrStUvWxYz012345",
           "salt": "S4LT-x", "c2s": 26, "s2c": 28}


def make_fixture(ahkey="0x2a5f1", salt="S4LT-x"):
    c2s, s2c = fixture_tables()
    return (FIXTURE.replace("@@C2S@@", table_js("tc2s", c2s)).replace("@@S2C@@", table_js("ts2c", s2c))
            .replace("@@AHKEY@@", ahkey).replace("@@SALT@@", ", ".join("0x%x" % ord(c) for c in salt)))


def obfuscate(src, seed=7):
    """One-line string-array form of `src` like Krunker's game.min.js: the string literals that
    are property names / table values move into a rotated array behind a decoder."""
    rnd = random.Random(seed)
    lit = re.compile(E.STR)
    strings = []
    index = {}

    def take(m):
        s = E.js_unquote(m.group(0))
        # keep object keys literal (Krunker's obfuscator does too); everything else goes in the array
        after = src[m.end():m.end() + 3].lstrip()
        if after.startswith(":") or rnd.random() < 0.15:
            return m.group(0)
        if s not in index:
            index[s] = len(strings)
            strings.append(s)
        return "@@D%d@@" % index[s]
    body = lit.sub(take, src)
    offset = 0x1ac
    # checksum strings that parseInt to known numbers
    b, c, d = 2 * rnd.randint(10, 999), 3 * rnd.randint(10, 99), 4 * rnd.randint(10, 9999)
    a = (b // 2) * (c // 3) + rnd.randint(1, 99999)  # keeps the checksum a positive integer
    nums = ["%d%s" % (v, rnd.choice(["abc", "XyZ", "q"])) for v in (a, b, c, d)]
    for n in nums:
        index[n] = len(strings)
        strings.append(n)
    final = list(strings)
    k = rnd.randint(1, len(final) - 1)
    stored = final[-k:] + final[:-k]  # k push(shift) rotations restore `final`
    ids = [offset + index[n] for n in nums]
    expr = ("parseInt(sA(0x%x))/0x1+-parseInt(sA(0x%x))/0x2*(parseInt(sA(0x%x))/0x3)+parseInt(sA(0x%x))/0x4"
            % tuple(ids))
    target = E._Checksum(expr).eval(lambda i: final[i - offset])
    assert target == int(target) and target > 0
    body = re.sub(r"@@D(\d+)@@", lambda m: "dA(0x%x)" % (offset + int(m.group(1))), body)
    body = re.sub(r"\s*\n\s*", "", body)
    head = ("function dA(x1,x2){var x3=gA();return dA=function(x4,x5){x4=x4-0x%x;var x6=x3[x4];return x6;},dA(x1,x2);}"
            "(function(y1,y2){var sA=dA,y3=y1();while(!![]){try{var y4=%s;if(y4===y2)break;else y3['push'](y3['shift']());}"
            "catch(y5){y3['push'](y3['shift']());}}}(gA,0x%x),!(function(){var sB=dA;})());"
            "function gA(){var z1=[%s];gA=function(){return z1;};return gA();}"
            % (offset, expr, int(target), ",".join(E.js_quote(s) for s in stored)))
    # a decoder alias inside a function, like the real thing: function f(){var a=dA; ... a(0x1b0) ...}
    m = re.search(r"function ks1\(n1, n2\) \{.*?return k2;\}", body)
    body = body[:m.start()] + m.group(0).replace("dA(", "al1(").replace("{", "{var al1=dA;", 1) + body[m.end():]
    return head + body


def test_synthetic(tmp):
    fx = make_fixture()
    doc, err = run_extract(fx)
    check(err is None, "fixture extracts (%s)" % err)
    if doc:
        check_values("fixture", doc, dict(FIX_EXP, anchors=("en",)))
        ex = doc["extras"]
        check(ex["matchmaker_url"] == "https://mm.example.test", "fixture: matchmaker URL")
        check(ex["turnstile_site_key"] == "0x4AAAAAAAtestSiteKey000", "fixture: turnstile key")
        check(ex["regions"] == {"regions": ["de-fra", "us-nj"], "conditional": ["sss"]}, "fixture: regions")
        check(ex["dedicated_only_regions"] == ["us-wa"], "fixture: dedicated regions")
        check(ex["client_revision"] == "7", "fixture: clientRevision")
        check(ex["custom_build_version"] == {"value": None, "env": "CUSTOM_VERSION"}, "fixture: customBuildVersion")
        check(doc["warnings"] == [], "fixture: no warnings")
    ob = obfuscate(fx)
    check("\n" not in ob and "'buildVersion'" in ob and "dA(0x" in ob, "obfuscated fixture is one line with decoder calls")
    odoc, err = run_extract(ob)
    check(err is None, "obfuscated fixture extracts (%s)" % err)
    if odoc and doc:
        check(comparable(odoc) == comparable(doc), "obfuscated fixture: values identical to the readable one")
        check(odoc["deobfuscation"] and odoc["deobfuscation"][0]["calls"] > 50, "obfuscated fixture: decoder calls replaced")
    # decimal numbers, as v10.js prints them
    dec = re.sub(r"0x([0-9a-f]+)", lambda m: str(int(m.group(1), 16)), make_fixture())
    ddoc, err = run_extract(dec)
    check(err is None and ddoc and ddoc["ahkey"] == FIX_EXP["ahkey"], "decimal fixture extracts (%s)" % err)
    ddoc, err = run_extract(make_fixture(ahkey="94547"))
    check(err is None and ddoc and ddoc["ahkey"] == 94547, "decimal AHKEY accepted (%s)" % err)

    # every anchor broken -> non-zero exit naming it, nothing written
    breaks = [
        ("AHKEY", lambda s: s.replace("'cptR'] && (this", "'cptX'] && (this")),
        ("AHKEY", lambda s: s.replace("return a1 + a2 & 0xff;", "return a1 - a2 & 0xff;")),
        ("AHKEY", lambda s: s.replace("0x2a5f1", "0x2a5f1f")),
        ("ahNum trailer", lambda s: s.replace("[a3 >> 0x4 & 0xf, 0xf & a3]", "[a3 >> 0x3 & 0xf, 0xf & a3]")),
        ("clientVersion", lambda s: s.replace("'clientVersion': vv2", "'clientVersion': vvX")),
        ("clientVersion", lambda s: s.replace("var vv1 = '9.8.7';", "var vv1 = '9.8';")),
        ("buildVersion", lambda s: s.replace("AbCdEfGhIjKlMnOpQrStUvWxYz012345", "AbCdEfGhIjKlMnOpQrStUvWxYz01234")),
        ("packet table c2s", lambda s: s.replace("'ENTER_GAME': 'en'", "'ENTER_GAMEX': 'en'")),
        ("packet table s2c", lambda s: s.replace("'IO_INIT': 'io-init'", "'IO_INITX': 'io-init'")),
        ("packet table s2c", lambda s: s.replace("'CHEAT_CHECK': 'cc'", "'CHEAT_CHECKX': 'cc'")),
        ("cheat check", lambda s: s.replace("0x811c9dc5", "0x811c9dc6")),
        ("cheat salt", lambda s: s.replace("var salt1 = [", "var salt1 = [0x1, ")),
        ("cheat keystream", lambda s: s.replace(") + k3 & 0xff", ") + 0x1 & 0xff")),
        ("cheat handler", lambda s: s.replace("'_eval': e1", "'_evil': e1")),
    ]
    for anchor, brk in breaks:
        broken = brk(fx)
        check(broken != fx, "break of %s applies" % anchor)
        path = os.path.join(tmp, "broken.js")
        with open(path, "w", encoding="utf8") as f:
            f.write(broken)
        hdr, js = os.path.join(tmp, "out.h"), os.path.join(tmp, "out.json")
        err = io.StringIO()
        with redirect_stderr(err), redirect_stdout(io.StringIO()):
            rc = E.main([path, "--header", hdr, "--json", js])
        msg = err.getvalue()
        check(rc != 0 and ("FAILED: " + anchor) in msg and not os.path.exists(hdr) and not os.path.exists(js),
              "broken %-17s -> rc %d, %s" % (anchor, rc, msg.strip().split("\n")[0][:110]))


# ------------------------------------------------------- 3. committed header, CLI, C compile

def test_committed(tmp):
    with open(E.OUT_JSON, encoding="utf8") as f:
        doc = json.load(f)
    with open(E.OUT_HEADER, encoding="utf8") as f:
        hdr = f.read()
    check(E.render_header(doc) == hdr, "krunker_build.h == render(build.json)")
    check(doc["generator_rev"] == E.GENERATOR_REV, "build.json generator rev current")
    check(re.fullmatch(r"[0-9a-f]{64}", doc["source"]["sha256"]) is not None, "build.json records the source sha256")
    check("KRUNKER_BUILD_SOURCE_SHA256 \"%s\"" % doc["source"]["sha256"] in hdr, "header records the source sha256")
    deob = os.path.join(OFFICIAL, "game.deob.js")
    if os.path.exists(deob):
        with open(deob, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        if sha == doc["source"]["sha256"]:
            # the CLI, as `make krunker-build-check` runs it, offline on the local copy
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "extract_build.py"), deob, "--check"],
                               capture_output=True, text=True, timeout=300)
            check(r.returncode == 0, "--check on the committed files: rc %d %s" % (r.returncode, r.stderr.strip()[:200]))
            # regenerate into a temp dir: identical except provenance (url vs local file)
            hdr2, js2 = os.path.join(tmp, "k.h"), os.path.join(tmp, "k.json")
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "extract_build.py"), deob,
                                "--header", hdr2, "--json", js2], capture_output=True, text=True, timeout=300)
            check(r.returncode == 0, "CLI regenerate rc %d" % r.returncode)
            if r.returncode == 0:
                with open(js2, encoding="utf8") as f:
                    d2 = json.load(f)
                d1 = dict(doc)
                d1.pop("source")
                d2.pop("source")
                check(d1 == d2, "regenerated build.json values == committed")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        print("  SKIP C compile (no compiler)")
        return
    c = os.path.join(tmp, "use_header.c")
    with open(c, "w") as f:
        f.write(r'''#include "krunker_build.h"
#include <string.h>
#if KRUNKER_BUILD_SCHEMA != 1
#error schema
#endif
#define WIRE(id, wire, name) wire,
static const char *const c2s[KRUNKER_C2S_COUNT] = { KRUNKER_C2S_PACKETS(WIRE) };
static const char *const s2c[KRUNKER_S2C_COUNT] = { KRUNKER_S2C_PACKETS(WIRE) };
int main(void) {
    unsigned ah = 0;
    ah = (ah + KRUNKER_AHKEY) & 0xffu;
    return !(ah == KRUNKER_AHNUM_STEP && !strcmp(c2s[KRUNKER_C2S_ENTER_GAME], "en") &&
             !strcmp(s2c[KRUNKER_S2C_IO_INIT], "io-init") && !strcmp(c2s[KRUNKER_C2S_cptR], "cptR") &&
             sizeof KRUNKER_CHEAT_SALT - 1 == KRUNKER_CHEAT_SALT_LEN && KRUNKER_CHEAT_FNV_OFFSET == 0x811c9dc5u);
}
''')
    exe = os.path.join(tmp, "use_header")
    r = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I",
                        os.path.join(ROOT, "shared", "include"), c, "-o", exe], capture_output=True, text=True, timeout=120)
    check(r.returncode == 0, "header compiles as C11 -Werror with %s %s" % (os.path.basename(cc), r.stderr[:300]))
    if r.returncode == 0:
        check(subprocess.run([exe], timeout=30).returncode == 0, "header values usable from C")


def main():
    tmp = tempfile.mkdtemp(prefix="kn-krunker-extract-test-")
    try:
        for name, fn in (("real bundles", test_real_bundles), ("synthetic bundles", test_synthetic),
                         ("committed header", test_committed)):
            print("%s:" % name)
            fn(tmp)
    finally:
        shutil.rmtree(tmp)
    print("%d checks passed, %d failed" % (passed[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
