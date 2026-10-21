#!/usr/bin/env python3
"""CORS for the official Krunker page on kn_brain (the coordinator's /ping and /mm/* answers).

  cors_origin_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

kn_brain runs on an ephemeral port with brain.cors_origins set to the live list (the official page's
origins and the KN front ends, deploy/ansible/group_vars/all.yml). The page's Origin must get
Access-Control-Allow-Origin with that exact origin, on GET and on the preflight; an origin outside the
list, and a request without an Origin, must not. A static check then holds deploy/nginx/cors.conf to
the same two lists. Each check prints one PASS or FAIL line. Exit 0 when every check passes, 1 otherwise,
77 when there is no kn_brain binary.
"""
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else ""
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG

PAGE = ["https://krunker.io", "https://www.krunker.io"]
KN_FRONT = ["https://krunk.io", "https://www.krunk.io", "https://rebel10n.github.io", "https://krunker.party"]
OTHER = "https://evil.test"

FAILS = 0


def check(name, ok, detail=""):
    global FAILS
    if not ok:
        FAILS += 1
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, ("  " + detail) if (detail and not ok) else ""))


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def request(port, method, path, origin=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method)
    if origin:
        req.add_header("Origin", origin)
    if method == "OPTIONS":
        req.add_header("Access-Control-Request-Method", "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.headers
    except urllib.error.HTTPError as e:
        return e.code, e.headers


def acao(headers):
    return headers.get("Access-Control-Allow-Origin")


def list_in(path, key):
    """The origins under `key:` in a group_vars style YAML list (no YAML parser needed)."""
    out, on = [], False
    for line in read(path).splitlines():
        if re.match(r"^%s:\s*$" % re.escape(key), line):
            on = True
            continue
        if on:
            m = re.match(r'^\s+-\s+"([^"]+)"\s*$', line)
            if m:
                out.append(m.group(1).replace("{{ kn_domain }}", "krunk.io"))
            elif line.strip() and not line.startswith((" ", "\t")):
                break  # the next top-level key or comment ends the list
    return out


def static_checks():
    allv = os.path.join(ROOT, "deploy", "ansible", "group_vars", "all.yml")
    tmpl = os.path.join(ROOT, "deploy", "ansible", "roles", "kn_brain", "templates", "config.json.j2")
    cors = os.path.join(ROOT, "deploy", "nginx", "cors.conf")
    web = list_in(allv, "kn_web_origins")
    official = list_in(allv, "kn_official_page_origins")
    check("group_vars lists the official page origins", official == PAGE, repr(official))
    check("the brain template sends kn_official_page_origins", "kn_official_page_origins" in read(tmpl))
    directives = "\n".join(l for l in read(cors).splitlines() if not l.lstrip().startswith("#"))
    nginx = sorted(set(re.findall(r'if \(\$http_origin = "([^"]+)"\)', directives)))
    check("cors.conf echoes only the listed origins (no '*')",
          "'*'" not in directives and '"*"' not in directives and
          nginx == sorted(set(web + official)), repr(nginx))


def scenario(port):
    for origin in PAGE:
        for path in ["/ping", "/mm/ping"]:
            st, h = request(port, "GET", path, origin)
            check("GET %s from %s gets its origin" % (path, origin), st == 200 and acao(h) == origin,
                  "status=%s acao=%r" % (st, acao(h)))
            check("GET %s from %s varies on Origin" % (path, origin),
                  "origin" in (h.get("Vary") or "").lower(), repr(h.get("Vary")))
        st, h = request(port, "OPTIONS", "/mm/ping", origin)
        check("preflight from %s gets its origin" % origin, st in (200, 204) and acao(h) == origin,
              "status=%s acao=%r" % (st, acao(h)))
    for origin in KN_FRONT:
        st, h = request(port, "GET", "/ping", origin)
        check("KN front end %s still gets its origin" % origin, acao(h) == origin, "acao=%r" % acao(h))
    st, h = request(port, "GET", "/ping", OTHER)
    check("an origin outside the list gets no Access-Control-Allow-Origin", acao(h) is None, "acao=%r" % acao(h))
    st, h = request(port, "GET", "/ping", None)
    check("a request without an Origin gets no wildcard", acao(h) is None, "acao=%r" % acao(h))


def main():
    static_checks()
    if not BRAIN or not os.path.isfile(BRAIN) or not os.access(BRAIN, os.X_OK):
        print("cors_origin_test: no kn_brain at %s: skipped (static checks only)" % BRAIN)
        return 1 if FAILS else 77
    log = os.path.join(os.environ.get("TMPDIR", "/tmp"), "kn-cors-origin-%d.log" % os.getpid())
    env = dict(os.environ)
    origins = ",".join(PAGE + KN_FRONT)
    brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                              "--brain.id=cors-test-%d" % os.getpid(), "--brain.cors_origins=" + origins],
                             cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+)", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        scenario(int(m.group(1)))
    finally:
        brain.terminate()
        try:
            brain.wait(timeout=5)
        except subprocess.TimeoutExpired:
            brain.kill()
        try:
            os.remove(log)
        except OSError:
            pass
    print("cors_origin_test: %s" % ("all checks passed" if not FAILS else "%d check(s) failed" % FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
