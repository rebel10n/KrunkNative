#!/usr/bin/env python3
"""The Host Game flow's server_bin command lines parse and start (shared/src/host_command.c).

  python3 -I server/tests/host_command_parse_test.py BUILD_DIR      (run from the repository root)

host_command_test --emit prints the lines the flow generates (the common settings, the Zombies host
type with the game settings). Each line is split the way a POSIX shell splits it and run through
server_bin with a free port, --cfg none and --no-cli, plus one line naming a map file as the
positional argument. Every one must start (print "Listening on port"), print no usage and no config
error, and exit 0 on SIGTERM. Needs KRUNKNATIVE_ASSETS_DIR (default: assets/ of this checkout) for
the catalogue maps. Exit 77 when the binaries are not built.
"""
import os
import shlex
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
SERVER = os.path.join(ROOT, BUILD, "server_bin")
EMIT = os.path.join(ROOT, BUILD, "host_command_test")
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR", os.path.join(ROOT, "assets"))
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def run_server(argv, workdir, env):
    """Start server_bin, wait for its listen line (or its exit), stop it with SIGTERM; returns (out, code)."""
    out_path = os.path.join(workdir, "server.out")
    with open(out_path, "w") as out:
        proc = subprocess.Popen(argv, cwd=workdir, env=env, stdout=out, stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    listening = False
    while time.time() < deadline and proc.poll() is None:
        with open(out_path) as f:
            if "Listening on port" in f.read():
                listening = True
                break
        time.sleep(0.2)
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        code = proc.wait(timeout=40)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = -9
    with open(out_path) as f:
        text = f.read()
    return text, code, listening


def main():
    if not os.path.exists(SERVER) or not os.path.exists(EMIT):
        print(f"SKIP  server_bin / host_command_test not built in {BUILD}")
        return 77
    emitted = subprocess.run([EMIT, "--emit"], capture_output=True, text=True, timeout=60)
    check("host_command_test --emit runs", emitted.returncode == 0, emitted.stderr.strip())
    lines = [l for l in emitted.stdout.splitlines() if l.strip()]
    check("three generated lines", len(lines) == 3, str(len(lines)))

    commands = [shlex.split(line) for line in lines]
    map_file = os.path.join(ASSETS, "maps", "official", "10_kanji.json")
    commands.append(["server_bin", map_file, "--set", "gameserver.game.gameTime=7", "--set", "gameserver.game.maxPlayers=6", "--headless"])
    ports = kn_proto.free_ports(len(commands))
    env = dict(os.environ, KRUNKNATIVE_ASSETS_DIR=ASSETS, KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_AUDIO_BACKEND="null")
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)

    for index, (words, port) in enumerate(zip(commands, ports)):
        label = f"line {index + 1}"
        if words[0] != "server_bin":
            check(f"{label} starts with server_bin", False, words[0])
            continue
        work = tempfile.mkdtemp(prefix="kn-hostcmd-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
        env["XDG_CONFIG_HOME"] = os.path.join(work, "cfg")
        argv = [SERVER] + words[1:] + ["--cfg", "none", "--port", str(port), "--no-cli"]
        text, code, listening = run_server(argv, work, env)
        check(f"{label} parses and listens", listening, text[-400:] if not listening else "")
        check(f"{label} prints no usage", "Usage:" not in text, text[:200] if "Usage:" in text else "")
        check(f"{label} has no config error", "Config error" not in text and "unknown" not in text.lower(), text[-300:] if "unknown" in text.lower() or "Config error" in text else "")
        check(f"{label} stops on SIGTERM", code == 0, f"exit {code}")

    passed = sum(results)
    print(f"\n{passed}/{len(results)} checks passed", flush=True)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
