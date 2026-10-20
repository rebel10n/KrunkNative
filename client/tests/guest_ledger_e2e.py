#!/usr/bin/env python3
"""A guest's Unclaimed Rewards come from kn_brain, not from the browser (client/src/ui/v10_rewards.c).

  python3 -I client/tests/guest_ledger_e2e.py BUILD_DIR      (run from the repository root)

v10 keeps a signed-out guest's balance on the server (GET /guest/profile); KrunkNative's brain keeps it in
guest_profiles, keyed by the guest id the client keeps on its device (cfg/krunknative/guestId.json). A
scratch sqlite database (services/brain/tests/db_scratch.py) holds one row for a random guest id:
funds 12, score 800. The device also holds a stale browser balance, guestEarned.json {"kr": 3, "score": 3},
which the client must ignore. A headless client_bin --offline (no account: a guest, the brain still answers)
opens the menu and samples the box every 1500 ms from 1 s to 12 s:
  run 1 (the brain's balance 12 / 800):
    1. the Unclaimed Rewards box is on the menu (knGuestEarned) and its KR count ends at 12 (knGuestKrMenu),
       the XP one at 800 (knGuestXpMenu): the brain's numbers, not the stale 3 and 3;
    2. the stale guestEarned.json is removed by the page's first load (its value is not moved);
    3. the device keeps the guest id it had (the page reads a valid id, it never replaces it);
    4. the client log does not print the guest id.
  run 2 (the brain's balance 0 / 0): no box at all (no knGuestEarned, no knGuestKrMenu).
Exit 0 when every check passes, 1 when one fails, 77 when skipped (no client_bin, no sqlite).
A git worktree lacks the checkout's gitignored game content (assets/css, models, sound, ...): then
KRUNKNATIVE_ASSETS_DIR must name a directory that has it (the client exits 255 without the fonts).
"""
import json
import os
import re
import secrets
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = os.getcwd()
BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_guest_ledger_%d" % os.getpid()
GUEST = secrets.token_hex(16)  # 32 lowercase hex: this run's guest id (never printed)
STALE = {"kr": 3, "score": 3}  # the browser's old ledger: the page must neither show nor move it
WORK = tempfile.mkdtemp(prefix="kn-guest-ledger-e2e.",
                        dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=15.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def scratch_db(action):
    """db_scratch.py: create / drop the test database (sqlite file or the PostgreSQL schema)."""
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    if action == "drop":
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return None
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    return json.loads(r.stdout) if r.returncode == 0 else r.returncode


def db_exec(cfg, sql, args=()):
    """One statement on the scratch database (sqlite only: the row is set directly)."""
    con = sqlite3.connect(cfg["path"])
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def client_env(brain_port, base):
    os.makedirs(os.path.join(base, "cfg", "krunknative"), exist_ok=True)
    return dict({k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}, **{
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(base, "cfg"),
        "XDG_STATE_HOME": os.path.join(base, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
        "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
        "KRUNKNATIVE_NET_TRANSPORT": "tcp",
        "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port})


def samples(log, label):
    """[(t, n, text)] of every `text @label` probe of the input script log, in order."""
    return [(int(m.group(1)), int(m.group(2)), m.group(3).strip())
            for m in re.finditer(r"\[input_script\] text t=(\d+) @%s n=(\d+) .*? text=(.*)" % re.escape(label), log)]


def last(log, label):
    s = samples(log, label)
    return s[-1] if s else None


def write_script(path):
    lines = ["0 menu"]
    for t in range(1000, 12001, 1500):
        lines += [f"{t} text @gk [id=knGuestKrMenu]", f"{t} text @gx [id=knGuestXpMenu]", f"{t} text @gb [id=knGuestEarned]"]
    lines.append("12500 quit")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def run_client(port, name, seed):
    """One headless guest session on a fresh device: the seeded storage files, then the probe script."""
    base = os.path.join(WORK, name)
    env = client_env(port, base)
    for key, text in seed.items():
        with open(os.path.join(base, "cfg", "krunknative", key + ".json"), "w") as f:
            f.write(text) # the stored values are raw text (junk_wallet_e2e.py)
    with open(os.path.join(base, "cfg", "krunknative", "knSettings.json"), "w") as f:
        f.write(json.dumps({"knSeekOnLaunch": False})) # no Quick Play seek at launch (it would hold the script's clock)
    script = os.path.join(base, "client.script")
    write_script(script)
    env["KRUNKNATIVE_INPUT_SCRIPT"] = script
    log = os.path.join(base, "client.log")
    with open(log, "w") as out:
        client = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(client)
        try:
            client.wait(240)
        except subprocess.TimeoutExpired:
            os.killpg(client.pid, signal.SIGTERM)
            client.wait(5)
            return base, None
    return base, read_log(log)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("guest_ledger_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    if DRIVER != "sqlite":
        print("guest_ledger_e2e: this check runs on sqlite (its guest_profiles row is set directly): skipped")
        return 77
    cfg = scratch_db("create")
    if not isinstance(cfg, dict):
        print("guest_ledger_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    brain_bin = f"{BUILD}/services/brain/kn_brain"
    if not os.access(brain_bin, os.X_OK):
        check("kn_brain is built (services_all)", False, brain_bin)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in cfg.items()}
    env = dict(os.environ, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([brain_bin, "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([brain_bin, "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local",
                                  "--brain.id=guest-ledger-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)))
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))

        try:
            db_exec(cfg, "INSERT INTO guest_profiles (guest_id, funds, score, updated_at) VALUES (?, ?, ?, ?)",
                    (GUEST, 12, 800, int(time.time())))
        except sqlite3.Error as e:
            check("the brain's guest_profiles table holds the guest's balance (migration 0350)", False, str(e))
            return 1

        base1, log1 = run_client(port, "run1", {"guestId": GUEST, "guestEarned": json.dumps(STALE)})
        if log1 is None:
            check("run 1: the client finished its script", False, "timed out; log in " + base1)
            return 1
        gk, gx, gb = last(log1, "gk"), last(log1, "gx"), last(log1, "gb")
        check("1: the Unclaimed Rewards box is on the menu for the guest's balance", gb is not None and gb[1] == 1,
              str(gb and gb[1:]))
        check("1: the KR count ends at 12, the brain's funds (not the stale 3)", gk is not None and gk[2] == "12",
              str(gk and gk[2]))
        check("1: the XP count ends at 800, the brain's score (not the stale 3)", gx is not None and gx[2] == "800",
              str(gx and gx[2]))
        stale_path = os.path.join(base1, "cfg", "krunknative", "guestEarned.json")
        check("2: the stale browser balance is removed by the page's first load", not os.path.exists(stale_path)
              or read_log(stale_path) == "", "guestEarned.json still holds a value")
        check("3: the device keeps the guest id it had", read_log(os.path.join(base1, "cfg", "krunknative", "guestId.json")) == GUEST)
        check("4: the client log does not print the guest id", GUEST not in log1)

        db_exec(cfg, "UPDATE guest_profiles SET funds = 0, score = 0 WHERE guest_id = ?", (GUEST,))
        _, log2 = run_client(port, "run2", {"guestId": GUEST, "guestEarned": json.dumps(STALE)})
        if log2 is None:
            check("run 2: the client finished its script", False, "timed out")
            return 1
        gk2, gb2 = last(log2, "gk"), last(log2, "gb")
        check("run 2 (balance 0 / 0): no Unclaimed Rewards box on the menu", gb2 is not None and gb2[1] == 0
              and gk2 is not None and gk2[1] == 0, str(gb2 and gb2[1:]))
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
        scratch_db("drop")
    ok = bool(results) and all(results)
    print("guest_ledger_e2e: " + ("OK" if ok else "FAILED") + " (%d checks)" % len(results))
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
