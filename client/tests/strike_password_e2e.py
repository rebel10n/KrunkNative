#!/usr/bin/env python3
"""A Krunker Strike join the server refuses for its password (ERROR 004005) opens Strike's own password prompt,
and Submit joins the same game again with the typed password (client/src/ui/strike_menu.c SERVER_PASSWORD,
client/src/ui/servers_ui.c servers_ui_connection_lost / servers_ui_join_password).

  python3 -I client/tests/strike_password_e2e.py BUILD_DIR      (run from the repository root)

A standalone Strike server_bin with sv_password (no brain, no director) and two headless Strike clients
(KRUNKNATIVE_HEADLESS=1, an input script, fixed 60 fps step). The server log is the judge of what was sent:
"Wrong server password" is logged for every refused password, and nothing for a right one.
  A. client_bin --flavor strike --connect host:port: the startup connect is refused before the page is booted
     (the prompt must survive the boot's menu screen). The prompt opens (codeInput, Submit, Leave shown, the field
     empty). A wrong password is typed and submitted: refused by the server, and the prompt opens again. The right
     password is typed and submitted: no refusal follows, the prompt is gone. v10's own prompt is not used.
  B. the same server, a Strike client that connects from the menu: the developer console's `connect host:port`
     is refused for the password; the prompt opens; the right password is typed and submitted, and is accepted.
Needs a headless GL device (KRUNKNATIVE_EGL_DEVICE picks it). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402  (free_ports: ports free for TCP and UDP right now)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-password-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PASSWORD = "letmein"
WRONG = "nope"
WINDOW = (1280, 720)
REFUSED = "Wrong server password"
results = []
procs = []

VIS = re.compile(r"\[input_script\] vis t=\d+ @(\S+) (.*)$", re.M)
TEXT = re.compile(r"\[input_script\] text t=\d+ @(\S+) n=\d+ checked=\d value=(.*?) text=", re.M)


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def vis(text, label):
    """the last `vis @label` line's fields, e.g. {'codeInput': '1'}; {} when none"""
    fields = {}
    for m in VIS.finditer(text):
        if m.group(1) == label:
            fields = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    return fields


def value(text, label):
    """the field's value at the last `text @label` probe, None when no probe"""
    found = None
    for m in TEXT.finditer(text):
        if m.group(1) == label:
            found = m.group(2)
    return found


def after_last_submit(text):
    """the client log from the last Submit on (the join it sent with the typed password)"""
    i = text.rfind("again with the server password")
    return text[i:] if i >= 0 else ""


def refused_for_password(text):
    """the client was disconnected with a password refusal (ERROR 004005)"""
    return re.search(r"disconnected from server: .*\(004005\)", text) is not None


def start_server(port):
    cfg = os.path.join(WORK, "server.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Strike password e2e"\nport {port}\nnode_id PW1\nregion local\nmaxplayers 6\n'
                f"mode ffa\nsv_password {PASSWORD}\njoin_timeout 2\n")
    config = os.path.join(WORK, "none.json")
    with open(config, "w") as f:
        f.write("{}\n")
    out = open(os.path.join(WORK, "server.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--flavor", "strike", "--cfg", cfg, "--config", config],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return p


def server_refusals():
    """the server's refusals for a wrong password (one "Rejecting ...: Wrong server password" line each)"""
    return len(re.findall(r"^Rejecting \S+ \(id \d+\): " + REFUSED + "$", read(os.path.join(WORK, "server.log")), re.M))


def run_client(name, args, script, timeout=300):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(cfg, exist_ok=True)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "%dx%d" % WINDOW})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    return read(log)


def script_a():
    """startup connect (boot case): a wrong password, then the right one"""
    return "\n".join([
        "0 boot",
        "15000 vis @asked codeInput confirmBtn declineBtn",
        "15100 text @before [id=codeInput]",
        "15200 clickid codeInput",
        f"15400 type {WRONG}",
        "15600 clickid confirmBtn",
        "19000 vis @again codeInput confirmBtn",
        "19100 text @empty [id=codeInput]",
        "19200 clickid codeInput",
        f"19400 type {PASSWORD}",
        "19500 text @typed [id=codeInput]",
        "19600 clickid confirmBtn",
        "24000 vis @done codeInput confirmBtn",
        "24500 quit",
    ]) + "\n"


def script_b(port):
    """console `connect` from the menu (the menu path): the right password"""
    return "\n".join([
        "0 boot",
        "600 down GRAVE", "640 up GRAVE",          # ` opens the developer console
        "900 console @open",
        f"1000 type connect 127.0.0.1:{port}",
        "1200 down ENTER", "1240 up ENTER",        # runs it: the direct connect is refused for the password
        "1400 clicksel [id=devConsoleOverlay] .titlebar .x",  # close the console by its X (a ` would be typed in the field)
        "15000 vis @asked codeInput confirmBtn declineBtn",
        "15100 text @before [id=codeInput]",
        "15200 clickid codeInput",
        f"15400 type {PASSWORD}",
        "15500 text @typed [id=codeInput]",
        "15600 clickid confirmBtn",
        "19000 vis @done codeInput confirmBtn",
        "19500 quit",
    ]) + "\n"


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/server_bin", os.X_OK):
        print(f"no {BUILD}/client_bin or server_bin")
        return 1
    port = kn_proto.free_ports(1)[0]
    start_server(port)
    time.sleep(1.0)
    try:
        # A. startup connect, refused before the page is booted
        text = run_client("startup", ["--flavor", "strike", "--connect", f"127.0.0.1:{port}"], script_a())
        check("A: the refused startup connect opens Strike's password prompt",
              "[strike] the game needs a password" in text and "[strike] screen SERVER_PASSWORD" in text,
              "no prompt line in the client log")
        asked = vis(text, "asked")
        check("A: the prompt shows its field, Submit and Leave",
              asked.get("codeInput") == "1" and asked.get("confirmBtn") == "1" and asked.get("declineBtn") == "1", str(asked))
        check("A: the field starts empty", value(text, "before") == "", repr(value(text, "before")))
        check("A: Strike's prompt is used, not v10's own popup", "[party] the game needs a password" not in text)
        check("A: no Lost Connection popup comes over the prompt", "[strike] screen POPUP_WARNING" not in text)
        check("A: the wrong password is refused by the server, and the prompt opens again",
              server_refusals() == 1 and vis(text, "again").get("codeInput") == "1",
              f"refusals={server_refusals()} again={vis(text, 'again')}")
        check("A: the typed password is what Submit sent (the field holds it, the action runs)",
              value(text, "typed") == PASSWORD and text.count("[strike] action join-password") >= 2,
              f"typed={value(text, 'typed')!r}")
        check("A: the right password is accepted (no refusal after the last Submit)",
              not refused_for_password(after_last_submit(text)) and server_refusals() == 1,
              after_last_submit(text)[-300:])
        check("A: the prompt is gone after the right password",
              vis(text, "done").get("codeInput") == "-" and vis(text, "done").get("confirmBtn") == "-", str(vis(text, "done")))

        # B. console connect from the menu, refused, the right password
        text = run_client("console", ["--flavor", "strike"], script_b(port))
        asked = vis(text, "asked")
        check("B: a console connect refused for its password opens the prompt",
              "[strike] screen SERVER_PASSWORD" in text and asked.get("codeInput") == "1", str(asked))
        check("B: the field starts empty", value(text, "before") == "", repr(value(text, "before")))
        check("B: no Lost Connection popup comes over the prompt", "[strike] screen POPUP_WARNING" not in text)
        check("B: the typed password is what Submit sent", value(text, "typed") == PASSWORD, repr(value(text, "typed")))
        check("B: the right password is accepted (no refusal after Submit, no new one on the server)",
              not refused_for_password(after_last_submit(text)) and server_refusals() == 1,
              after_last_submit(text)[-300:])
        check("B: the prompt is gone after the right password",
              vis(text, "done").get("codeInput") == "-" and vis(text, "done").get("confirmBtn") == "-", str(vis(text, "done")))
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
