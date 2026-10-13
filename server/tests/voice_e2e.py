#!/usr/bin/env python3
"""Voice chat end-to-end: headless clients against a local server_bin, a generated tone as client
A's microphone (KRUNKNATIVE_VOICE_INPUT=tone:<hz>, client/src/voice.c) and every other client's
decoded clips written as WAV files (KRUNKNATIVE_VOICE_DUMP; playback on the null audio backend, so
nothing is ever played on this machine). Each scenario checks who decodes the speaker's tone
(energy at its frequency) under the settings players use:

  ptt-udp        push to talk held (V, voiceChatTrigger 1), FFA, knet over UDP
  ptt-tcp        the same over TCP
  toggle         voiceChatTrigger 0: one press starts, one press stops
  no-opus        the speaker has no Opus (KRUNKNATIVE_OPUS=0, WAV clips): still heard
  dead           the speaker talks from the death screen (/kill)
  spectator      the speaker spectates (v7 relays spectators; muteSpec on a listener drops them)
  team           TDM, team channel: the teammate hears it, the enemy does not
  volume-0       a listener with voiceVolume 0 gets nothing (and is told), the other one hears it
  mic-volume-0   micVolume 0 (the Pro preset): nobody hears it (the speaker is told)
  bad-input      audioInput set to a device that does not exist: the default microphone is used
  web            (with --web DIST) a browser speaks through net-bridge --web (the /native
                 WebSocket) and a native client hears it, then the native client speaks back
                 (Chromium's fake microphone plays a generated 440 Hz WAV)
  web-rtc        the same through the kn-rtc WebRTC gateway (needs go)

  python3 -I server/tests/voice_e2e.py BUILD_DIR [--web DIST] [scenario ...]   (from the repository root)

Ports come from kn_proto.free_ports(); every process this starts is killed when its scenario ends.
One scenario at a time. Exit 0 only when every check passed.
"""
import glob
import json
import math
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

ARGS = sys.argv[1:]
BUILD = ARGS.pop(0) if ARGS and not ARGS[0].startswith("-") else "cmake-build-debug"
WEB = None
if "--web" in ARGS:
    i = ARGS.index("--web")
    WEB = os.path.abspath(ARGS[i + 1])
    del ARGS[i:i + 2]
ONLY = set(ARGS)
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-voice-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
TONE = 440.0
results = []
procs = []


def check(scenario, name, ok, detail=""):
    results.append((scenario, name, bool(ok)))
    print(f"{'PASS' if ok else 'FAIL'}  {scenario}: {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def kill_all():
    while procs:
        p = procs.pop()
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def spawn(cmd, log, env=None, cwd=ROOT):
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return p


def start_server(name, extra=()):
    (port,) = free_ports(1)
    log = os.path.join(WORK, f"{name}-server.log")
    spawn(["timeout", "120", f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port), *extra], log)
    end = time.time() + 10
    while time.time() < end and "Listening on UDP" not in read(log):
        time.sleep(0.1)
    return port


def tone_ratio(samples, rate, hz):
    """Share of the signal's energy within 5% of hz (a plain DFT over a Hann window, no numpy)."""
    n = len(samples)
    if n < 64:
        return 0.0
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)) for i in range(n)]
    x = [s * w for s, w in zip(samples, win)]
    total = sum(v * v for v in x) * n / 2 or 1e-12

    def power(f):
        k = 2 * math.pi * f / rate
        re = sum(v * math.cos(k * i) for i, v in enumerate(x))
        im = sum(v * math.sin(k * i) for i, v in enumerate(x))
        return re * re + im * im

    step = max(1.0, rate / n)  # one DFT bin
    band = [hz * 0.95 + j * step for j in range(int(hz * 0.1 / step) + 1)]
    return min(1.0, sum(power(f) for f in band) / total)


def read_wav(path, max_seconds=1.0, skip_seconds=0.0):
    """Mono float samples (first channel) and the rate; at most max_seconds after skip_seconds."""
    with wave.open(path) as w:
        rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        w.setpos(min(w.getnframes(), int(skip_seconds * rate)))
        raw = w.readframes(int(max_seconds * rate))
    if width != 2:
        return [], rate
    vals = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return [v / 32768.0 for v in vals[::ch]], rate


def heard(dump_dir, hz=TONE):
    """The decoded clips a listener wrote: (count, best tone ratio, rms of the best clip)."""
    clips = sorted(glob.glob(os.path.join(dump_dir, "vc-*.wav")))
    best, best_rms = 0.0, 0.0
    for path in clips[:6]:
        samples, rate = read_wav(path, 0.25)
        # decimate for the pure-python DFT: 48 kHz -> 12 kHz is plenty for a 440 Hz tone
        if rate >= 24000:
            f = rate // 12000
            samples, rate = samples[::f], rate // f
        r = tone_ratio(samples, rate, hz)
        rms = math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0.0
        if r > best:
            best, best_rms = r, rms
    return len(clips), best, best_rms


def client_env(name, storage=None, extra=None):
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in (storage or {}).items():
        with open(os.path.join(cfg, "krunknative", f"{key}.json"), "w") as f:
            f.write(value)
    dump = os.path.join(WORK, f"{name}-dump")
    os.makedirs(dump, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": cfg, "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_FRAME_CAP": "30",
                # never a real sound device: playback mixes on miniaudio's null device, captured to a file
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_AUDIO_CAPTURE": os.path.join(WORK, f"{name}-out.wav"),
                "KRUNKNATIVE_VOICE_LOG": "1", "KRUNKNATIVE_VOICE_DUMP": dump, "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KN_TEST_EGL", "mesa")})
    env.update(extra or {})
    return env, dump


def run_clients(scenario, port, specs, seconds=26):
    """specs: [(name, script, storage, env_extra)]. Runs them together; returns {name: (log, dump)}."""
    out = {}
    for name, script, storage, extra in specs:
        env, dump = client_env(f"{scenario}-{name}", storage, extra)
        script_path = os.path.join(WORK, f"{scenario}-{name}.script")
        with open(script_path, "w") as f:
            f.write("\n".join(line.strip() for line in script.strip().splitlines()) + "\n")
        env["KRUNKNATIVE_INPUT_SCRIPT"] = script_path
        log = os.path.join(WORK, f"{scenario}-{name}.log")
        spawn(["timeout", str(seconds + 10), "stdbuf", "-oL", f"{BUILD}/client_bin", "--connect", f"127.0.0.1:{port}"], log, env)
        out[name] = (log, dump)
        time.sleep(1.5)  # join in order: A gets the lowest uid
    end = time.time() + seconds + 12
    while time.time() < end and any(p.poll() is None for p in procs if "client_bin" in " ".join(p.args)):
        time.sleep(0.25)
    return out


def sent_clips(log):
    return sum(1 for line in read(log).splitlines() if "[voice]" in line and " send clip " in line)


# A speaker script: play, wait for the spawn, then hold V (push to talk) for `talk` ms from `at`.
def talk_script(at=2500, talk=2600, pre="", post=""):
    return f"""
        0 play
        {pre}
        {at} down V
        {at + talk} up V
        {post}
        {at + talk + 6000} quit
    """


LISTEN = """
    0 play
    16000 quit
"""


def expect_heard(scenario, who, dump, want=True, min_ratio=0.5):
    n, ratio, rms = heard(dump)
    if want:
        check(scenario, f"{who} decodes the speaker's {TONE:g} Hz tone", n > 0 and ratio >= min_ratio and rms > 0.02,
              f"{n} clips, tone share {ratio:.2f}, rms {rms:.3f}")
    else:
        check(scenario, f"{who} receives nothing", n == 0, f"{n} clips")
    return n, ratio


def expect_played(scenario, who, name):
    """The listener's mixed audio output (its null device, KRUNKNATIVE_AUDIO_CAPTURE) has the tone."""
    path = os.path.join(WORK, f"{name}-out.wav")
    best = 0.0
    try:
        with wave.open(path) as w:
            seconds = w.getnframes() / float(w.getframerate() or 1)
        for t in range(1, int(seconds)):
            samples, rate = read_wav(path, 0.25, t)
            f = max(1, rate // 12000)
            best = max(best, tone_ratio(samples[::f], rate // f, TONE))
    except (OSError, wave.Error, EOFError):
        pass
    check(scenario, f"{who} plays the tone through its audio output", best >= 0.3, f"output tone share {best:.2f}")


TONE_IN = {"KRUNKNATIVE_VOICE_INPUT": f"tone:{TONE:g}"}


def basic(scenario, transport, speaker_extra=None, speaker_storage=None, played=False):
    port = start_server(scenario)
    env = dict(TONE_IN, KRUNKNATIVE_NET_TRANSPORT=transport, **(speaker_extra or {}))
    logs = run_clients(scenario, port, [("A", talk_script(), speaker_storage, env),
                                        ("B", LISTEN, None, {"KRUNKNATIVE_NET_TRANSPORT": transport})])
    sent = sent_clips(logs["A"][0])
    check(scenario, "the speaker sends clips while V is held", sent >= 4, f"{sent} clips")
    expect_heard(scenario, "the listener", logs["B"][1])
    n_self, _, _ = heard(logs["A"][1])
    check(scenario, "no echo to the speaker", n_self == 0, f"{n_self} clips")
    if played:
        expect_played(scenario, "the listener", f"{scenario}-B")
    return logs


def sc_ptt_udp():
    logs = basic("ptt-udp", "udp", played=True)
    check("ptt-udp", "both sessions over UDP", all("udp session established" in read(l) for l, _ in logs.values()))


def sc_ptt_tcp():
    basic("ptt-tcp", "tcp")


def sc_no_opus():
    logs = basic("no-opus", "udp", speaker_extra={"KRUNKNATIVE_OPUS": "0"})
    check("no-opus", "the speaker sent WAV clips", glob.glob(os.path.join(logs["A"][1], "sent-*.wav")) != [])


def sc_toggle():
    scenario = "toggle"
    port = start_server(scenario)
    script = """
        0 play
        2500 down V
        2600 up V
        5000 down V
        5100 up V
        11000 quit
    """
    logs = run_clients(scenario, port, [("A", script, {"knSettings": json.dumps({"voiceChatTrigger": 0})}, TONE_IN), ("B", LISTEN, None, None)])
    sent = sent_clips(logs["A"][0])
    check(scenario, "one press records until the next press (~2.5 s: 4-6 clips)", 4 <= sent <= 6, f"{sent} clips")
    expect_heard(scenario, "the listener", logs["B"][1])


def sc_dead():
    scenario = "dead"
    port = start_server(scenario, ["--open-cheats"])
    script = talk_script(at=4000, pre="2000 con say /kill\n3800 vis @dead deathUIHolder")
    logs = run_clients(scenario, port, [("A", script, None, TONE_IN), ("B", LISTEN, None, None)])
    check(scenario, "the speaker is on the death screen while talking", "@dead deathUIHolder=1" in read(logs["A"][0]))
    sent = sent_clips(logs["A"][0])
    check(scenario, "a dead speaker sends clips", sent >= 4, f"{sent} clips")
    expect_heard(scenario, "the listener", logs["B"][1])


def sc_spectator():
    scenario = "spectator"
    port = start_server(scenario)
    spect = """
        0 menu
        1000 clickid specBtn
        1500 play
        3500 down V
        6100 up V
        12000 quit
    """
    logs = run_clients(scenario, port, [("A", spect, None, TONE_IN), ("B", LISTEN, None, None),
                                        ("C", LISTEN, {"knSettings": json.dumps({"muteSpec": True})}, None)])
    sent = sent_clips(logs["A"][0])
    check(scenario, "the spectator sends clips", sent >= 4, f"{sent} clips")
    expect_heard(scenario, "a player", logs["B"][1])
    expect_heard(scenario, "a player with Mute Spectators", logs["C"][1], want=False)


def sc_team():
    scenario = "team"
    port = start_server(scenario, ["--mode", "tdm"])
    # A and C red, B blue; A and C switch the text & voice channel to Team (#chatSwitch)
    a = talk_script(at=9000, pre="1500 con say /team red\n4500 call switchChat")
    b = "0 play\n1500 con say /team blue\n20000 quit"
    c = "0 play\n1500 con say /team red\n4500 call switchChat\n20000 quit"
    logs = run_clients(scenario, port, [("A", a, None, TONE_IN), ("B", b, None, None), ("C", c, None, None)])
    sent = sent_clips(logs["A"][0])
    check(scenario, "the speaker sends team clips", sent >= 4 and "ch=1" not in read(logs["A"][0]), f"{sent} clips")
    expect_heard(scenario, "the teammate", logs["C"][1])
    expect_heard(scenario, "the enemy", logs["B"][1], want=False)


def sc_volume_0():
    scenario = "volume-0"
    port = start_server(scenario)
    logs = run_clients(scenario, port, [("A", talk_script(), None, TONE_IN), ("B", LISTEN, None, None),
                                        ("C", LISTEN, {"knSettings": json.dumps({"voiceVolume": 0})}, None)])
    expect_heard(scenario, "the listener", logs["B"][1])
    expect_heard(scenario, "the listener with voiceVolume 0", logs["C"][1], want=False)
    check(scenario, "that listener is told why", "Voice Chat volume is 0" in read(logs["C"][0]))


def sc_mic_volume_0():
    scenario = "mic-volume-0"
    port = start_server(scenario)
    logs = run_clients(scenario, port, [("A", talk_script(), {"knSettings": json.dumps({"micVolume": 0})}, TONE_IN),
                                        ("B", LISTEN, None, None)])
    expect_heard(scenario, "the listener", logs["B"][1], want=False)
    check(scenario, "the speaker is told why", "Mic Volume setting is 0" in read(logs["A"][0]))


def sc_bad_input():
    scenario = "bad-input"
    port = start_server(scenario)
    # no test tone: the saved input does not exist, so the default device (the null backend's
    # silent capture device) is used; the clips go out and the silent microphone is reported
    logs = run_clients(scenario, port, [("A", talk_script(), {"knSettings": json.dumps({"audioInput": "No Such Microphone"})}, None),
                                        ("B", LISTEN, None, None)])
    log = read(logs["A"][0])
    check(scenario, "falls back to the default microphone", "not found, using the default microphone" in log)
    check(scenario, "clips are sent", sent_clips(logs["A"][0]) >= 4)
    check(scenario, "the silent microphone is reported", "microphone is silent" in log or "delivers no audio" in log)
    n, _, _ = heard(logs["B"][1])
    check(scenario, "the listener gets the (silent) clips", n >= 3, f"{n} clips")


def write_tone_wav(path, seconds=6.0, rate=48000):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(9000 * math.sin(2 * math.pi * TONE * i / rate))) for i in range(int(seconds * rate))))


def sc_web(scenario="web", transport="ws"):
    """A browser speaks through net-bridge --web (the /native WebSocket) or the kn-rtc WebRTC gateway,
    a native client listens and then speaks back."""
    chrome = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
    modules = os.path.join(ROOT, "net-bridge", "node_modules")
    if not WEB or not chrome or not shutil.which("node") or not os.path.isdir(modules) or not os.path.isfile(os.path.join(WEB, "client.wasm")):
        print("SKIP  web: needs --web DIST (a web build), node, net-bridge/node_modules and Chromium", flush=True)
        return
    if transport == "rtc" and not shutil.which("go"):
        print("SKIP  web-rtc: needs go (the kn-rtc gateway is built into the scratch dir)", flush=True)
        return
    port = start_server(scenario)
    http, cdp, ice, sig = free_ports(4)
    bridge_log = os.path.join(WORK, f"{scenario}-bridge.log")
    rtc = ""
    if transport == "rtc":
        gw = os.path.join(WORK, "kn-rtc")
        if not os.path.isfile(gw):
            subprocess.run(["go", "build", "-trimpath", "-buildvcs=false", "-o", gw, "."], cwd=os.path.join(ROOT, "net-bridge", "rtc"),
                           check=True, env=dict(os.environ, GOTOOLCHAIN="local", CGO_ENABLED="0"))
        rtc_log = os.path.join(WORK, f"{scenario}-rtc.log")
        spawn(["timeout", "120", gw, "-http", "127.0.0.1:%d" % sig, "-ice-addr", ":%d" % ice], rtc_log, dict(os.environ, KRUNK_NATIVE_PORT=str(port)))
        end = time.time() + 10
        while time.time() < end and "[rtc] signaling" not in read(rtc_log):
            time.sleep(0.1)
        rtc = "&rtc=http://127.0.0.1:%d/rtc" % sig
    spawn(["timeout", "120", "node", "-r", "ts-node/register/transpile-only", "index.ts", "--web"], bridge_log,
          dict(os.environ, KRUNK_NATIVE_PORT=str(port), KRUNK_NATIVE_BRIDGE_PORT=str(http), KRUNK_NATIVE_WEB_DIR=WEB),
          cwd=os.path.join(ROOT, "net-bridge"))
    end = time.time() + 30
    while time.time() < end and "web listening on" not in read(bridge_log):
        time.sleep(0.2)
    tone = os.path.join(WORK, f"{scenario}-tone.wav")
    write_tone_wav(tone)
    spawn(["timeout", "120", chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
           "--enable-unsafe-swiftshader", "--user-data-dir=" + os.path.join(WORK, f"{scenario}-chrome"), "--remote-debugging-port=%d" % cdp,
           "--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--use-file-for-fake-audio-capture=" + tone,
           "--autoplay-policy=no-user-gesture-required", "--mute-audio", "about:blank"], os.path.join(WORK, f"{scenario}-chrome.log"))
    env, dump = client_env(f"{scenario}-B")
    script = os.path.join(WORK, f"{scenario}-B.script")
    with open(script, "w") as f:
        f.write("0 play\n9000 down V\n11600 up V\n40000 quit\n")
    env["KRUNKNATIVE_INPUT_SCRIPT"] = script
    env.update(TONE_IN)
    listener = spawn(["timeout", "60", "stdbuf", "-oL", f"{BUILD}/client_bin", "--connect", f"127.0.0.1:{port}"],
                     os.path.join(WORK, f"{scenario}-B.log"), env)
    url = "http://127.0.0.1:%d/?ws=ws://127.0.0.1:%d/native&transport=%s&voicelog=1%s" % (http, http, transport, rtc)
    drv = subprocess.run(["timeout", "100", "node", "tools/kref/voice-web.js", str(cdp), url, "3000", "60", "12000"], cwd=ROOT,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        report = json.loads(drv.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        check(scenario, "driver report", False, drv.stdout[-800:])
        return
    log = "\n".join(report["console"])
    if transport == "rtc":
        rtc_log = read(os.path.join(WORK, f"{scenario}-rtc.log"))
        opened = re.search(r"\[rtc\] #\d+ .* -> udp 127\.0\.0\.1:%d; open in" % port, rtc_log)
        check(scenario, "the browser's session runs through kn-rtc (WebRTC data channel)", opened, "" if opened else rtc_log[-300:])
    check(scenario, "the browser got in game", report["inGame"], "" if report["inGame"] else log[-400:])
    sends = log.count("send clip")
    check(scenario, "the browser captured its fake microphone and sent clips", sends >= 4, f"{sends} clips" + ("" if sends >= 4 else "; " + log[-500:]))
    recv = sum(1 for line in report["console"] if "[voice]" in line and " play " in line)
    check(scenario, "the browser decoded and played the native speaker's clips", recv >= 3, f"{recv} clips played")
    check(scenario, "the browser's mic icon was on", any("voiceOn" in i for i in report["icons"]), str(report["icons"][:3]))
    time.sleep(3)
    listener.kill()
    expect_heard(scenario, "the native listener", dump)


SCENARIOS = [("ptt-udp", sc_ptt_udp), ("ptt-tcp", sc_ptt_tcp), ("toggle", sc_toggle), ("no-opus", sc_no_opus), ("dead", sc_dead),
             ("spectator", sc_spectator), ("team", sc_team), ("volume-0", sc_volume_0), ("mic-volume-0", sc_mic_volume_0),
             ("bad-input", sc_bad_input), ("web", sc_web), ("web-rtc", lambda: sc_web("web-rtc", "rtc"))]


def main():
    if not os.path.isfile(f"{BUILD}/client_bin") or not os.path.isfile(f"{BUILD}/server_bin"):
        print(f"voice_e2e: no client_bin / server_bin in {BUILD}")
        return 2
    try:
        for name, fn in SCENARIOS:
            if ONLY and name not in ONLY:
                continue
            print(f"== {name}", flush=True)
            try:
                fn()
            finally:
                kill_all()  # every process of the scenario, also on failure
    finally:
        kill_all()
    failed = [r for r in results if not r[2]]
    print(f"\nvoice_e2e: {len(results) - len(failed)}/{len(results)} checks passed" + (f", logs in {WORK}" if failed else ""))
    if not failed:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
