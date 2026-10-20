#!/usr/bin/env python3
"""Client updates end to end (doc/backend.md "Client updates"): launcher + updates server + client.

    python3 -I launcher/tests/launcher_e2e.py <build dir> [--keep] [--no-client]

The updates store is made by the real tools/release/kn-updates.py (stage + publish + assets) from a
fake package and a fake game-content tree, and served by a local HTTP server on a free port
(kn_proto.free_ports) that logs every request. The "new" build is this build's client_bin and
launcher with their embedded commit id patched (same length), so they report another commit.

  1  fresh portable install, old build, partial content: the launcher detects the newer build,
     downloads + verifies + swaps the executables and ui/, installs the package's own assets/
     files, leaves the rest of assets/ alone, fetches only the missing/changed content files,
     then starts the new client (-- --version: it prints the patched commit)
  2  second start: up to date, nothing downloaded
  3  content update on the server: only the changed / added files are fetched, the removed one
     deleted, the user's own file and the executables untouched
  4  deb install: no executable update (package manager), content synced into the user data dir
     ($XDG_DATA_HOME/krunknative/assets) together with the package's assets/ files
  5  corrupt package on the server: refused by its sha256, the install stays as it was
  5b an unreachable updates server (the 2026-10-13 report: assets.krunk.io had no DNS record after
     the move to Spaceship): the launcher retries a failed lookup / 5xx (3 tries), says why
     ("cannot resolve <host> (DNS: ...)", "HTTP 404 from <url>") on stderr and in the window,
     does not retry a 404, and a 503 that clears on the second try still finds the update
  6  the client itself (headless, --offline): detects "outdated" from the manifest and `update
     now` restarts through KRUNKNATIVE_LAUNCHER (a stand-in that records its arguments)
  7  Auto-restart for updates (Settings > Client, default on; client/include/game_reload.h), the
     real launcher (KRUNKNATIVE_LAUNCHER = the install's own) and a second channel manifest
     (no game content set, a package without assets/: nothing is written outside the scratch dir):
     a  in a match (offline game): the newer build is found mid-match (the manifest answers 503
        until the player is in game, then `update check`) and the client does NOT restart while
        playing; the match ends (`end`): it restarts through the launcher, which installs the
        update (sha256, the staged client's --version check) and starts the new client with the
        same command line, back in the offline game
     b  in the menu: the newer build restarts the client right away; the new client is current
     c  a package the launcher refuses (corrupt): one restart, the launcher starts the installed
        client again, which does not restart for that build a second time (no loop)
     d  a package whose client does not run here (launcher.c staged_client_runs): the same, the
        install never gets the staged client
"""
import argparse
import hashlib
import http.server
import io
import json
import os
import re
import shutil
import socketserver
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
import kn_proto  # noqa: E402  (free_ports)

KN_UPDATES = os.path.join(REPO, "tools", "release", "kn-updates.py")
FAILS = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def run(cmd, env=None, cwd=None, timeout=120):
    p = subprocess.run(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return p.returncode, p.stdout.decode(errors="replace")


AUTO_GATE = threading.Event()  # step 7: /auto/ answers 503 until set
AUTO_GATE.set()
FLAKY = {"left": 0}  # step 5b: /flaky/ answers 503 this many times, then serves


class Log:
    def __init__(self):
        self.lock = threading.Lock()
        self.paths = []

    def take(self):
        with self.lock:
            out, self.paths = self.paths, []
        return out


def serve(root, port, log):
    class Handler(http.server.SimpleHTTPRequestHandler):
        protocol_version = "HTTP/1.1"  # keep-alive, as the launcher's downloader expects

        def __init__(self, *a, **kw):
            super().__init__(*a, directory=root, **kw)

        def do_GET(self):
            with log.lock:
                log.paths.append(self.path)
            if self.path.startswith("/auto/") and not AUTO_GATE.is_set():
                self.send_error(503)
                return
            if self.path.startswith("/flaky/"):
                with log.lock:
                    flaky = FLAKY["left"] > 0
                    FLAKY["left"] -= 1 if flaky else 0
                if flaky:
                    self.send_error(503)
                    return
            super().do_GET()

        def log_message(self, *a):
            pass

    class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    srv = Server(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def patch_commit(src, dst, old12, new12):
    data = read(src)
    n = data.count(old12.encode())
    write(dst, data.replace(old12.encode(), new12.encode()))
    os.chmod(dst, 0o755)
    return n


def make_package(path, top, files):
    with tarfile.open(path, "w:gz") as t:
        for rel, (data, mode) in sorted(files.items()):
            info = tarfile.TarInfo(f"{top}/{rel}")
            info.size = len(data)
            info.mode = mode
            info.mtime = int(time.time())
            t.addfile(info, io.BytesIO(data))


def tree_state(root):
    """path -> (inode, mtime_ns, sha256) of every file below root."""
    out = {}
    for d, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(d, fn)
            st = os.lstat(p)
            out[os.path.relpath(p, root)] = (st.st_ino, st.st_mtime_ns, sha256(read(p)) if stat.S_ISREG(st.st_mode) else "")
    return out


def objects_fetched(paths):
    return sorted(p.rsplit("/", 1)[1] for p in paths if "/assets/objects/" in p)


AUTO_PIDS = set()  # step 7: client / launcher processes of our installs (killed at the end)


def pids_of(inst):
    """pids whose executable lies in the install dir inst (our scratch installs only)."""
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            exe = os.readlink("/proc/%s/exe" % d)
            with open("/proc/%s/stat" % d) as f:
                zombie = f.read().split(")")[-1].split()[0] == "Z"
        except OSError:
            continue
        if not zombie and exe.startswith(inst.rstrip("/") + "/"):
            out.append(int(d))
    return out


def kill_tree_pid(pid):
    for sig in (15, 9):
        try:
            os.kill(pid, sig)
        except OSError:
            return
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except OSError:
                return
            time.sleep(0.1)


def private_assets(tmp):
    """A game content dir of symlinks to the checkout's assets/ entries: step 7's manifest has no
    content set and its packages no assets/, so the launcher writes nothing into it."""
    root = os.path.join(tmp, "auto-assets")
    if not os.path.isdir(root):
        os.makedirs(root)
        real = os.path.realpath(os.path.join(REPO, "assets"))
        for entry in os.listdir(real):
            os.symlink(os.path.realpath(os.path.join(real, entry)), os.path.join(root, entry))
    return root


def auto_restart_steps(tmp, env, srv_root, port, plat, marker, new_bins, kn_build, new12, old12, client_bin, launcher_bin):
    store = os.path.join(srv_root, "auto", "client")
    pkg_name = f"krunknative-client-{plat}.tar.gz"
    pkg_files = {
        "client_bin": (read(os.path.join(new_bins, "client_bin")), 0o755),
        "krunknative": (read(os.path.join(new_bins, "krunknative")), 0o755),
        "ui/index.html": (b"<html>new</html>", 0o644),
        "kn-install.json": (json.dumps(marker).encode(), 0o644),
    }
    os.makedirs(os.path.join(store, "builds", new12))
    pkg = os.path.join(store, "builds", new12, pkg_name)
    make_package(pkg, f"krunknative-client-{plat}", pkg_files)
    good = read(pkg)
    manifest = {"format": 1, "channel": "latest", "build": json.loads(kn_build),
                "platforms": {plat: {"update": {"url": f"builds/{new12}/{pkg_name}", "sha256": sha256(good), "size": len(good)}}}}
    write(os.path.join(store, "latest.json"), json.dumps(manifest))
    url = f"http://127.0.0.1:{port}/auto/client/latest.json"
    assets = private_assets(tmp)

    def run_auto(name, args, script, relaunch, gate_on=None, timeout=180):
        inst = os.path.join(tmp, name)
        os.makedirs(inst)
        shutil.copy2(client_bin, os.path.join(inst, "client_bin"))  # the old build (no assets/: see private_assets)
        shutil.copy2(launcher_bin, os.path.join(inst, "krunknative"))
        write(os.path.join(inst, "ui", "index.html"), "<html>old</html>")
        write(os.path.join(inst, "kn-install.json"), json.dumps(dict(marker, package="tar.gz")))
        write(os.path.join(tmp, name + ".script"), script)
        write(os.path.join(tmp, name + "-relaunch.script"), relaunch)
        cenv = dict(env)
        cenv.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_ASSETS_DIR": assets, "KRUNKNATIVE_UI_DIR": os.path.join(REPO, "ui"),
                     "KRUNKNATIVE_LAUNCHER": os.path.join(inst, "krunknative"), "KRUNKNATIVE_UPDATES_URL": url,
                     "KRUNKNATIVE_INPUT_SCRIPT": os.path.join(tmp, name + ".script"),
                     "KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH": os.path.join(tmp, name + "-relaunch.script"),
                     "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                     "KRUNKNATIVE_LOCAL_BUILD": "0",
                     "XDG_CONFIG_HOME": os.path.join(tmp, name + "-cfg"), "XDG_STATE_HOME": os.path.join(tmp, name + "-state")})
        log = os.path.join(tmp, name + ".log")
        if gate_on:
            AUTO_GATE.clear()
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(inst, "client_bin")] + args, cwd=inst, env=cenv,
                             stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        AUTO_PIDS.add(p.pid)
        end = time.time() + timeout
        while time.time() < end:
            text = read(log).decode(errors="replace")
            if gate_on and not AUTO_GATE.is_set() and re.search(gate_on, text):
                AUTO_GATE.set()
            p.poll()
            live = pids_of(inst)
            AUTO_PIDS.update(live)
            if p.returncode is not None and not live:
                break
            time.sleep(0.2)
        else:
            check(False, f"{name}: the clients finished in time")
        for pid in pids_of(inst):
            kill_tree_pid(pid)
        AUTO_GATE.set()
        return inst, read(log).decode(errors="replace")

    # 7a: in a match. The newer build is found while playing; the restart waits for the match end.
    print("7a auto-restart: in a match")
    inst, out = run_auto("auto-match", ["--offline"],
                         "0 play\n1000 state\n3000 con update check\n9000 state\n9500 con end\n60000 quit\n",
                         "0 play\n2000 state\n8000 quit\n", gate_on=r"\[input_script\] state t=")
    print("\n".join("     " + line for line in out.splitlines() if re.match(r"\[(update|reload|launcher)\]", line)))
    found = out.find("[reload] a newer client is ready")
    restart = out.find("[update] restarting through")
    st = [m.group(1) for m in re.finditer(r"\[input_script\] state t=\d+ slot=\S+ weapon=.+? menu=(\d)", out)]
    check(found >= 0 and "(Update now: via the launcher)" in out, "7a: the client finds the newer build (mid-match)")
    check(found >= 0 and restart > found and "the match ended, restarting" in out[found:restart] and len(st) >= 2 and st[1] == "0",
          "7a: no restart while playing (still in game after the check), only once the match ended")
    check(f"updated to v9.9.9-1-g{new12[:7]}" in out and read(os.path.join(inst, "client_bin")) == pkg_files["client_bin"][0],
          "7a: the launcher installed the update (sha256 + staged client check) after the restart")
    after = out[restart:] if restart >= 0 else ""
    check(f"({new12}," in after and re.search(r"\[update\] latest \S+ \(\S+\): current", after) is not None,
          "7a: the new client came back (the patched commit, up to date)")
    check(re.search(r"\[input_script\] state t=\d+ slot=\S+ weapon=.+? menu=0", after.split("starting ")[-1]) is not None,
          "7a: the new client is back in the offline game (the same command line)")

    # 7b: in the menu: right away
    print("7b auto-restart: in the menu")
    inst, out = run_auto("auto-menu", ["--offline"], "0 menu\n40000 quit\n", "0 menu\n6000 quit\n")
    print("\n".join("     " + line for line in out.splitlines() if re.match(r"\[(update|reload|launcher)\]", line)))
    check("in the menu, restarting" in out and "[update] restarting through" in out, "7b: in the menu the newer build restarts the client")
    check(read(os.path.join(inst, "client_bin")) == pkg_files["client_bin"][0] and f"({new12}," in out.split("restarting through")[-1],
          "7b: the update is installed and the new client runs")
    check(out.count("restarting through") == 1, "7b: one restart (the new client is current)")

    # 7c: the launcher refuses the package: one restart, then the installed client stays
    print("7c auto-restart: a package the launcher refuses")
    write(pkg, good[:-10] + b"0123456789")
    inst, out = run_auto("auto-bad", ["--offline"], "0 menu\n40000 quit\n", "0 menu\n9000 quit\n")
    write(pkg, good)
    print("\n".join("     " + line for line in out.splitlines() if re.match(r"\[(update|reload|launcher)\]", line)))
    check("corrupt" in out and "the update failed: starting the installed client" in out, "7c: the launcher refused it and started the installed client")
    check(read(os.path.join(inst, "client_bin")) != pkg_files["client_bin"][0] and f"({old12}," in out.split("starting the installed client")[-1],
          "7c: the install is unchanged, the old client runs again")
    check(out.count("[update] restarting through") == 1 and "was tried already" in out, "7c: no second restart for that build (no loop)")

    # 7d: the staged client does not run here (launcher.c staged_client_runs): never installed, one
    # restart, the installed client runs again and does not restart for that build a second time
    print("7d auto-restart: a staged client that does not run here")
    broken = dict(pkg_files, client_bin=(b"#!/bin/sh\necho 'needs a newer system' >&2\nexit 3\n", 0o755))
    make_package(pkg, f"krunknative-client-{plat}", broken)
    bad = read(pkg)
    write(os.path.join(store, "latest.json"), json.dumps(dict(manifest, platforms={plat: {"update": {
        "url": f"builds/{new12}/{pkg_name}", "sha256": sha256(bad), "size": len(bad)}}})))
    inst, out = run_auto("auto-norun", ["--offline"], "0 menu\n40000 quit\n", "0 menu\n9000 quit\n")
    write(pkg, good)
    write(os.path.join(store, "latest.json"), json.dumps(manifest))
    print("\n".join("     " + line for line in out.splitlines() if re.match(r"\[(update|reload|launcher)\]", line)))
    check("does not start here" in out and "the update failed: starting the installed client" in out,
          "7d: the launcher's staged client check refused it and started the installed client")
    check(read(os.path.join(inst, "client_bin")) == read(client_bin) and f"({old12}," in out.split("starting the installed client")[-1],
          "7d: the install is unchanged (the staged client never replaced it), the old client runs again")
    check(out.count("[update] restarting through") == 1 and "was tried already" in out, "7d: no second restart for that build (no loop)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    ap.add_argument("--no-client", action="store_true", help="skip step 6 (needs the game content + headless GL)")
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    client_bin, launcher_bin = os.path.join(build, "client_bin"), os.path.join(build, "krunknative")
    for b in (client_bin, launcher_bin):
        if not os.path.exists(b):
            sys.exit(f"missing {b} (build the client_bin and kn_launcher targets)")
    scratch_base = os.path.join(os.path.expanduser("~"), ".cache")
    os.makedirs(scratch_base, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="kn-launcher-e2e-", dir=scratch_base)
    print(f"launcher e2e in {tmp}")

    _, ver = run([client_bin, "--version"])
    m = re.search(r"\(([0-9a-f]{12}), built", ver)
    if not m:
        sys.exit(f"cannot read the commit from: {ver}")
    old12 = m.group(1)
    new12 = "".join("0123456789abcdef"[(int(c, 16) + 7) % 16] for c in old12)
    # the binary's commit time is not printed: the published build is "now + 3 days", newer than
    # any committed build by more than the 48 h grace, so this build counts as outdated
    old_time = int(time.time())
    new_commit = new12 + "ab" * 14
    proto = re.search(r"protocol (\d+\.\d+)", ver).group(1)
    plat = "linux-amd64" if os.uname().machine in ("x86_64", "amd64") else "linux-aarch64"

    # ---------------------------------------------------------------- the store
    new_bins = os.path.join(tmp, "newbins")
    n1 = patch_commit(client_bin, os.path.join(new_bins, "client_bin"), old12, new12)
    n2 = patch_commit(launcher_bin, os.path.join(new_bins, "krunknative"), old12, new12)
    check(n1 > 0 and n2 > 0, f"patched the commit {old12} -> {new12} into the new build ({n1}, {n2} places)")
    marker = {"format": 1, "platform": plat, "package": "tar.gz", "channel": "latest", "root": "."}
    pkg_files = {
        "client_bin": (read(os.path.join(new_bins, "client_bin")), 0o755),
        "krunknative": (read(os.path.join(new_bins, "krunknative")), 0o755),
        "ui/index.html": (b"<html>new</html>", 0o644),
        "ui/NEW": (b"new ui file", 0o644),
        "kn-install.json": (json.dumps(marker).encode(), 0o644),
        "assets/shaders/x.vert": (b"new shader", 0o644),
    }
    pkgs = os.path.join(tmp, "packages")
    os.makedirs(pkgs)
    pkg_name = f"krunknative-client-{plat}.tar.gz"
    make_package(os.path.join(pkgs, pkg_name), f"krunknative-client-{plat}", pkg_files)
    write(os.path.join(pkgs, pkg_name + ".sha256"), f"{sha256(read(os.path.join(pkgs, pkg_name)))}  {pkg_name}\n")
    web = os.path.join(tmp, "web-assets")  # stands in for /opt/krunknative-web/assets on kru
    for rel, data in {"textures/a.png": "A1", "sound/b.ogg": "B1", "css/fonts/font2.ttf": "FONT", "css/fonts/copy.ttf": "FONT",
                      "shaders/x.vert": "stale web copy", "version.txt": "x"}.items():
        write(os.path.join(web, rel), data)
    store = os.path.join(tmp, "store")
    kn_build = json.dumps({"describe": "v9.9.9-1-g" + new12[:7], "commit": new_commit, "commit_time": old_time + 3 * 86400,
                           "build_time": "2026-10-12T00:00:00Z", "protocol": proto})
    code, out = run([sys.executable, "-I", KN_UPDATES, "stage", "--kn-build", kn_build, "--packages", pkgs,
                     "--out", os.path.join(tmp, "incoming", new12), "--download-base",
                     "https://github.com/rebel10n/KrunkNative/releases/download/client-latest"])
    check(code == 0, "kn-updates stage")
    staged = json.loads(read(os.path.join(tmp, "incoming", new12, "build.json")))
    check(staged["package_assets"] == ["shaders/x.vert"], "stage lists the assets/ files the package ships")
    code, out = run([sys.executable, "-I", KN_UPDATES, "publish", os.path.join(tmp, "incoming", new12), "--root", store,
                     "--web-assets", web])
    print(out.strip())
    check(code == 0, "kn-updates publish")
    manifest = json.loads(read(os.path.join(store, "public", "client", "latest.json")))
    listing = json.loads(read(os.path.join(store, "public", "assets", manifest["assets"]["version"] + ".json")))
    check(sorted(f[0] for f in listing["files"]) == ["css/fonts/copy.ttf", "css/fonts/font2.ttf", "sound/b.ogg", "textures/a.png"],
          "the content index leaves out version.txt and the package's own files")
    check(manifest["assets"].get("packaged") == ["shaders/x.vert"], "the manifest names the package's own files")
    obj = os.path.join(store, "public", "assets", "objects", sha256(b"A1")[:2], sha256(b"A1"))
    check(os.path.exists(obj) and os.stat(obj).st_ino == os.stat(os.path.join(web, "textures/a.png")).st_ino,
          "objects are hardlinks into the web asset tree")

    srv_root = os.path.join(tmp, "srv")
    os.makedirs(srv_root)
    os.symlink(os.path.join(store, "public"), os.path.join(srv_root, "updates"))
    os.symlink(os.path.join(store, "public"), os.path.join(srv_root, "flaky"))
    port = kn_proto.free_ports(1)[0]
    log = Log()
    srv = serve(srv_root, port, log)
    url = f"http://127.0.0.1:{port}/updates/client/latest.json"
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_ASSETS_DIR", "KRUNKNATIVE_LAUNCHER"):
        env.pop(k, None)
    env["KRUNKNATIVE_UPDATES_URL"] = url
    env["XDG_DATA_HOME"] = os.path.join(tmp, "xdg-data")
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "xdg-config")

    def make_install(name, package):
        inst = os.path.join(tmp, name)
        os.makedirs(inst)
        shutil.copy2(client_bin, os.path.join(inst, "client_bin"))
        shutil.copy2(launcher_bin, os.path.join(inst, "krunknative"))
        write(os.path.join(inst, "ui", "index.html"), "<html>old</html>")
        write(os.path.join(inst, "ui", "OLD_ONLY"), "removed by the update")
        write(os.path.join(inst, "kn-install.json"), json.dumps(dict(marker, package=package)))
        write(os.path.join(inst, "assets", "shaders", "x.vert"), "old shader")
        return inst

    try:
        # ------------------------------------------------------------ 1 update + first sync
        print("1  portable install: executables + content")
        inst = make_install("install", "tar.gz")
        assets = os.path.join(inst, "assets")
        write(os.path.join(assets, "textures", "a.png"), "A1")    # already right: adopted by hash
        write(os.path.join(assets, "sound", "b.ogg"), "B0")       # stale: fetched
        write(os.path.join(assets, "user-added.txt"), "mine")      # not managed: never touched
        before = tree_state(assets)
        log.take()
        code, out = run([os.path.join(inst, "krunknative"), "--cli", "--full-assets", "--", "--version"], env=env, cwd=tmp)
        print("\n".join("     " + line for line in out.strip().splitlines()))
        paths = log.take()
        check(code == 0, "launcher exit 0")
        check(any(p.endswith(pkg_name) for p in paths), "downloaded the update package")
        check(read(os.path.join(inst, "client_bin")) == pkg_files["client_bin"][0], "client_bin swapped")
        check(read(os.path.join(inst, "krunknative")) == pkg_files["krunknative"][0], "launcher swapped (self-update)")
        check(os.access(os.path.join(inst, "client_bin"), os.X_OK), "client_bin still executable")
        check(read(os.path.join(inst, "ui", "index.html")) == b"<html>new</html>" and os.path.exists(os.path.join(inst, "ui", "NEW"))
              and not os.path.exists(os.path.join(inst, "ui", "OLD_ONLY")), "ui/ replaced as a whole")
        check(read(os.path.join(assets, "shaders", "x.vert")) == b"new shader", "the package's own assets file installed")
        after = tree_state(assets)
        check(after["textures/a.png"] == before["textures/a.png"], "matching content file untouched (same inode, mtime)")
        check(after["user-added.txt"] == before["user-added.txt"], "the user's own file untouched")
        check(read(os.path.join(assets, "sound", "b.ogg")) == b"B1" and read(os.path.join(assets, "css", "fonts", "font2.ttf")) == b"FONT"
              and read(os.path.join(assets, "css", "fonts", "copy.ttf")) == b"FONT", "stale + missing content fetched (two paths, one object)")
        # FONT once: two paths with one content are downloaded once and copied (assets_sync.c)
        check(objects_fetched(paths) == sorted([sha256(b"B1"), sha256(b"FONT")]), "only the needed objects were requested, each once")
        check(f"({new12}," in out, "the launcher started the new client (it reports the patched commit)")
        check(not os.path.exists(os.path.join(inst, ".kn-update")), "no update state left behind")
        state = json.loads(read(os.path.join(assets, ".kn-assets.json")))
        check(state["version"] == manifest["assets"]["version"], "content state recorded")

        # ------------------------------------------------------------ 2 nothing to do
        print("2  second start")
        exe_before = tree_state(inst)
        code, out = run([os.path.join(inst, "krunknative"), "--cli", "--", "--version"], env=env, cwd=tmp)
        paths = log.take()
        check(code == 0 and "current" in out, "up to date")
        check(paths == ["/updates/client/latest.json"], f"only the manifest was fetched ({len(paths)} requests)")
        check(tree_state(inst) == exe_before, "nothing in the install changed")

        # ------------------------------------------------------------ 3 content diff
        print("3  content update")
        write(os.path.join(web, "textures", "a.png.tmp"), "A2")
        os.replace(os.path.join(web, "textures", "a.png.tmp"), os.path.join(web, "textures", "a.png"))  # new inode, as rsync does
        os.unlink(os.path.join(web, "sound", "b.ogg"))
        write(os.path.join(web, "maps2", "new.bin"), "NEW")
        code, out = run([sys.executable, "-I", KN_UPDATES, "assets", "--root", store, "--web-assets", web])
        print(out.strip())
        manifest2 = json.loads(read(os.path.join(store, "public", "client", "latest.json")))
        check(code == 0 and manifest2["assets"]["version"] != manifest["assets"]["version"], "kn-updates assets: new content version")
        # the client's cache (KN_ASSETS_OBJECTS): an object of content the list no longer has goes, a listed one stays
        old_obj = os.path.join(assets, ".kn-objects", sha256(b"A1")[:2], sha256(b"A1"))
        kept_obj = os.path.join(assets, ".kn-objects", sha256(b"FONT")[:2], sha256(b"FONT"))
        write(old_obj, "A1")
        write(kept_obj, "FONT")
        exe_files = {k: v for k, v in tree_state(inst).items() if not k.startswith("assets/")}
        code, out = run([os.path.join(inst, "krunknative"), "--cli", "--full-assets", "--", "--version"], env=env, cwd=tmp)
        print("\n".join("     " + line for line in out.strip().splitlines() if "game content" in line))
        paths = log.take()
        check(code == 0, "launcher exit 0")
        check(objects_fetched(paths) == sorted([sha256(b"A2"), sha256(b"NEW")]), "only the changed + added objects requested")
        check(not any("/builds/" in p for p in paths), "no executable download")
        check(read(os.path.join(assets, "textures", "a.png")) == b"A2" and read(os.path.join(assets, "maps2", "new.bin")) == b"NEW",
              "changed + added files in place")
        check(not os.path.exists(os.path.join(assets, "sound", "b.ogg")), "the removed file is deleted")
        check(not os.path.exists(old_obj) and read(kept_obj) == b"FONT", "the cache drops an object the list no longer names, keeps the others")
        check(read(os.path.join(assets, "user-added.txt")) == b"mine", "the user's own file kept")
        check({k: v for k, v in tree_state(inst).items() if not k.startswith("assets/")} == exe_files, "executables untouched")

        # ------------------------------------------------------------ 4 deb: package manager
        print("4  deb install")
        deb = make_install("install-deb", "deb")
        os.chmod(deb, 0o555)  # /opt/krunknative is not the user's
        deb_client = read(os.path.join(deb, "client_bin"))
        write(os.path.join(tmp, "xdg-data", "krunknative", "assets", "textures", "a.png"), "OLD!")  # stale copy (wrong size)
        code, out = run([os.path.join(deb, "krunknative"), "--cli", "--", "--version"], env=env, cwd=tmp)
        paths = log.take()
        data_assets = os.path.join(tmp, "xdg-data", "krunknative", "assets")
        check(code == 0 and "package manager" in out, "update deferred to the package manager")
        check(read(os.path.join(deb, "client_bin")) == deb_client and not any("/builds/" in p for p in paths), "executables untouched")
        check(read(os.path.join(data_assets, "css", "fonts", "font2.ttf")) == b"FONT", "content synced into $XDG_DATA_HOME/krunknative/assets")
        check(read(os.path.join(data_assets, "shaders", "x.vert")) == b"old shader", "the installed package's own assets/ file mirrored")
        # the default first run: the menu set is downloaded, the rest only listed (the game fetches it on demand)
        check(not os.path.exists(os.path.join(data_assets, "textures", "a.png")), "default run: the stale non-menu copy is removed, left to the game")
        state_paths = {f[0] for f in json.loads(read(os.path.join(data_assets, ".kn-assets.json")))["files"]}
        check("textures/a.png" in state_paths, "default run: the state still lists it (the whole list)")
        check(f"({old12}," in out, "the installed (old) client started")
        os.chmod(deb, 0o755)

        # ------------------------------------------------------------ 5 corrupt package
        print("5  corrupt package")
        bad = make_install("install-bad", "tar.gz")
        pkg_path = os.path.join(store, "public", "client", "builds", new12, pkg_name)
        good = read(pkg_path)
        write(pkg_path, good[:-10] + b"0123456789")
        before = tree_state(bad)
        code, out = run([os.path.join(bad, "krunknative"), "--cli", "--no-launch"], env=env, cwd=tmp)
        log.take()
        check("corrupt" in out, "sha256 mismatch detected")
        after = {k: v for k, v in tree_state(bad).items() if not k.startswith("assets/")}
        check(after == {k: v for k, v in before.items() if not k.startswith("assets/")}, "install unchanged")
        write(pkg_path, good)

        # ------------------------------------------------------------ 5b unreachable server
        print("5b unreachable updates server")
        un = make_install("install-unreachable", "tar.gz")  # no game content yet: nothing playable
        uenv = dict(env, KRUNKNATIVE_UPDATES_URL="https://nohost.invalid/updates/client/latest.json")  # RFC 6761: never resolves
        code, out = run([os.path.join(un, "krunknative"), "--cli", "--no-launch"], env=uenv, cwd=tmp)
        print("\n".join("     " + line for line in out.strip().splitlines() if "launcher]" in line and "Checking" not in line)[-600:])
        check(code == 2, f"no manifest and no content: exit 2 ({code})")
        check("cannot resolve nohost.invalid (DNS" in out, "the reason names the DNS lookup and the host")
        check("Cannot reach the update server to download the game content: cannot resolve nohost.invalid" in out,
              "the window's error line carries the reason, not the content dir")
        check("try 3 of 3" in out, "a failed lookup is tried 3 times")
        log.take()
        code, out = run([os.path.join(un, "krunknative"), "--cli", "--check"],
                        env=dict(env, KRUNKNATIVE_UPDATES_URL=f"http://127.0.0.1:{port}/updates/client/missing.json"), cwd=tmp)
        paths = log.take()
        check(code == 2 and f"HTTP 404 from http://127.0.0.1:{port}/updates/client/missing.json" in out, "a 404 says HTTP 404 and the URL")
        check(paths == ["/updates/client/missing.json"], f"a 404 is not retried ({len(paths)} requests)")
        FLAKY["left"] = 1
        code, out = run([os.path.join(un, "krunknative"), "--cli", "--check"],
                        env=dict(env, KRUNKNATIVE_UPDATES_URL=f"http://127.0.0.1:{port}/flaky/client/latest.json"), cwd=tmp)
        paths = log.take()
        check(code == 3 and "reached the server on try 2" in out, f"a 503 then 200: the second try finds the update (exit {code})")
        check(paths == ["/flaky/client/latest.json"] * 2, f"two manifest requests ({paths})")

        # ------------------------------------------------------------ 6 the client's own check
        if not a.no_client:
            print("6  client: Outdated + Update now")
            cl = make_install("install-client", "tar.gz")
            record = os.path.join(tmp, "launcher-args.txt")
            fake = os.path.join(tmp, "fake-launcher.sh")
            write(fake, f"#!/bin/sh\necho \"$@\" > '{record}'\n")
            os.chmod(fake, 0o755)
            script = os.path.join(tmp, "update-now.script")
            write(script, "0 play\n2500 down ESC\n2550 up ESC\n6000 con update now\n20000 quit\n")
            cenv = dict(env)
            cenv.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_ASSETS_DIR": os.path.join(REPO, "assets"),
                         "KRUNKNATIVE_UI_DIR": os.path.join(REPO, "ui"), "KRUNKNATIVE_LAUNCHER": fake,
                         "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_NO_BROWSER": "1",
                         "KRUNKNATIVE_LOCAL_BUILD": "0"})  # a published build: a dev build's flag would hide the box
            t0 = time.time()
            code, out = run(["timeout", "120", os.path.join(cl, "client_bin"), "--offline"], env=cenv, cwd=cl, timeout=150)
            took = time.time() - t0
            lines = [line for line in out.splitlines() if line.startswith("[update]")]
            print("\n".join("     " + line for line in lines))
            check(any("outdated" in line for line in lines), "the client reports itself outdated")
            check(any("restarting through" in line for line in lines), "update now restarts through the launcher")
            time.sleep(0.5)
            args = read(record).decode().split() if os.path.exists(record) else []
            # --wait-pid <pid>, then the client's own command line after "--" (it comes back the same)
            check(args[:2] == ["--wait-pid", args[1] if len(args) > 1 else ""] and len(args) > 1 and args[1].isdigit()
                  and args[2:] == ["--", "--offline"], f"launcher started with --wait-pid <pid> -- --offline ({args})")
            check(took < 60, f"the client quit for the update ({took:.0f} s, before the script's quit)")
        # ------------------------------------------------------------ 7 auto-restart for updates
        if not a.no_client:
            auto_restart_steps(tmp, env, srv_root, port, plat, marker, new_bins, kn_build, new12, old12, client_bin, launcher_bin)
    finally:
        for pid in AUTO_PIDS:
            kill_tree_pid(pid)
        srv.shutdown()
        if FAILS or a.keep:
            print(f"scratch kept: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"launcher e2e: {'FAILED: ' + '; '.join(FAILS) if FAILS else 'all checks passed'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
