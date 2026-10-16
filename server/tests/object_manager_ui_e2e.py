#!/usr/bin/env python3
"""KrunkNative Object Manager window (F1, client/src/ui/object_manager_ui.c) in the headless client.

  python3 -I server/tests/object_manager_ui_e2e.py BUILD_DIR      (run from the repository root)

Offline (the sandbox server, Kanji), one client_bin run per scenario (KRUNKNATIVE_HEADLESS=1, a private
XDG_CONFIG_HOME). Prints PASS/FAIL lines; exit code 0 only when everything passed.

  wrap   the Entities tab, searched for "western": the one tile left, "Perk: Western Hands" (a name
         wider than its tile), wraps onto two lines inside the tile (.knOmName is a box of 2 lines,
         no ellipsis): the `rects` probe (input_script.c) gives the name box of each tile.
  perf   the same window under KRUNKNATIVE_HTML_PROFILE=1 (html_engine profile lines, one per 2 s
         window), one run per interaction: hover (the cursor over the tiles), scroll (wheel steps), tabs
         (the four tab buttons) and search (typing). The first window (it holds the boot) is skipped; the
         rest must stay inside PERF_BUDGET (the largest layout time, the most subtree rebuilds). The
         times are this host's (llvmpipe, loaded): they are printed too.
  The empty line (#knOmEmpty) is hidden while a tile matches the search (`vis` probe).

Budgets are chosen with headroom over the fixed build's numbers on llvmpipe; see PERF_BUDGET.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
WORK = tempfile.mkdtemp(prefix="kn-omui-e2e-", dir=os.path.expanduser("~/.cache"))

# per interaction: the largest layout time (ms) of any profile window, the most layouts that
# rebuilt a subtree in one window and the most elements one window restyled. om-perf-2 (debug build,
# NVIDIA, this host loaded), origin/main d11be9a6 -> with the fixes (scrollbar width cached per
# restyle, word-at-a-time style / paint hashes, a shown subtree restyles only what changed):
#   hover   layout max 0.3 -> 0.3 ms          scroll  layout max 0.9 -> 0.6 ms
#   tabs    layout max 97.7 -> 17.7 ms, restyle 990 -> 388 elements, 26 rebuilds either way
#   search  layout max 4.7 -> 3.9 ms, 9 rebuilds
# The budgets are regression guards with headroom over the fixed build on a loaded machine.
PERF_BUDGET = {
    "hover": {"layout_max": 10.0, "rebuilds": 4, "restyle": 400},
    "scroll": {"layout_max": 10.0, "rebuilds": 4, "restyle": 400},
    "tabs": {"layout_max": 45.0, "rebuilds": 40, "restyle": 650},
    "search": {"layout_max": 15.0, "rebuilds": 20, "restyle": 400},
}

results = []


def record(scenario, label, want, got, ok):
    results.append((scenario, label, want, got, bool(ok)))


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_client(name, script, env_extra=None, timeout=240):
    """client_bin --offline (Kanji) with an input script; returns its log lines, None on a timeout."""
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_UPDATES_URL": "off"})
    env.update(env_extra or {})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        record(name, "finished", "exit before the timeout", "timeout", False)
        return None
    return read(log).splitlines()


def name_boxes(lines, label):
    """the visible .knOmName boxes (x, y, w, h) of the last `rects @label .knOmName` probe"""
    rects = []
    for line in lines:
        m = re.search(rf"\[input_script\] rects t=\d+ @{label} n=\d+(.*)", line)
        if m:
            rects = m.group(1).split()
    boxes = []
    for r in rects:
        if "@" not in r or r.split("@", 1)[1] == "0.0,0.0,0.0,0.0":
            continue  # a tile of a grid that is not shown (display: none: an empty box)
        x, y, w, h = (float(v) for v in r.split("@", 1)[1].split(","))
        boxes.append((x, y, w, h))
    return boxes


def sc_wrap():
    """Entities tab, searched: a one-word name ("Nuke") is the reference for one line, then "western"
    ("Perk: Western Hands", wider than its tile) must be taller than that line: it wraps."""
    heights = {}
    for word in ("nuke", "western"):
        lines = run_client(f"wrap-{word}", f"""
            0 play
            1000 down F1
            1050 up F1
            2000 vis @w windowHolder knOmGrid3
            2100 clickid knOmTab1
            2600 clickid knOmSearchBox
            3000 type {word}
            3800 rects @w .knOmName
            3900 vis @hidden knOmEmpty1 knOmGrid1
            4000 quit
        """)
        if lines is None:
            return
        boxes = name_boxes(lines, "w")
        record(f"wrap-{word}", "the search leaves one name", "1 visible name box", len(boxes), len(boxes) == 1)
        if boxes:
            heights[word] = boxes[0]
        vis = [line for line in lines if "[input_script] vis" in line and "@hidden" in line]
        record(f"wrap-{word}", "the empty line is hidden while a tile matches", "knOmEmpty1=0",
               vis[-1].split("knOmEmpty1=")[-1].split()[0] if vis and "knOmEmpty1=" in vis[-1] else "no probe",
               bool(vis) and "knOmEmpty1=0" in vis[-1])
    if "nuke" in heights and "western" in heights:
        line_h = heights["nuke"][3]
        x, y, w, h = heights["western"]
        # the two-line name is about twice the one-line name's box; the one-line box is what a
        # single unwrapped line takes (the same font and line height)
        record("wrap", "the long name wraps onto two lines", f"height >= 1.7 x the one-line name ({line_h:.1f} px)",
               f"{h:.1f} px", h >= 1.7 * line_h)
        record("wrap", "the wrapped name stays inside its tile", "width <= 136 px (a tile)", f"{w:.1f} px", w <= 136.0)


def profile_windows(lines):
    """[(frames, frame_avg, frame_max, layout_avg, layout_max, rebuilds, restyle_elems)] per profile window
    that closes after the window was opened ("[object-manager] action 0" logged), the first such one
    dropped: it holds the opening restyle (~560 elements), which a slow boot used to land in a measured
    window (perf-hover / perf-scroll failing with 8-30 rebuilds on a loaded machine)"""
    opened = next((i for i, l in enumerate(lines) if "[object-manager] action" in l), None)
    lines = lines[opened + 1:] if opened is not None else lines
    out = []
    for line in lines:
        if "[html_engine profile]" not in line:
            continue
        frames = re.search(r"(\d+) frames", line)
        frame = re.search(r"frame ([\d.]+)/([\d.]+) ms", line)
        layout = re.search(r"layout ([\d.]+)/([\d.]+) ms \((\d+)\) \((\d+) rebuilds\)", line)
        restyle = re.search(r"restyle [\d.]+/[\d.]+ ms \((\d+)\) \[(\d+) elems\]", line)
        if not (frames and frame and layout):
            continue
        out.append({"frames": int(frames.group(1)), "frame_avg": float(frame.group(1)), "frame_max": float(frame.group(2)),
                    "layout_avg": float(layout.group(1)), "layout_max": float(layout.group(2)),
                    "rebuilds": int(layout.group(4)), "restyle": int(restyle.group(2)) if restyle else 0})
    return out


PERF_SCRIPTS = {
    "hover": ("".join(f"{3000 + 40 * i} cursor {330 + (i * 37) % 560} {300 + (i * 53) % 220}\n" for i in range(120))),
    "scroll": ("".join(f"{3000 + 80 * i} wheel {-1 if i < 20 else 1}\n" for i in range(40))),
    "tabs": ("".join(f"{3000 + 250 * i} clickid knOmTab{i % 4}\n" for i in range(24))),
    "search": ("3000 clickid knOmSearchBox\n" + "".join(f"{3200 + 250 * i} type {ch}\n" for i, ch in enumerate("rifle"))
               + "".join(f"{4700 + 250 * i} type {ch}\n" for i, ch in enumerate("xyz"))),
}


def sc_perf():
    for phase, body in PERF_SCRIPTS.items():
        # the first two seconds with the window open are the pointer and the first grid: the same for all
        end = 3000 + 6000
        script = "0 play\n1000 down F1\n1050 up F1\n2000 vis @om windowHolder knOmGrid3\n" + body + f"{end} quit\n"
        lines = run_client(f"perf-{phase}", script, env_extra={"KRUNKNATIVE_HTML_PROFILE": "1"})
        if lines is None:
            continue
        windows = profile_windows(lines)[1:]  # the first window after the open holds the open
        busy = [w for w in windows if w["frames"]]
        if not busy:
            record(f"perf-{phase}", "profile windows", "some", 0, False)
            continue
        layout_max = max(w["layout_max"] for w in busy)
        frame_max = max(w["frame_max"] for w in busy)
        rebuilds = max(w["rebuilds"] for w in busy)
        budget = PERF_BUDGET[phase]
        print(f"  perf {phase}: windows={len(busy)} frame_max={frame_max:.1f} ms layout_max={layout_max:.1f} ms "
              f"layout_rebuilds_per_window_max={rebuilds} restyle_elems_max={max(w['restyle'] for w in busy)}", flush=True)
        record(f"perf-{phase}", "layout spike", f"<= {budget['layout_max']:g} ms", f"{layout_max:.1f} ms", layout_max <= budget["layout_max"])
        record(f"perf-{phase}", "subtree rebuilds per 2 s", f"<= {budget['rebuilds']}", rebuilds, rebuilds <= budget["rebuilds"])
        restyled = max(w["restyle"] for w in busy)
        record(f"perf-{phase}", "elements restyled per 2 s", f"<= {budget['restyle']}", restyled, restyled <= budget["restyle"])


def main():
    scenarios = [sc_wrap, sc_perf]
    for sc in scenarios:
        sc()
    failed = 0
    for scenario, label, want, got, ok in results:
        print(("PASS" if ok else "FAIL") + f"  {scenario}: {label}: want {want}, got {got}", flush=True)
        failed += 0 if ok else 1
    print(f"{len(results) - failed}/{len(results)} passed", flush=True)
    return 1 if failed or not results else 0


if __name__ == "__main__":
    sys.exit(main())
