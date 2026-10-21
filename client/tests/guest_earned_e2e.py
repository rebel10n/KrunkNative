#!/usr/bin/env python3
"""A guest's Unclaimed Rewards box (client/src/ui/v10_rewards.c: Krunker v10.0.2's _39368, svelte-1hb83uo)
renders where v10 mounts it, with its amounts and its green glow (box-shadow: 0 0 15px rgba(158, 235, 86, 0.25)).

  python3 -I client/tests/guest_earned_e2e.py BUILD_DIR      (run from the repository root)

Headless renders (client_bin --render-scenario, no window), state.guestEarned standing in for the ledger:
  1. the signed-out end screen with 12 KR / 800 score (tools/uidiff/scenarios/kn-guest-earned-end.json): the box
     is in the signup rewards' right slot (.signup-rewards-guest-earned-collect) with "Unclaimed Rewards", the KR
     count-up at 12 with the angled coin, the XP one at 800, no REGISTER NOW (origin match_end), and the glow:
     the same frame rendered from a copy of ui/ whose svelte.css has no box-shadow on .guest-earned-collect
     differs from it only outside the box (the 1..15 px ring), never inside it;
  2. the main menu with both amounts capped (kn-guest-earned-menu.json): the box in the right panel (#tlInfHold)
     with CAPPED on both, "Register to earn more rewards" and REGISTER NOW / Claim your rewards;
  3. the end screen of a stand-in account with the same balance: no box (v10 renders it for a guest only);
  4. the end screen of a guest with nothing earned: the slot is empty (as the v10 sandbox capture shows);
  5. the end card of a guest with no balance, v10's own capture state (ingame-endscreen-late.json,
     doc/ref/krunker-v10/ingame/ingame-endscreen-late.png): the Double XP button under the results card at
     v10's place (514, 312, 246 x 48) and the signup banner below it, its top at v10's 385;
  6. the end card's offer (check_end_card_offer): an account on games 4 without an offer still sees the Double XP
     button; the click claims (rqdxp) on games % 3 only, whatever the offer says, and a guest's click claims nothing;
     a comp match shows "Return to Krunker" (#returnButton) and no Double XP, signed in or not;
  7. the same end card with three vote maps (#endMidHolder moves down to top 310): the Double XP button and the
     signup banner do not overlap, and the banner stays on the screen.
KRUNKNATIVE_SCENARIO_DUMP prints the box tree under the slot after the capture: the texts and the box's
viewport rect come from it. Exit 0 when every check passes, 1 otherwise.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.getcwd()
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
CLIENT = os.path.join(BUILD, "client_bin")
SCENARIOS = os.path.join(ROOT, "tools", "uidiff", "scenarios")
VP = re.compile(r"vp\(([-\d.]+),([-\d.]+) ([\d.]+)x([\d.]+)\)")
TEXT = re.compile(r'text "([^"]*)"')
SHADOW = "box-shadow:0 0 15px rgba(158, 235, 86, 0.25);"
WORK = tempfile.mkdtemp(prefix="kn-guest-earned-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def scenario(name, base, patch):
    with open(os.path.join(SCENARIOS, base), encoding="utf-8") as f:
        doc = json.load(f)
    doc["state"].update(patch)
    for key in [k for k, v in patch.items() if v is None]:
        del doc["state"][key]
    path = os.path.join(WORK, name + ".json")
    with open(path, "w") as f:
        json.dump(doc, f)
    return path


def render(name, scen, dump, ui_dir=None):
    """The capture's PNG path, the dump's lines (texts, boxes) and the dumped root's viewport rect."""
    out = os.path.join(WORK, name + ".png")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg-" + name),
        "XDG_STATE_HOME": os.path.join(WORK, "state-" + name), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
        "KRUNKNATIVE_SCENARIO_DUMP": dump, "KRUNKNATIVE_ASSETS_DIR": ASSETS,
    })
    if ui_dir:
        env["KRUNKNATIVE_UI_DIR"] = ui_dir
    proc = subprocess.run([CLIENT, "--render-scenario", scen, out], env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)
    lines = proc.stderr.splitlines() + proc.stdout.splitlines() # the client's [double_xp] log is on stdout
    if proc.returncode != 0 or not os.path.exists(out):
        print("\n".join(lines[-20:]))
        return None, lines, None
    rect = None
    for line in lines: # the dumped root's own line: the first box line (its element matched the selector)
        if line.startswith(("block <", "flex <", "grid <", "inline <")) and dump.split()[-1].lstrip("#.") in line:
            m = VP.search(line)
            if m:
                rect = tuple(float(m.group(i)) for i in range(1, 5))
            break
    return out, lines, rect


def texts(lines):
    return [m.group(1).strip() for line in lines for m in [TEXT.search(line)] if m]


def read_png(path):
    """(width, height, rows of RGBA bytes) of an 8-bit RGB(A) PNG, no external module."""
    with open(path, "rb") as f:
        data = f.read()
    pos, chunks, w = 8, [], 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and color in (2, 6), (depth, color)
            bpp = 3 if color == 2 else 4
        elif kind == b"IDAT":
            chunks.append(body)
        pos += 12 + n
    raw = zlib.decompress(b"".join(chunks))
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                cur[i] = (cur[i] + a) & 255
            elif ft == 2:
                cur[i] = (cur[i] + b) & 255
            elif ft == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(cur))
        prev = cur
    return w, h, bpp, rows


def pixel_diff(a, b, rect, ring):
    """(changed inside the rect shrunk by 2 px, changed in the ring 1..ring px outside it)."""
    wa, ha, bpp, ra = read_png(a)
    wb, hb, bpp2, rb = read_png(b)
    assert (wa, ha, bpp) == (wb, hb, bpp2), ((wa, ha, bpp), (wb, hb, bpp2))
    x0, y0, w, h = rect
    x1, y1 = x0 + w, y0 + h
    inside = outside = 0
    for y in range(max(0, int(y0 - ring)), min(ha, int(y1 + ring) + 1)):
        for x in range(max(0, int(x0 - ring)), min(wa, int(x1 + ring) + 1)):
            if ra[y][x * bpp:x * bpp + 3] == rb[y][x * bpp:x * bpp + 3]:
                continue
            if x0 + 2 <= x < x1 - 2 and y0 + 2 <= y < y1 - 2:
                inside += 1
            elif x < x0 - 1 or x > x1 or y < y0 - 1 or y > y1:
                outside += 1
    return inside, outside


def ui_without_shadow():
    """A copy of ui/ (its other trees linked) whose svelte.css lacks the box's box-shadow."""
    src = os.path.join(ROOT, "ui")
    dst = os.path.join(WORK, "ui-noshadow")
    os.makedirs(os.path.join(dst, "css"))
    for entry in os.listdir(src):
        if entry != "css":
            os.symlink(os.path.join(src, entry), os.path.join(dst, entry))
    for entry in os.listdir(os.path.join(src, "css")): # css/fonts/ stays a link
        if os.path.isdir(os.path.join(src, "css", entry)):
            os.symlink(os.path.join(src, "css", entry), os.path.join(dst, "css", entry))
        else:
            shutil.copy(os.path.join(src, "css", entry), os.path.join(dst, "css", entry))
    path = os.path.join(dst, "css", "svelte.css")
    with open(path, encoding="utf-8") as f:
        css = f.read()
    assert css.count(SHADOW) == 1, css.count(SHADOW)
    with open(path, "w", encoding="utf-8") as f:
        f.write(css.replace(SHADOW, ""))
    return dst


END_BOX = "#matchEndSignupIncentives .guest-earned-collect" # not the menu's (#knGuestEarned, hidden with the menu in game)


def check_end_screen():
    scen = os.path.join(SCENARIOS, "kn-guest-earned-end.json")
    png, lines, rect = render("end", scen, END_BOX)
    check("end screen: a guest with 12 KR / 800 score renders", png is not None)
    if png is None:
        return
    t = texts(lines)
    check("end screen: the Unclaimed Rewards box is in the signup rewards' right slot", rect is not None and "Unclaimed Rewards" in t,
          " | ".join(t))
    check("end screen: the KR count-up ends at 12 and the XP one at 800", "12" in t and "800" in t and "XP" in t, " | ".join(t))
    check("end screen: the angled coin (an <img>) is the KR item's icon", # the img box follows its .coin-icon line
          any(lines[i + 1].strip().startswith("img <img") for i, line in enumerate(lines[:-1]) if "coin-icon" in line), "")
    check("end screen: no REGISTER NOW at the match end (showRegister false)", "REGISTER NOW" not in t and "CAPPED" not in t, " | ".join(t))
    if rect is None:
        return
    x, y, w, h = rect
    check("end screen: the box is v10's 350 px wide slot at the UI scale (the right 316 px slot's)", 150 < w < 360 and 60 < h < 200,
          f"{w:.0f}x{h:.0f} at {x:.0f},{y:.0f}")
    flat, _, rect2 = render("end-noshadow", scen, END_BOX, ui_without_shadow())
    check("end screen: the same frame renders without the box's box-shadow", flat is not None and rect2 == rect)
    if flat is None:
        return
    inside, outside = pixel_diff(png, flat, rect, 15)
    check("end screen: the glow paints outside the box (box-shadow: 0 0 15px rgba(158, 235, 86, 0.25))", outside >= 200,
          f"{outside} pixels differ in the 15 px ring")
    check("end screen: the glow leaves the box's inside as it is", inside == 0, f"{inside} pixels differ inside")


V10_GUEST_CAPTURE = os.path.join(SCENARIOS, "ingame-endscreen-late.json") # v10's guest end card, no balance (1280x720)
DOUBLE_XP_AT = (514, 312, 246, 48) # v10's #doubleXPButton in ingame-endscreen-late.png
SIGNUP_BANNER_TOP = 385            # v10's signup banner container (absolute top 850 px at the UI scale, centred)


def check_double_xp_and_banner():
    """The end card of a guest with no balance renders v10's Double XP button under the results card, and the
    signup banner below it, both where v10's own capture draws them."""
    png, lines, button = render("zero-button", V10_GUEST_CAPTURE, "#doubleXPButton")
    check("end card, a guest: renders", png is not None)
    if png is None:
        return
    t = texts(lines)
    check("end card, a guest: the Double XP button is shown (v10's #doubleXPButton, not a comp match)",
          button is not None and "Double XP" in t, " | ".join(t))
    if button is not None:
        bx, by, bw, bh = button
        check("end card, a guest: the Double XP button is where v10 draws it (514, 312, 246 x 48)",
              all(abs(a - b) <= 3 for a, b in zip((bx, by, bw, bh), DOUBLE_XP_AT)), f"{bx:.0f},{by:.0f} {bw:.0f}x{bh:.0f}")
    _, _, banner = render("zero-banner", V10_GUEST_CAPTURE, "#matchEndSignupIncentives .signup-rewards-container")
    check("end card, a guest: the signup banner is at v10's place (top 385)",
          banner is not None and abs(banner[1] - SIGNUP_BANNER_TOP) <= 3, f"banner top {banner[1]:.0f}" if banner else "no banner")
    if button is not None and banner is not None:
        check("end card, a guest: the signup banner is below the Double XP button",
              banner[1] >= button[1] + button[3], f"button bottom {button[1] + button[3]:.0f}, banner top {banner[1]:.0f}")


VOTE_MAPS = [{"mode": 0, "map": 0, "votes": 0, "name": "Lagoon"}, {"mode": 0, "map": 1, "votes": 0, "name": "Tropicana"},
             {"mode": 1, "map": 2, "votes": 0, "name": "Freight"}]


def boxes_intersect(a, b):
    (ax, ay, aw, ah), (bx, by, bw, bh) = a, b
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def check_vote_cards_and_banner():
    """With the vote cards (three maps: v10:219592 puts #endMidHolder at top 310, so the card and its Double XP button
    move down), the signup banner must not cover the button: their boxes do not intersect, and the banner stays on the
    screen (its top is v10's 385 when the button leaves room, below the button with v10's gap when it does not)."""
    with open(V10_GUEST_CAPTURE, encoding="utf-8") as f:
        doc = json.load(f)
    doc["state"]["matchVotes"] = VOTE_MAPS
    path = os.path.join(WORK, "votes-guest.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    png, lines, button = render("votes-button", path, "#doubleXPButton")
    check("end card, a guest with vote cards: renders", png is not None and button is not None)
    if png is None or button is None:
        return
    _, _, banner = render("votes-banner", path, "#matchEndSignupIncentives .signup-rewards-container")
    if banner is None:
        check("end card, a guest with vote cards: the signup banner renders", False)
        return
    check("end card, a guest with vote cards: the Double XP button and the signup banner do not overlap",
          not boxes_intersect(button, banner), f"button {button[1]:.0f}-{button[1] + button[3]:.0f}, "
          f"banner {banner[1]:.0f}-{banner[1] + banner[3]:.0f}")
    check("end card, a guest with vote cards: the signup banner is on the screen (bottom within 720 px)",
          banner[1] + banner[3] <= 720.5, f"banner bottom {banner[1] + banner[3]:.0f}")


END_CARD = os.path.join(SCENARIOS, "ingame-endscreen-late.json") # a match's end card (results, no balance)


def end_card_scenario(name, account=None, comp=False, claim=False):
    """The end card's capture state with a stand-in account, a comp match (endscreen.comp) and/or the Double XP
    button clicked when the card shows (endscreen.doubleXP "claim")."""
    with open(END_CARD, encoding="utf-8") as f:
        doc = json.load(f)
    if account is not None:
        doc["state"]["account"] = account
    if comp:
        doc["state"]["endscreen"]["comp"] = 1
    if claim:
        doc["state"]["endscreen"]["doubleXP"] = "claim"
    path = os.path.join(WORK, name + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return path


def account_at(games, claimable):
    """A stand-in account after `games` matches, with the brain's offer state (dxp.offer)."""
    return {"name": "Player", "score": 50000, "funds": 100, "games": games,
            "dxp": {"offer": {"games": games, "xp": 120, "claimable": claimable}}}


def check_end_card_offer():
    """v10's end card (GUI.endScreen.show, v10.js 203200): #doubleXPButton after every match but a comp one, for a
    signed-in player as for a guest; #returnButton ("Return to Krunker", backToOrigin()) in its place on a comp
    match. The click (showDoubleXPRewardedAd, v10.js 222690) claims only on games % 3 (_74098, v10.js 222668), with
    an account id: a guest's click claims nothing."""
    png, lines, rect = render("offer-shown", end_card_scenario("offer-shown", account=account_at(4, False)), "#doubleXPButton")
    t = texts(lines)
    check("end card, an account on games 4 (no offer): the Double XP button is shown (every non-comp match)",
          png is not None and rect is not None and "Double XP" in t, " | ".join(t))

    def sent(name, account):
        png, lines, _ = render(name, end_card_scenario(name, account=account, claim=True), "#endButtonsHolder")
        return png, [line for line in lines if "rqdxp" in line]

    png, lines = sent("claim-games3", account_at(3, True))
    check("end card, an account on games 3: the Double XP click sends the claim (rqdxp at games 3)",
          png is not None and any("rqdxp at games 3" in s for s in lines), " | ".join(lines))
    png, lines = sent("claim-games4", account_at(4, False))
    check("end card, an account on games 4: the click sends nothing (v10 claims on games % 3 only)",
          png is not None and not lines, " | ".join(lines))
    png, lines = sent("claim-games4-offer", account_at(4, True))
    check("end card, an account on games 4 whose offer is claimable: still nothing sent (the rule is games % 3)",
          png is not None and not lines, " | ".join(lines))
    png, lines = sent("claim-guest", None)
    check("end card, a guest's click: nothing sent (v10's _74098 needs an account id)", png is not None and not lines, " | ".join(lines))

    png, lines, rect = render("comp-guest", end_card_scenario("comp-guest", comp=True), "#returnButton")
    t = texts(lines)
    check("end card, a comp match, a guest: Return to Krunker in place of the Double XP button",
          rect is not None and "Return to Krunker" in t and "Double XP" not in t, " | ".join(t))
    png, lines, rect = render("comp-account", end_card_scenario("comp-account", account=account_at(3, True), comp=True),
                              "#endButtonsHolder")
    t = texts(lines)
    check("end card, a comp match, an account on games 3 (claimable): Return to Krunker, no Double XP",
          png is not None and "Return to Krunker" in t and "Double XP" not in t, " | ".join(t))


def check_menu():
    scen = os.path.join(SCENARIOS, "kn-guest-earned-menu.json")
    png, lines, rect = render("menu", scen, "#knGuestEarned")
    check("menu: a guest with 20 KR / 2500 score (both caps) renders", png is not None)
    if png is None:
        return
    t = texts(lines)
    check("menu: the box is in the right panel with both amounts", rect is not None and "Unclaimed Rewards" in t and "20" in t and "2500" in t,
          " | ".join(t))
    check("menu: CAPPED on both, the warning and REGISTER NOW / Claim your rewards",
          t.count("CAPPED") == 2 and any("Register to earn more rewards" in s for s in t) and "REGISTER NOW" in t and "Claim your rewards" in t,
          " | ".join(t))
    if rect:
        x, y, w, h = rect
        check("menu: the box hangs at the panel's top right (top 80 px, right 20 px at the UI scale)", x > 900 and y < 80,
              f"{w:.0f}x{h:.0f} at {x:.0f},{y:.0f}")


def check_account_and_empty():
    account = {"name": "Player", "score": 50000, "funds": 100}
    scen = scenario("end-account", "kn-guest-earned-end.json", {"account": account})
    png, lines, rect = render("end-account", scen, ".guest-earned-collect") # neither the end screen's nor the menu's
    check("end screen, an account: renders", png is not None)
    check("end screen, an account: no box (v10 renders it for a guest only)", png is not None and rect is None
          and any("no match" in line for line in lines), " | ".join(texts(lines))[:200])
    scen = scenario("end-empty", "kn-guest-earned-end.json", {"guestEarned": None})
    png, lines, rect = render("end-empty", scen, ".signup-rewards-guest-earned-collect")
    check("end screen, nothing earned: renders", png is not None)
    check("end screen, nothing earned: the slot is empty", png is not None and rect is not None and rect[3] == 0
          and "Unclaimed Rewards" not in texts(lines), f"{rect}")


def main():
    if not os.path.exists(CLIENT):
        print(f"client_bin not found: {CLIENT}")
        return 1
    check_end_screen()
    check_double_xp_and_banner()
    check_vote_cards_and_banner()
    check_end_card_offer()
    check_menu()
    check_account_and_empty()
    print(f"{sum(results)}/{len(results)} checks passed  (work dir {WORK})")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
