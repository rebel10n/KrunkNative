#!/usr/bin/env python3
"""Krunker Strike's round-end victory banner animates before the lean screen (client/src/ui/strike_hud.c).

  python3 -I server/tests/strike_banner_anim_e2e.py BUILD_DIR      (run from the repository root)

FRVR's banner (.frvr-in-game-ui__victory-screen: "#1 VICTORY" / "MATCH OVER" / VICTORY / DEFEAT / DRAW)
comes up at the END with the stylesheet's fade-in (opacity 0 -> 1 and scale 5 -> 1 over .5 s), holds for
the 2.5 s endAnim, and then LEAN_END_SCREEN replaces it. A live offline Strike client (a bot round and
`con end`, KRUNKNATIVE_FIXED_DELTA 16.667) is sampled every 100 ms of script time for the banner, the
banner's large title (its client rect) and the lean screen's header. Checks:
  - the banner comes up, and the lean screen is not up while it shows;
  - its first sample is at least twice as wide as the settled title (the zoom starts large);
  - the title settles: its last samples agree within 1%;
  - the lean screen comes after the banner has gone.
Prints PASS/FAIL lines; exit 0 only when everything passed. Needs a headless GL device.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-banner-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

START, STEP, END = 7300, 100, 16000 # input scripts hold 512 events: 3 probes per sample stays under it


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def script():
    lines = ["menu", "1500 nodes @page #frvrMenuMobile", "4000 con bot 2", "4500 con end"]
    for t in range(START, END, STEP):
        lines.append(f"{t} nodes @b{t} .frvr-in-game-ui__victory-screen")
        lines.append(f"{t} rects @t{t} .frvr-in-game-ui__victory-text--large")
        lines.append(f"{t} nodes @l{t} .lean-end-screen__header")
    lines.append(f"{END} quit")
    return "\n".join(lines) + "\n"


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    path = os.path.join(WORK, "banner.txt")
    with open(path, "w") as f:
        f.write(script())
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    cfg = os.path.join(WORK, "cfg")
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_ASSETS_DIR": os.path.join(ROOT, "assets")})
    os.makedirs(cfg, exist_ok=True)
    with open(log, "w") as out:
        rc = subprocess.run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], cwd=ROOT, env=env, stdout=out,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=600).returncode
    with open(log, errors="replace") as f:
        text = f.read()
    check("the live Strike client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}, log {log}")
    check("the banner was drawn (\"[strike] end banner\")", "[strike] end banner" in text)

    banner, width, lean = {}, {}, {}
    for m in re.finditer(r"\[input_script\] nodes t=(\d+) @b\d+ elements=\d+ match=(\d+)", text):
        banner[int(m.group(1))] = int(m.group(2))
    for m in re.finditer(r"\[input_script\] nodes t=(\d+) @l\d+ elements=\d+ match=(\d+)", text):
        lean[int(m.group(1))] = int(m.group(2))
    for m in re.finditer(r"\[input_script\] rects t=(\d+) @t\d+ n=(\d+) ([^\n]*)", text):
        cells = re.findall(r"@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)", m.group(3))
        if int(m.group(2)) and cells:
            width[int(m.group(1))] = float(cells[0][2])
    up = sorted(t for t, n in banner.items() if n)
    check("the banner is up for some samples", len(up) >= 3, f"{len(up)} samples")
    if len(up) < 3:
        return finish(WORK)
    # the first banner (the bot round's END): the samples from its first to the first gap (a round after the lean screen repeats it)
    episode = [up[0]]
    for t in up[1:]:
        if t - episode[-1] > 2 * STEP:
            break
        episode.append(t)
    first, last = episode[0], episode[-1]
    check("the lean screen is not up while the banner is", all(lean.get(t, 0) == 0 for t in episode), "")
    widths = [(t, width[t]) for t in episode if t in width]
    check("the title is measured while the banner is up", len(widths) >= 3, f"{len(widths)} samples")
    if len(widths) < 3:
        return finish(WORK)
    settled = [w for _, w in widths[-3:]]
    settled_w = settled[-1]
    check("the title settles: its last three samples agree within 1%",
          max(settled) - min(settled) <= 0.01 * settled_w, " ".join(f"{w:.1f}" for w in settled))
    start_w = widths[0][1]
    check("the zoom starts large: the first sample is at least twice the settled width",
          start_w >= 2.0 * settled_w, f"first {start_w:.1f}px, settled {settled_w:.1f}px")
    later = [t for t, n in lean.items() if n and t > last]
    check("the lean screen comes after the banner has gone", bool(later) and banner.get(min(later), 0) == 0,
          f"banner last at {last} ms, lean first at {min(later) if later else 'never'} ms")
    return finish(WORK)


def finish(work):
    ok = all(results)
    print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs in {work})"))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
