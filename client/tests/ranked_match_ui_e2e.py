#!/usr/bin/env python3
"""The in-match UI of a ranked lobby on the Krunker v10.0.2 page (client/src/ui/hud_ui.c hud_ui_match_gate,
comp_ui.c comp_ui_match_gate / comp_ui_box, v10_menu.c v10_menu_match_gate), end to end.

  python3 -I client/tests/ranked_match_ui_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin --offline plays a match in a comp lobby (the input script's `ranked` command:
the server's NET_EXT_KN_RANKED_MATCH through the client's own packet path, game_events.c). v10 (g:219154-
219156, g:219431) for a ranked lobby:
  - the comp menu (#mMenuHolComp) is up, its box titled "Krunker Ranked" over the "NvN on <map>" line;
  - the Invite button (#inviteButton) is greyed (buttonD, no pointer events);
  - the spectate button (#cmpSpectBtn) is hidden;
  - the Host card (#menuBtnHost) is not greyed: v10's last toggle sets buttonD from isOfficial only.
Then `ranked 0 1` (a comp lobby that is not ranked): "Krunker Competitive", Invite live, spectate shown.
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
WORK = tempfile.mkdtemp(prefix="kn-ranked-ui-e2e-", dir=os.path.expanduser("~/.cache"))
results = []

SCRIPT = """
0 menu
1000 check @menu menuHolder=1
1500 play
3000 ranked 1 1
3500 style @comp mMenuHolComp:display
3500 text @title-ranked [id="cmpDatBox"]
3500 nodes @invite-grey [id="inviteButton"].buttonD
3500 nodes @host-grey [id="menuBtnHost"].buttonD
3500 style @spect cmpSpectBtn:display
5000 ranked 0 1
5500 style @comp-plain mMenuHolComp:display
5500 text @title-plain [id="cmpDatBox"]
5500 nodes @invite-plain [id="inviteButton"].buttonD
5500 nodes @host-plain [id="menuBtnHost"].buttonD
5500 style @spect-plain cmpSpectBtn:display
6000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run():
    script = os.path.join(WORK, "ranked-ui.script")
    lines = []
    for raw in SCRIPT.strip().splitlines():
        t, cmd, *rest = raw.split()
        if cmd == "check":
            lines.append(f"{t} vis @{rest[0].lstrip('@')} " + " ".join(kv.split("=", 1)[0] for kv in rest[1:]))
        else:
            lines.append(raw)
    with open(script, "w") as f:
        f.write("\n".join(lines) + "\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_FIXED_DELTA": "16.6667",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": os.path.join(REPO, "assets")})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    check("the v10 page loaded", "[input_script] vis" in text, log)
    check("the ranked flag was delivered twice (ranked, then not)", text.count("[input_script] ranked ") == 2, log)

    got, nodes, texts = {}, {}, {}
    for line in text.splitlines():
        m = re.search(r"\[input_script\] (?:vis|style) t=\S+ @(\S+)(.*)", line)
        if m:
            have = got.setdefault(m.group(1), {})
            for kv in m.group(2).split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    have[k] = v
        m = re.search(r"\[input_script\] nodes t=\S+ @?(\S+) elements=\d+ match=(\d+)", line)
        if m:
            nodes[m.group(1)] = int(m.group(2))
        m = re.search(r"\[input_script\] text t=\S+ @?(\S+) n=(\d+) checked=\d+ value=.*? text=(.*)$", line)
        if m:
            texts[m.group(1)] = m.group(3)

    check("the comp menu is up in a comp lobby", got.get("comp", {}).get("mMenuHolComp:display") == "block", str(got.get("comp")))
    title = texts.get("title-ranked", "")
    check("the comp box is titled Krunker Ranked over the NvN line (v10 g:219431)",
          re.match(r"Krunker Ranked\s*\d+v\d+ on \S", title or "") is not None, repr(title))
    check("the Invite button is greyed in a ranked lobby (v10 g:219154)", nodes.get("invite-grey") == 1, str(nodes.get("invite-grey")))
    check("the spectate button is hidden in a ranked lobby (v10 g:219155)",
          got.get("spect", {}).get("cmpSpectBtn:display") == "none", str(got.get("spect")))
    check("the Host card is not greyed in a ranked lobby (v10 g:219156: official only)", nodes.get("host-grey") == 0,
          str(nodes.get("host-grey")))
    plain = texts.get("title-plain", "")
    check("a comp lobby that is not ranked is titled Krunker Competitive", re.match(r"Krunker Competitive\s*\d+v\d+ on \S", plain or "") is not None,
          repr(plain))
    check("the Invite button is live again outside ranked", nodes.get("invite-plain") == 0, str(nodes.get("invite-plain")))
    check("the spectate button shows again outside ranked", got.get("spect-plain", {}).get("cmpSpectBtn:display") == "-",
          str(got.get("spect-plain")))
    check("the Host card stays live outside official games", nodes.get("host-plain") == 0, str(nodes.get("host-plain")))
    return all(results)


def main():
    ok = run()
    print(f"\n{sum(results)}/{len(results)} passed  (log: {os.path.join(WORK, 'client.log')})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
