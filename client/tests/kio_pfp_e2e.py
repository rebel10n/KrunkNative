#!/usr/bin/env python3
"""A krunker.io account's profile picture (account_ui.c kio_pfp_*, the Account window), end to end against the krunker.io
stubs of server/tests/krunker_account_e2e.py. Nothing leaves the machine; no kn_brain.

  python3 -I client/tests/kio_pfp_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7 (deob-10.0.7.js): the Account window's #profilePicM shows a premium account's own picture, user-assets
/p<id>/profile.png (:218977-218978, :24307-24312), with "Change Picture" opening pfpPopup (:223293-223295). Its Select picks
a PNG (updateFile: the name and the size in kb), Publish/Update refuses a file over 250 kb ("Failed. 250kb Image Limit") and
sends UPLOAD_PROFILE_PICTURE ["uploadP", data URL] on the game socket (:223508-223524); UPLOAD_PROFILE_RESPONSE ["upPR"]
gives the "Uploaded!" popup (:225328-225333). Before this, the krunker.io account's window had the stock picture and no
"Change Picture" (KrunkNative's own upload goes to kn_brain, which the gate refuses while krunker.io is the backend).
The client starts with a stored krunker.io session (a premium account), joins the stub's game, goes back to the menu:
  page    showWindow 5: the picture is user-assets /p777/profile.png, not blurred, with "Change Picture"
  big     pfpPopup, Select (the file dialog is a stub zenity on PATH: a 300 kB PNG), Publish/Update: the 250 kb refusal,
          nothing sent
  upload  Select again (a small PNG): its name and size, Publish/Update: "uploadP" with the PNG's data URL; the stub answers
          "upPR" and the popup says "Uploaded!"
Exit 0 when every check passed, 77 when it cannot run.
"""
import base64
import os
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-pfp-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    detail = str(detail)
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail[:400]) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


def attr_of(text, label, name):
    m = re.search(r"\[input_script\] attr t=\d+ @%s n=\d+ %s=(.*)" % (re.escape(label), re.escape(name)), text)
    return m.group(1).strip() if m else ""


def png(width, height, pad=0):
    """A valid PNG (grey pixels); pad: a private ancillary chunk of that many bytes, to make the file large."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    raw = b"".join(b"\x00" + b"\x80" * width for _ in range(height))
    body = chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw))
    if pad:
        body += chunk(b"knPd", os.urandom(pad))
    return b"\x89PNG\r\n\x1a\n" + body + chunk(b"IEND", b"")


def script(shots):
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "13000 respawn",
         "14400 down ESC", "14450 up ESC",
         "15000 call showWindow 5",
         "15010 waitfor [id=profilePicM]",
         "15500 attr @src src [id=profilePicM] img",
         "15510 attr @cls class [id=profilePicM]",
         "15520 text @change .pfpAction",
         "15530 shot " + shots["page"],
         "16000 clicksel .pfpAction",
         "16010 waitfor [id=pubPfp]",
         "16500 text @warn [id=pfpUpResp]",
         "16510 clicksel [id=pfpBtn]",
         "17500 text @name1 [id=pfpName]",
         "17510 clicksel [id=pubPfp]",
         "18000 text @resp1 [id=pfpUpResp]",
         "18010 shot " + shots["big"],
         "18500 clicksel [id=pfpBtn]",
         "19500 text @name2 [id=pfpName]",
         "19510 text @size2 [id=pfpSize]",
         "19520 shot " + shots["picked"],
         "19600 clicksel [id=pubPfp]",
         "21500 text @done [id=genericPop]",
         "21510 shot " + shots["done"],
         "22000 quit"]
    return "\n".join(s) + "\n"


def fake_zenity(files):
    """zenity on PATH: each run prints the next file's path (the native dialog of avatar_ui.c)."""
    bindir = os.path.join(WORK, "bin")
    os.makedirs(bindir, exist_ok=True)
    count = os.path.join(WORK, "zenity.count")
    tool = os.path.join(bindir, "zenity")
    with open(tool, "w") as f:
        f.write("#!/bin/sh\n")
        f.write("n=$(cat '%s' 2>/dev/null || echo 0)\n" % count)
        f.write("echo $((n + 1)) > '%s'\n" % count)
        for i, path in enumerate(files):
            f.write("[ \"$n\" = %d ] && echo '%s' && exit 0\n" % (i, path))
        f.write("exit 1\n")
    os.chmod(tool, os.stat(tool).st_mode | stat.S_IXUSR)
    return bindir


def run_client(stub, shots, bindir):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script(shots))
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (as kio_market_e2e.py): signed in at start, refreshed against the stub's FRVR auth
    for key, value in (("kio_refresh_token", kae.REFRESH1), ("kio_username", kae.ACCOUNT_NAME)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off", "PATH": bindir + os.pathsep + os.environ.get("PATH", ""),
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH})
    p = subprocess.Popen(["xvfb-run", "-a", "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log)


def checks(text, uploads, small, shots):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    src = attr_of(text, "src", "src")
    check("the premium account's picture is user-assets /p<id>/profile.png?v=<the hour>",
          re.fullmatch(r"https://user-assets\.krunker\.io/p%d/profile\.png\?v=\d+000" % kae.ACCOUNT_ID, src) is not None, src)
    check("not blurred (premium), with v10's \"Change Picture\"",
          attr_of(text, "cls", "class") in ("", "-") and text_of(text, "change") == "Change Picture",
          (attr_of(text, "cls", "class"), text_of(text, "change")))
    check("pfpPopup is v10's (the ban warning under Select)", text_of(text, "warn") == "Inappropriate Images will result in a ban",
          text_of(text, "warn"))
    check("a 300 kB PNG: Publish/Update says v10's 250 kb limit and sends nothing",
          text_of(text, "resp1") == "Failed. 250kb Image Limit" and text_of(text, "name1") == "big.png", (text_of(text, "name1"), text_of(text, "resp1")))
    size = text_of(text, "size2")
    check("the small PNG: updateFile's name and size in kb", text_of(text, "name2") == "small.png"
          and size.startswith("%g" % (len(small) / 1000.0)) and size.endswith("kb"), (text_of(text, "name2"), size))
    data = uploads[0][1] if uploads and len(uploads[0]) > 1 else ""
    check("UPLOAD_PROFILE_PICTURE: one \"uploadP\" with the PNG's data URL on the official socket",
          len(uploads) == 1 and data == "data:image/png;base64," + base64.b64encode(small).decode(), [str(u)[:80] for u in uploads])
    check("UPLOAD_PROFILE_RESPONSE: the \"Uploaded!\" popup", "Uploaded!" in text_of(text, "done"), text_of(text, "done"))
    check("no brain request for the picture (the gate)", "/api/account/avatar" not in text, re.findall(r".*avatar.*", text)[:3])
    check("the screenshots were taken", all(os.path.exists(p) for p in shots.values()), shots)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_pfp_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    big, small = png(64, 64, pad=300 * 1000), png(32, 32)
    paths = [os.path.join(WORK, "big.png"), os.path.join(WORK, "small.png")]
    for path, data in zip(paths, (big, small)):
        with open(path, "wb") as f:
            f.write(data)
    bindir = fake_zenity(paths)
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.players = []
    stub.account_data = [{"player_premium": 30}] * 2  # premiumT > 0: v10's pfpPopup is for premium accounts only
    uploads = []

    async def on_upload(send, raw):  # the client's UPLOAD_PROFILE_PICTURE: answered UPLOAD_PROFILE_RESPONSE
        try:
            uploads.append(kae.frame_args(raw))
        except Exception as e:  # noqa: BLE001 (a frame the decoder cannot read fails the check)
            uploads.append(["uploadP", "undecodable: %s" % e])
        await send("upPR")

    stub.frame_handlers["uploadP"] = on_upload
    shots = {k: os.path.join(WORK, k + ".png") for k in ("page", "big", "picked", "done")}
    try:
        stub.start()
        text = run_client(stub, shots, bindir)
        checks(text, uploads, small, shots)
    finally:
        stub.stop()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
