#!/usr/bin/env python3
"""Krunker Strike's kill log honours NET_KILL_HIDE_FEED (client/src/net/game_events.c events_kill ->
client/src/ui/strike_hud.c strike_hud_killfeed), end to end against the krunker.io stub of
krunker_official_e2e.py: the flag only reaches the client through the official link ("3" KILL_PLAYER's
hideFeed argument, client/src/net/krunker_link.c on_kill), so the scripted kills are sent by that stub.

  python3 -I server/tests/strike_kill_hide_feed_e2e.py BUILD_DIR      (run from the repository root)

The stub's three kills of its bot by us, in one burst, client run with --flavor strike:
  1. no hideFeed, wId 1 + wallbang: a kill-log row with the wallbang icon;
  2. hideFeed true, wId 12 + pj (thrown): counts, but no kill-log row;
  3. no hideFeed, wId 12 + pj: a row with the thrown icon.
The kill log holds three rows, so with the flag honoured the log shows exactly two rows (kills 1 and 3);
a client that ignored the flag would show three (kills 1, 2 and 3). The rows are read from the page's DOM
with input_script's "nodes" probes every 600 ms while they are alive (10 s).
Ports come from kn_proto.free_ports() (krunker_official_e2e's launch_native). Headless: EGL, no window.
Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import asyncio
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import krunker_official_e2e as K  # noqa: E402  (stub, launcher, check(); its main() is not run)


class HideFeedStub(K.Stub):
    """krunker_official_e2e's Stub with the "ingame" phases replaced by the three kills above. Strike's page
    has no Security Challenge modal to paste into, so the first seek is let through (captcha_free)."""

    captcha_free = True

    async def ingame_phases(self, send, stop):
        async def hold(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end and not stop.is_set():
                qs = [f for f in self.frames if f[0] == "q"]
                isn = int(qs[-1][1][1]) if qs else 0
                await send("l", [isn, 42, 4.0, 0, 4.0, 0, 0, 0, 0.0, True, False, False, 0, False, False, 0, 0, 0, 0, True, False, 0, 0, None, 0])
                await asyncio.sleep(0.4)

        await hold(3.0)
        self.sent["ingame"] = time.monotonic()
        await send("3", K.BOT_SID, 1, K.MY_SID, None, {"wId": 1, "wb": True}, False)  # no hideFeed: a row, wallbang icon
        await send("3", K.BOT_SID, 2, K.MY_SID, None, {"wId": 12, "pj": True}, True)  # hideFeed: no row
        await send("3", K.BOT_SID, 3, K.MY_SID, None, {"wId": 12, "pj": True}, False)  # no hideFeed: a row, thrown icon
        await hold(6.0)
        self.sent["done"] = time.monotonic()


def run_strike_hide_feed():
    label = "strike-hide-feed"
    dense = ["nodes @rows .frvr-in-game-ui__kill-log-row", "nodes @wb [src='./img/wallbang_0.png']",
             "nodes @thrown [src='./img/thrown_0.png']"]
    probes = "".join("%d %s\n" % (t + k, kind) for t in range(5000, 22000, 600) for k, kind in enumerate(dense))
    script = "0 boot\n3800 play\n%s22500 quit\n" % probes
    events = sorted(script.strip().split("\n"), key=lambda l: int(l.split()[0]))
    stub, p, out, log = K.launch_native(label, "\n".join(events) + "\n", {}, args=("--krunker", "--flavor", "strike"), scenario="ingame")
    lines = K.finish_native(label, stub, p, out, log, timeout=240)
    text = "\n".join(lines)
    K.check(f"{label}: the client joined as the stub's sid", f"joined as sid {K.MY_SID}" in text)
    for name in ("ingame", "done"):
        K.check(f"{label}: the stub got to send its \"{name}\" packets", name in stub.sent, sorted(stub.sent))
    K.check(f"{label}: the server saw no ahNum / \"q\"-sequence violations", not stub.proto_errors, stub.proto_errors[:5])

    def counts(name):
        return [int(m.group(1)) for m in re.finditer(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % name, text)]

    rows, wb, thrown = counts("rows"), counts("wb"), counts("thrown")
    K.check(f"{label}: no flag: the wallbang kill has its Strike kill-log row", max(wb or [0]) >= 1, wb[-3:])
    K.check(f"{label}: no flag: the thrown kill has its Strike kill-log row", max(thrown or [0]) >= 1, thrown[-3:])
    K.check(f"{label}: hideFeed: the hidden kill adds no row (two rows, not three)", max(rows or [0]) == 2, rows[-3:])


def main():
    if K.websockets is None:
        print("strike_kill_hide_feed_e2e: needs the Python websockets package: skipped")
        return 77
    if not os.path.isfile(f"{K.BUILD}/client_bin"):
        print("strike_kill_hide_feed_e2e: no client_bin in %s: skipped" % K.BUILD)
        return 77
    K.Stub = HideFeedStub  # launch_native builds its stub through the module's Stub name
    run_strike_hide_feed()
    return 0 if K.results and all(K.results) else 1


if __name__ == "__main__":
    sys.exit(main())
