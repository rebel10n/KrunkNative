#!/usr/bin/env python3
"""A match end with rewards on the Krunker v10.0.2 page (client/src/ui/hud_endscreen_v10.c), end to end.

  python3 -I server/tests/v10_endscreen_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin --offline (Kanji, a sandbox bot, a guest) plays a match in which the local
player finds two skins (the "skin" path of NET_PACKET_SKIN_FOUND: input script `skin <index>` calls
hud_ui_skin_found, the same handler the server's packet reaches). The offline match gives no rewards
of its own, so the skins are the only source. At the end of the match v10.0.2 shows (g:195637-196080):
  - the Collect Rewards button (svelte-3svau8: "COLLECT", "N s to auto collect", the first item's
    icon) in #collectRewardsButtonContainer, in v10's #endButtonsHolder row under the results card;
  - the countdown ticks once a second;
  - a click on the button (its onclick handler) opens the rewards overlay (svelte-1sac6ik): one card per skin, the cards
    flip one by one (500 ms, then every 1000 ms with two items), the items fly to the backpack, and
    the overlay closes with the Rewards tab selected and the button gone (v10's onCompleteManualCollect).
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
WORK = tempfile.mkdtemp(prefix="kn-v10-endscreen-e2e-", dir=os.path.expanduser("~/.cache"))
results = []

# (time ms, command). Times are from the client's start (the input script's clock).
SCRIPT = """
0 menu
300 con bot_add 1
1000 check @menu menuHolder=1 chatUIHolder=1
1500 play
3500 check @alive weapHolder=1 deathUIHolder=0 timerHolder=1
3600 skin 1
3800 skin 2
4000 check @found inGameUI=1
5000 con end
7000 lrmr 14 4 25
10000 check @endanim endUI=1 inGameUI=0
15000 check @results endUI=1 endTabbedView=1 matchEndSignupIncentives=1
15000 nodes @button .collect-rewards-button
15000 text @label .auto-collect-label
15000 text @collect .collect-text
15000 nodes @overlay-before .rewards-overlay
15000 rects @btn .collect-rewards-button
15000 rects @signup .signup-rewards-container
15000 rects @tab .tabBody
15000 rects @holder .endButtonsHolder
15000 rects @card #endTabbedView
15000 nodes @dxp .doubleXPButton
15000 rects @dxp .doubleXPButton
15500 nodes @rank1 .rank-display
15500 text @name1 .rank-display .rank-value
15500 text @rating1 .rank-display .crVal
15500 nodes @pos1 .rank-display .crVal.positive
15500 text @rp1 .otherEarnings .crVal
16000 text @label-later .auto-collect-label
16000 nodes @button-later .collect-rewards-button
16100 call knCollectRewards
16120 lrmr -7 3 0
16200 nodes @rank2 .rank-display
16200 text @name2 .rank-display .rank-value
16200 text @rating2 .rank-display .crVal
16200 nodes @neg2 .rank-display .crVal.negative
16200 text @rp2 .otherEarnings .crVal
16250 lrmr 0 0 0
16300 nodes @rank3 .rank-display
16500 nodes @cards .reward-card
16500 nodes @overlay-up .rewards-overlay
17700 nodes @flipped .reward-card.flipped
18300 nodes @flying .flying-item
21000 check @done endUI=1 chatUIHolder=1
21000 nodes @overlay-gone .rewards-overlay
21000 nodes @button-gone .collect-rewards-button
21000 text @tab .tabHeader.selected
21000 nodes @rewards-list .rewItemHolder
22000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def expand(script):
    lines, expects = [], []
    for raw in script.strip().splitlines():
        t, cmd, *rest = raw.split()
        if cmd == "check":
            label = rest[0].lstrip("@")
            expects.append((label, dict(kv.split("=", 1) for kv in rest[1:])))
            lines.append(f"{t} vis @{label} " + " ".join(kv.split("=", 1)[0] for kv in rest[1:]))
        elif cmd == "esc":
            lines += [f"{t} down ESC", f"{float(t) + 50:g} up ESC"]
        else:
            lines.append(raw)
    return "\n".join(lines) + "\n", expects


def run():
    text, expects = expand(SCRIPT)
    script = os.path.join(WORK, "endscreen.script")
    with open(script, "w") as f:
        f.write(text)
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
        lines = f.read().splitlines()
    check("the v10 page loaded", any("[input_script] vis" in l for l in lines), log)
    check("both skins were found during the match", sum("[input_script] skin " in l for l in lines) == 2)

    got, nodes, texts, rects = {}, {}, {}, {}
    for line in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @(\S+)(.*)", line)
        if m:
            got.setdefault(m.group(1), {}).update(kv.split("=", 1) for kv in m.group(2).split())
        m = re.search(r"\[input_script\] nodes t=\d+ @?(\S+) elements=\d+ match=(\d+)", line)
        if m:
            nodes[m.group(1)] = int(m.group(2))
        m = re.search(r"\[input_script\] text t=\d+ @?(\S+) n=(\d+) checked=\d+ value=.*? text=(.*)$", line)
        if m:
            texts[m.group(1)] = (int(m.group(2)), m.group(3))
        m = re.search(r"\[input_script\] rects t=\d+ @?(\S+) n=(\d+)(.*)$", line)
        if m:
            cells = re.findall(r"@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)", m.group(3))
            rects[m.group(1)] = [tuple(float(v) for v in c) for c in cells]
    for label, want in expects:
        have = got.get(label, {})
        bad = {k: have.get(k, "no probe") for k, v in want.items() if have.get(k) != v}
        check(f"{label}: " + " ".join(f"{k}={v}" for k, v in want.items()), not bad, f"got {bad}" if bad else "")

    def n(label):
        return nodes.get(label)

    check("Collect Rewards button mounted with the two found skins", n("button") == 1, f"{n('button')} .collect-rewards-button")
    check("the rewards overlay is closed before the click", n("overlay-before") == 0, f"{n('overlay-before')} .rewards-overlay")
    tlabel = texts.get("label", (0, ""))[1]
    check("countdown label: \"N s to auto collect\" (v10 g:195974)", re.fullmatch(r"\d+s to auto collect", tlabel or "") is not None, repr(tlabel))
    tc = texts.get("collect", (0, ""))[1]
    check("button text: COLLECT", tc == "COLLECT", repr(tc))
    secs = lambda s: int(re.match(r"(\d+)s", s).group(1)) if re.match(r"(\d+)s", s or "") else None
    a, b = secs(tlabel), secs(texts.get("label-later", (0, ""))[1])
    check("the countdown ticks down once a second", a is not None and b is not None and a > b >= 0, f"{a}s then {b}s")
    btn = (rects.get("btn") or [None])[0]
    # v10 puts #endButtonsHolder right after .endCard (margin-top 10px). The guest's signup panel is
    # fixed (main.css #matchEndSignupIncentives top 850px, z-index above everything): with the map
    # vote cards up (endMidHolder top 310px) it covers the row at 16:9 in v10 as well, so the click
    # below goes through the button's handler (the onclick of the svelte-3svau8 button) instead of
    # a hit test, and the auto collect is what a guest gets there in v10.
    tab = (rects.get("tab") or [None])[0]
    holder = (rects.get("holder") or [None])[0]
    if tab and holder and btn:
        check("the button row sits under the results card (v10 #endButtonsHolder after .endCard)",
              holder[1] >= tab[1] + tab[3] and holder[1] <= btn[1],
              f"card bottom {tab[1] + tab[3]:.0f}, row top {holder[1]:.0f}, button top {btn[1]:.0f}")
    else:
        check("the results card, the button row and the button have rects", False, f"tab={tab} holder={holder} btn={btn}")
    dxp = (rects.get("dxp") or [None])[0]
    check("Double XP button in #endButtonsHolder (v10 endButtonsHolder, one button)", n("dxp") == 1, f"{n('dxp')} #doubleXPButton")
    if dxp and btn:
        check("Double XP and COLLECT share a row, COLLECT to the right (v10 flex, gap 10px)",
              abs(dxp[1] - btn[1]) < 10 and btn[0] > dxp[0] + dxp[2], f"Double XP x {dxp[0]:.0f}..{dxp[0] + dxp[2]:.0f} y {dxp[1]:.0f}, COLLECT x {btn[0]:.0f} y {btn[1]:.0f}")
    check("two reward cards in the overlay after the click", n("cards") == 2, f"{n('cards')} .reward-card")
    check("the overlay is up during the flips", n("overlay-up") == 1, f"{n('overlay-up')} .rewards-overlay")
    check("both cards flipped by 1.2 s after the click (500 ms, then 1000 ms apart)", n("flipped") == 2, f"{n('flipped')} .flipped")
    check("the items fly to the backpack", (n("flying") or 0) >= 1, f"{n('flying')} .flying-item")
    check("the overlay closed after the flights", n("overlay-gone") == 0, f"{n('overlay-gone')} .rewards-overlay")
    check("the button is gone once collected", n("button-gone") == 0, f"{n('button-gone')} .collect-rewards-button")
    tab = texts.get("tab", (0, ""))[1]
    check("the Rewards tab is selected afterwards", tab == "Rewards", repr(tab))
    check("the Rewards tab lists the two skins", (n("rewards-list") or 0) == 2, f"{n('rewards-list')} .rewItemHolder")
    # v10 lrmr (the rating reply's packet, here the input script's lrmr): the rank card and "+N RP" (g:195637, g:195699)
    check("the rank card shows the rank the match left the player at (Silver 1), held from the end animation",
          n("rank1") == 1 and texts.get("name1", (0, ""))[1] == "Silver 1", f"{n('rank1')} .rank-display, name {texts.get('name1')}")
    check("the rating change: \"+14\" in the positive colour (v10 crVal positive)",
          texts.get("rating1", (0, ""))[1] == "+14" and n("pos1") == 1, f"{texts.get('rating1')} positive={n('pos1')}")
    check("the RP change: \"+25 RP\" under the card (v10 .otherEarnings)", texts.get("rp1", (0, ""))[1] == "+25 RP", repr(texts.get("rp1")))
    check("a later rating reply updates the card in place: Bronze 3, \"-7\" in the negative colour",
          n("rank2") == 1 and texts.get("name2", (0, ""))[1] == "Bronze 3" and texts.get("rating2", (0, ""))[1] == "-7" and n("neg2") == 1,
          f"{n('rank2')} {texts.get('name2')} {texts.get('rating2')} negative={n('neg2')}")
    check("no RP in that reply: no \"RP\" line under the card", texts.get("rp2", (0, ""))[1] == "" and (n("rank2") or 0) == 1, repr(texts.get("rp2")))
    check("a placing reply (rank 0) takes the card away (v10: shown for latestRank > 0 only)", n("rank3") == 0, f"{n('rank3')} .rank-display")
    return all(results)


def main():
    ok = run()
    print(f"\n{sum(results)}/{len(results)} passed  (log: {os.path.join(WORK, 'client.log')})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
