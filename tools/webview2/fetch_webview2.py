#!/usr/bin/env python3
"""The WebView2 SDK headers and loader for the Windows captcha webview (KN_WEBVIEW on Windows).

The package is Microsoft.Web.WebView2 (NuGet), pinned in webview2.lock by sha256: the .nupkg, then the
members taken from it (the C header and the x64 loader). `install` downloads the package (through
KN_FETCH_PROXY, e.g. http://host:port, like tools/branding/fetch_branding.py), checks it and writes the
members under --stage (default ~/.cache/krunknative/webview2): include/WebView2.h and x64/WebView2Loader.dll.
CMake reads that directory (KN_WEBVIEW2_DIR). WebView2EnvironmentOptions.h is not taken: it is C++ (WRL),
and the backend passes no environment options.

  fetch_webview2.py install [--stage DIR]   download, verify, write the members
  fetch_webview2.py check [--stage DIR]     the staged members match the lock (exit 1 otherwise)
  fetch_webview2.py repin                   rewrite webview2.lock from a fresh download (after a look)
"""
import argparse
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = os.path.join(HERE, "webview2.lock")
DEFAULT_STAGE = os.path.join(os.path.expanduser("~"), ".cache", "krunknative", "webview2")
PACKAGE_VERSION = "1.0.4258.31"
PACKAGE_URL = ("https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/%s/microsoft.web.webview2.%s.nupkg"
               % (PACKAGE_VERSION, PACKAGE_VERSION))
# staged name -> member path inside the package
MEMBERS = [
    ("include/WebView2.h", "build/native/include/WebView2.h"),
    ("x64/WebView2Loader.dll", "build/native/x64/WebView2Loader.dll"),
]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_lock():
    pins = {}
    with open(LOCK) as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            name, src, digest, size = line.rstrip("\n").split("\t")
            pins[name] = {"source": src, "sha256": digest, "size": int(size)}
    return pins


def write_lock(package, members):
    with open(LOCK, "w") as f:
        f.write("# WebView2 SDK for the Windows captcha webview (tools/webview2/fetch_webview2.py).\n")
        f.write("# name\tsource\tsha256\tsize   (package: the .nupkg; the others: members inside it)\n")
        f.write("package\t%s\t%s\t%d\n" % (PACKAGE_URL, package["sha256"], package["size"]))
        for staged, member in MEMBERS:
            f.write("%s\t%s\t%s\t%d\n" % (staged, member, members[staged]["sha256"], members[staged]["size"]))


def download(url):
    proxy = os.environ.get("KN_FETCH_PROXY", "")
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {}))
    req = urllib.request.Request(url, headers={"User-Agent": "KrunkNative-fetch/1"})
    with opener.open(req, timeout=300) as r:
        return r.read()


def members_of(data):
    """The pinned members taken out of the package bytes (their hashes are checked by the caller)."""
    out = {}
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for staged, member in MEMBERS:
            body = z.read(member)
            out[staged] = {"sha256": sha256(body), "size": len(body), "data": body}
    return out


def cmd_install(a):
    pins = read_lock()
    data = download(PACKAGE_URL)
    if sha256(data) != pins["package"]["sha256"] or len(data) != pins["package"]["size"]:
        raise SystemExit("%s: sha256 %s != pinned %s" % (PACKAGE_URL, sha256(data), pins["package"]["sha256"]))
    for staged, body in members_of(data).items():
        if body["sha256"] != pins[staged]["sha256"] or body["size"] != pins[staged]["size"]:
            raise SystemExit("%s: member sha256 %s != pinned %s" % (staged, body["sha256"], pins[staged]["sha256"]))
        dest = os.path.join(a.stage, staged)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        tmp = dest + ".part"
        with open(tmp, "wb") as f:
            f.write(body["data"])
        os.replace(tmp, dest)
        print("wrote %s (%d bytes)" % (dest, body["size"]))


def cmd_check(a):
    pins = read_lock()
    bad = 0
    for staged, _member in MEMBERS:
        path = os.path.join(a.stage, staged)
        if not os.path.isfile(path):
            print("missing: %s" % path)
            bad = 1
            continue
        with open(path, "rb") as f:
            body = f.read()
        if sha256(body) != pins[staged]["sha256"]:
            print("changed: %s" % path)
            bad = 1
        else:
            print("ok: %s" % path)
    raise SystemExit(bad)


def cmd_repin(_a):
    data = download(PACKAGE_URL)
    members = members_of(data)
    write_lock({"sha256": sha256(data), "size": len(data)},
               {k: {"sha256": v["sha256"], "size": v["size"]} for k, v in members.items()})
    print("wrote %s" % LOCK)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn in (("install", cmd_install), ("check", cmd_check)):
        s = sub.add_parser(name)
        s.add_argument("--stage", default=DEFAULT_STAGE, help="where the members go (default %(default)s)")
        s.set_defaults(fn=fn)
    sub.add_parser("repin").set_defaults(fn=cmd_repin)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
