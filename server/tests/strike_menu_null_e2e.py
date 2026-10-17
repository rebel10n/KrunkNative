#!/usr/bin/env python3
"""Krunker Strike's menus show no "null" text. FRVR's sandbox capture left a literal "null" in
TAP_TO_PLAY's top-centre mode label (.frvr-tap-to-play-map-mode-name-label, beside the green Extras
button); client/src/ui/strike_menu.c now fills that label with the game mode's name, or leaves it empty.
MAIN_MENU and TAP_TO_PLAY, for a guest and for a signed-in account:

  python3 -I server/tests/strike_menu_null_e2e.py BUILD_DIR      (run from the repository root)

  guest   headless client_bin --flavor strike --offline (Kanji): MAIN_MENU (the [id=frvrMenuMobile] root) has
          no "null"; PLAY, spawn, ESC -> TAP_TO_PLAY ([id=frvrTapToPlayMobile]) has no "null", and its mode
          label is the mode's name. (The input script's #id selectors match nothing; [id=...] does.)
  signed  the same two screens for a signed-in account: kn_brain on a scratch database, a brain-linked STK
          node and a client that signs in as a new account (strike_social_e2e's harness).
Prints PASS/FAIL lines; exit 0 only when everything passed. Needs a headless GL device; the signed-in run
needs kn_brain and python websockets (skipped, exit 77, when they are missing).
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-null-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
MODE_NAME = "Free for All"  # GameModeConfig.name of the ffa mode (shared/src/modes.c)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def seed_played(cfg, *names):
    """A returning player (the FTUE gate, client/src/ui/strike_ftue.c strike_ftue_first_run): a first run goes to
    quick connect and MAIN_MENU is never up, so the played flag is stored for each account key first. The file is
    in the Strike flavor's own config directory (flavor_storage_subdir: "krunknative-strike", not Box Office's
    "krunknative")."""
    path = os.path.join(cfg, "krunknative-strike", "knFtue.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump({name: {"played": True} for name in ("guest",) + names}, f)


def script():
    # MAIN_MENU: the root's text has no "null"; PLAY, a spawn, ESC pauses the match into TAP_TO_PLAY
    return ["0 menu",
            "3000 nodes @main [id=frvrMenuMobile]",
            "3000 grep @main-null null [id=frvrMenuMobile]",
            "3100 clickid mobMenuPlay",
            "8000 spawn",
            "9000 down ESC",
            "9020 up ESC",
            "10000 nodes @ttp [id=frvrTapToPlayMobile]",
            "10000 grep @ttp-null null [id=frvrTapToPlayMobile]",
            "10000 text @ttp-mode .frvr-tap-to-play-map-mode-name-label",
            "10100 quit"]


def parse(text, tag):
    grep = {m.group(1): (int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r"\[input_script\] grep t=\d+ @(\S+) needle=\S+ n=(\d+) hits=(\d+)", text)}
    texts = {m.group(1): m.group(2).strip()
             for m in re.finditer(r"\[input_script\] text t=\d+ @(\S+) n=\d+ \S+ value=\S* text=(.*)", text)}
    return grep, texts


def verdict(tag, text):
    grep, texts = parse(text, tag)
    check(f"{tag}: the Strike page loaded and ran its script", "[strike] Krunker Strike page ready" in text and "[input_script]" in text)
    n, hits = grep.get("main-null", (0, -1))
    check(f"{tag}: MAIN_MENU is up and has no \"null\" text", n == 1 and hits == 0, f"n={n} hits={hits}")
    n, hits = grep.get("ttp-null", (0, -1))
    check(f"{tag}: TAP_TO_PLAY is up and has no \"null\" text", n == 1 and hits == 0, f"n={n} hits={hits}")
    label = texts.get("ttp-mode", "<missing>")
    check(f"{tag}: TAP_TO_PLAY's mode label is the mode's name", label == MODE_NAME, repr(label))


def run_guest():
    path = os.path.join(WORK, "guest.script")
    with open(path, "w") as f:
        f.write("\n".join(script()) + "\n")
    log = os.path.join(WORK, "guest.log")
    seed_played(os.path.join(WORK, "cfg"))
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--flavor", "strike", "--offline", "--map", "kanji"],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("guest: client_bin finished its script", False, f"timeout, log {log}")
            return
    with open(log, errors="replace") as f:
        verdict("guest", f.read())


def run_signed():
    import strike_social_e2e as S  # the friends harness: kn_brain, a brain-linked node, the signed-in client
    import kn_proto  # noqa: E402 - on the path the import above put it on
    shutil.rmtree(S.WORK, ignore_errors=True)  # the import's own scratch dir: its logs go in WORK (kept on failure)
    S.WORK = WORK

    name = "kn_test_strike_null_%d" % os.getpid()
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", S.DRIVER, name], capture_output=True)
    if r.returncode:
        print("strike_menu_null_e2e: %s unavailable: signed-in run skipped" % S.DRIVER)
        return
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=S.SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=strike-null-%d" % os.getpid()],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        S.procs.append(brain)
        m = S.wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", S.read_log(brain_log)), 15)
        check("signed: kn_brain starts", bool(m), S.read_log(brain_log)[-300:])
        if not m:
            return
        port = int(m.group(1))
        node = S.Node(port, "STK", kn_proto.free_ports(1)[0], flavor="strike")
        check("signed: a brain-linked strike node with a lobby", node.linked and bool(node.game), str(node.game))
        if not (node.linked and node.game):
            return
        user = "menuchk%d" % (os.getpid() % 100000)  # the name is on MAIN_MENU: it must not itself contain "null"
        pw = "pw-null-%d" % (os.getpid() % 100000)
        code, reg = S.http(port, "/auth/register", {"username": user, "password": pw})
        token = reg.get("token", "") if isinstance(reg, dict) else ""
        check("signed: an account registers", bool(token), str(reg)[:200])
        if not token:
            return
        seed_played(os.path.join(WORK, "signed-cfg"), user)  # S.client's config dir: WORK/<name>-cfg
        p, log = S.client("signed", port, "\n".join(script()) + "\n", node.game, (user, token))
        S.stop(p, 240)
        verdict("signed", S.read_log(log))
    finally:
        for p in S.procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in S.procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", S.DRIVER, name], capture_output=True)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("strike_menu_null_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    run_guest()
    try:
        run_signed()
    except SystemExit as e:  # strike_social_e2e exits 77 without python websockets
        if e.code == 77:
            print("strike_menu_null_e2e: the signed-in run is skipped (python websockets missing)")
        else:
            raise
    if all(results) and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
