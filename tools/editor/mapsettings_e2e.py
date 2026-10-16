#!/usr/bin/env python3
"""Map settings e2e (the Essential panel's game configuration, the Environment panel and the map
keys behind them) against Krunker v10.0.2 (doc/v10/editor-social.md "Essential", "Environment"):

    python3 -I tools/editor/mapsettings_e2e.py --bin cmake-build-debug/client_bin [--egl DEVICE]

One headless `client_bin --editor new` run per case, driven by KRUNKNATIVE_EDITOR_SCRIPT (the
cfg: / map key commands of editor_gameconfig.c and the panel dumps of editor.c):

1. essential_v10_rows: the Essential panel shows v10's serverConfig rows (Hurtbox Scale in
   "Hitboxes", Player Damage Off in "Teams", Transfer Host on Leave in "Hosting", Webhook in "Other"),
   classConfig's Hurtbox Scale in every class, and the weapons 29 Boulder / 30 Compressor
   (v10 CFG:7641-8334, classes/renamed.js:2969-3102; editor_data.c from gen_editor_data.py).
2. settings_round_trip: the values set in the panel reach the export's config (settings, cls, weps)
   through the same stock-value rules as v7 (a value equal to v10's default is not written).
3. environment_round_trip: Ambient Fade Time (ambFadeTime, v10 IDX:20052), Fog Distance, Ambient
   Intensity and Minimap AssetId (mmId, text, v10 IDX:15130) round trip, and the Environment panel
   shows the edited values.

Exit status 0 = every case passed. The runs use a private XDG_CONFIG_HOME under ~/.cache.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


class Ctx:
    pass


def run_editor(c, name, script, timeout=300):
    """One client_bin --editor new run; returns the work dir (log, dumps and export are in it)."""
    work = os.path.join(c.tmp, name)
    os.makedirs(work, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(work, "cfg"),
        "KRUNKNATIVE_WINDOW_SIZE": "1920x937",
        "KRUNKNATIVE_EDITOR_SCRIPT": "closewindow;" + script,
        # the screenshot ends the run once the script is done
        "KRUNKNATIVE_SCREENSHOT": os.path.join(work, "shot.png"),
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    with open(os.path.join(work, "client.log"), "w") as log:
        subprocess.run([c.bin, "--offline", "--editor", "new"], cwd=ROOT, env=env, stdout=log,
                       stderr=subprocess.STDOUT, timeout=timeout)
    return work


def read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return f.read().splitlines()


def expect(problems, ok, what):
    if not ok:
        problems.append(what)


def has_line(lines, *parts):
    """A dump line containing every part (the dump's rows carry the value after the label)."""
    return any(all(p in ln for p in parts) for ln in lines)


# ---------------------------------------------------------------------------- case 1

def case_essential_rows(c):
    """One run: the panel rows after the cfg: writes (the dump) and the export of the same state."""
    work = os.path.join(c.tmp, "essential")
    os.makedirs(work, exist_ok=True)
    out = os.path.join(work, "left.txt")
    export = os.path.join(work, "export.json")
    # the cfg: commands write the values the panel's controls write (GUI.action)
    run_editor(c, "essential", ";".join([
        "cfg:set:settings.hurtboxScale:1.5",
        "cfg:set:settings.noPvp:true",
        "cfg:set:settings.transferHostOnLeave:false",
        "cfg:set:settings.webhook:\"https://hooks.example.test/krunk\"",
        "cfg:set:cls.3.hurtboxScale:1.4",
        "cfg:set:weps.29.spread:150",
        "cfg:set:weps.30.dmg:2",
        "wait:0.3",
        "dump:left:" + out,
        "export:" + export,
    ]))
    lines = read_lines(out)
    problems = []
    expect(problems, has_line(lines, "L[subsection] Hurtbox Scale : sval=1.5"), "Hitboxes: Hurtbox Scale shows the set 1.5")
    expect(problems, "Fs Hitboxes closed" in lines, "Hitboxes folder")
    expect(problems, has_line(lines, "L[subsection] Player Damage Off : switch=1"), "Teams: Player Damage Off is on")
    expect(problems, "Fs Hosting closed" in lines, "Hosting folder")
    expect(problems, has_line(lines, "L[subsection] Transfer Host on Leave : switch=0"), "Hosting: Transfer Host on Leave is off")
    expect(problems, has_line(lines, "L[subsection] Webhook : text()=https://hooks.example.test/krunk"), "Other: Webhook text")
    expect(problems, "Fs Boulder [ID: 29] closed" in lines, "weapon 29 Boulder folder")
    expect(problems, "Fs Compressor [ID: 30] closed" in lines, "weapon 30 Compressor folder")
    expect(problems, has_line(lines, "L[subsection] Hurtbox Scale : sval=1.4"), "class 3 Hurtbox Scale is 1.4")
    expect(problems, has_line(lines, "L[subsection] Hurtbox Scale : sval=1 "), "other classes keep Hurtbox Scale 1")
    return problems, export


def case_settings_round_trip(export):
    """The export of case_essential_rows: the panel's values reach config (settings, cls, weps)."""
    problems = []
    try:
        with open(export, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return ["export is missing or not JSON: " + export]
    cfg = doc.get("config") or {}
    settings = cfg.get("settings") or {}
    expect(problems, settings.get("hurtboxScale") == 1.5, "settings.hurtboxScale is 1.5 in the export (%r)" % settings.get("hurtboxScale"))
    expect(problems, settings.get("noPvp") == 1, "settings.noPvp is written as 1 (%r)" % settings.get("noPvp"))
    expect(problems, settings.get("transferHostOnLeave") == 0, "settings.transferHostOnLeave is written as 0 (%r)" % settings.get("transferHostOnLeave"))
    expect(problems, settings.get("webhook") == "https://hooks.example.test/krunk", "settings.webhook round trips (%r)" % settings.get("webhook"))
    cls = cfg.get("cls") or {}
    expect(problems, (cls.get("3") or {}).get("hurtboxScale") == 1.4, "cls.3.hurtboxScale is 1.4 (%r)" % (cls.get("3") or {}).get("hurtboxScale"))
    weps = cfg.get("weps") or {}
    expect(problems, (weps.get("29") or {}).get("spread") == 150, "weps.29 (Boulder) spread is 150 (%r)" % weps.get("29"))
    expect(problems, (weps.get("30") or {}).get("dmg") == 2, "weps.30 (Compressor) dmg is 2 (%r)" % weps.get("30"))
    return problems


def case_default_dropped(c):
    """A v10 default is not written: transferHostOnLeave true (its default) and noPvp false are absent."""
    problems = []
    work = run_editor(c, "defaults", ";".join([
        "cfg:set:settings.transferHostOnLeave:true",
        "cfg:set:settings.noPvp:false",
        "export:" + os.path.join(c.tmp, "defaults", "export.json"),
    ]))
    try:
        with open(os.path.join(work, "export.json"), encoding="utf-8") as f:
            settings = (json.load(f).get("config") or {}).get("settings") or {}
    except (OSError, ValueError):
        return ["defaults export is missing"]
    expect(problems, "transferHostOnLeave" not in settings, "transferHostOnLeave true (the default) is not written")
    expect(problems, "noPvp" not in settings, "noPvp false (the default) is not written")
    return problems


# ---------------------------------------------------------------------------- case 3

def case_environment(c):
    problems = []
    work = os.path.join(c.tmp, "environment")
    os.makedirs(work, exist_ok=True)
    right = os.path.join(work, "right.txt")
    export = os.path.join(work, "export.json")
    run_editor(c, "environment", ";".join([
        "cfg:set:map.ambFadeTime:2.5",
        "cfg:set:map.fogD:1234",
        "cfg:set:map.ambientI:1.5",
        "cfg:set:map.mmId:\"dt12\"",
        "panel:environment",
        "wait:0.3",
        "dump:right:" + right,
        "export:" + export,
    ]))
    lines = read_lines(right)
    expect(problems, has_line(lines, "L[] Fade Time : sval=2.5"), "Ambient: Fade Time shows 2.5")
    expect(problems, has_line(lines, "L[] Intensity : sval=1.5"), "Ambient: Intensity shows 1.5")
    expect(problems, has_line(lines, "L[subsubsection] Distance : sval=1234"), "Fog: Distance shows 1234")
    try:
        with open(export, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return problems + ["environment export is missing"]
    expect(problems, doc.get("ambFadeTime") == 2.5, "export ambFadeTime is 2.5 (%r)" % doc.get("ambFadeTime"))
    expect(problems, doc.get("fogD") == 1234, "export fogD is 1234 (%r)" % doc.get("fogD"))
    expect(problems, doc.get("ambientI") == 1.5, "export ambientI is 1.5 (%r)" % doc.get("ambientI"))
    expect(problems, doc.get("mmId") == "dt12", "export mmId is dt12 (%r)" % doc.get("mmId"))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=None, help="client_bin (default: cmake-build-debug/client_bin)")
    ap.add_argument("--egl", default=os.environ.get("KRUNKNATIVE_EGL_DEVICE"), help="KRUNKNATIVE_EGL_DEVICE")
    ap.add_argument("-k", default=None, help="run only the cases whose name contains this")
    a = ap.parse_args()
    c = Ctx()
    c.bin = os.path.abspath(a.bin or os.path.join(ROOT, "cmake-build-debug", "client_bin"))
    c.egl = a.egl
    if not os.path.isfile(os.path.join(ROOT, "assets", "css", "fonts", "font2.ttf")) or not os.access(c.bin, os.X_OK):
        print("skipped: client_bin or the game content (assets/css/fonts) missing")
        return 0
    base = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "krunknative")
    os.makedirs(base, exist_ok=True)
    c.tmp = tempfile.mkdtemp(prefix="mapsettings-e2e-", dir=base)
    failed = total = 0
    essential = None
    for name in ("essential_v10_rows", "settings_round_trip", "default_dropped", "environment_round_trip"):
        if a.k and a.k not in name:
            continue
        total += 1
        if name == "essential_v10_rows":
            problems, essential = case_essential_rows(c)
        elif name == "settings_round_trip":
            if essential is None:
                essential = case_essential_rows(c)[1]
            problems = case_settings_round_trip(essential)
        elif name == "default_dropped":
            problems = case_default_dropped(c)
        else:
            problems = case_environment(c)
        if problems:
            failed += 1
            print("FAIL %s" % name)
            for p in problems:
                print("    " + p)
        else:
            print("ok   %s" % name)
    print("%d/%d passed (work %s)" % (total - failed, total, c.tmp))
    if not failed:
        shutil.rmtree(c.tmp, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
