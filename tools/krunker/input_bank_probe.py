#!/usr/bin/env python3
"""Turn an input-bank probe log (KRUNKNATIVE_INPUT_BANK_PROBE, client/include/krunker_probe.h) into Krunker's input
bank: the cap (the most held-back movement the official server still simulates after a lag-switch burst) and the
refill (how fast it comes back). doc/v10/input-bank-probe.md explains the run.

For every walk length L the log has a control walk (frames sent as made) and a held walk (every frame kept back for
L, then sent at once). The server's displacement over each (its "l" positions, before the walk and after it settled)
gives covered = L * held / control: the walk time the server let through. Up to the cap covered ~ L; above it,
covered stays at the cap. A refill pair is two held walks of the longest length L with a rest g between them: the
bank drains at the first burst and refills for g + L until the second, so covered / (g + L) is the refill rate
wherever the second walk comes back below the cap.

usage: python3 -I tools/krunker/input_bank_probe.py input_bank_probe.tsv
"""

import math
import sys

SETTLE_MS = 1200.0  # after a walk ends: the burst's positions have arrived (the probe stands 1500 ms)


def read(path):
    steps, syncs, fails, releases = [], [], [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or line.startswith("t_ms"):
                continue
            c = line.rstrip("\n").split("\t")
            if len(c) < 2:
                continue
            t, ev = float(c[0]), c[1]
            if ev == "step":
                steps.append({"t": t, "index": int(c[2]), "label": c[3], "kind": c[4], "ms": float(c[6])})
            elif ev == "sync":
                syncs.append((t, float(c[5]), float(c[6]), float(c[7])))
            elif ev == "fail":
                fails.append((t, int(c[4])))
            elif ev == "release":
                releases.append((t, int(c[4])))
    return steps, syncs, fails, releases


def pos_at(syncs, t, after):
    """The last server position at or before t (after=False), or the first at or after it (after=True)."""
    best = None
    for s in syncs:
        if after and s[0] >= t:
            return s
        if not after and s[0] <= t:
            best = s
    return best


def displacement(syncs, start, length, until=None):
    """The server's horizontal move from the walk's start to SETTLE_MS after its end, or to just before the next
    walk starts when that is sooner (a refill pair's second walk follows after a short rest)."""
    a = pos_at(syncs, start, after=False)
    end = start + length + SETTLE_MS
    b = pos_at(syncs, until, after=False) if until is not None and until < end else pos_at(syncs, end, after=True)
    if not a or not b:
        return None
    return math.hypot(b[1] - a[1], b[3] - a[3])  # horizontal: x and z (Krunker y is up)


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1])
        return 2
    steps, syncs, fails, releases = read(argv[1])
    if not syncs or not steps:
        print("no server positions or steps in the log: did the probe arm (a game joined, KRUNKNATIVE_INPUT_BANK_PROBE set)?")
        return 1
    moving = [s for s in steps if s["kind"] in ("control", "held")]
    walks = {}
    for i, s in enumerate(moving):
        until = moving[i + 1]["t"] - 1.0 if i + 1 < len(moving) else None
        walks[s["label"]] = (s, displacement(syncs, s["t"], s["ms"], until))
    print(f"{len(syncs)} server positions, {len(steps)} steps, {len(releases)} releases, {len(fails)} Input Failed")
    for t, code in fails:
        print(f"  Input Failed {code} at {t / 1000:.1f} s")

    print("\nlength_ms  control  held  ratio  covered_ms")
    covered = []
    control_speed = {}
    # a control walk that moved under a third of the run's best speed hit something (a wall): its pair is skipped
    speeds = [d / s["ms"] for s, d in walks.values() if s["kind"] == "control" and d]
    best = max(speeds) if speeds else 0.0
    for label, (s, d) in walks.items():
        if s["kind"] != "control":
            continue
        length = int(s["ms"])
        held = walks.get(f"held-{length}")
        if d is None or not held or held[1] is None:
            print(f"{length:9d}  (missing positions)")
            continue
        if best <= 0.0 or d / length < best / 3.0:
            print(f"{length:9d}  {d:7.3f}  the control walk barely moved: start the probe in the open (doc)")
            continue
        control_speed[length] = d
        ratio = held[1] / d
        covered.append((length, ratio * length, ratio))
        print(f"{length:9d}  {d:7.3f}  {held[1]:5.3f}  {ratio:5.2f}  {ratio * length:10.0f}")

    capped = [c for c in covered if c[2] < 0.9]
    if not covered:
        print("\ncap: no usable walks")
    elif not capped:
        print(f"\ncap: none up to {max(c[0] for c in covered)} ms (every held walk moved in full); try longer lengths")
    else:
        plateau = sorted(c[1] for c in capped)
        cap = plateau[len(plateau) // 2]
        first = min(c[0] for c in capped)
        print(f"\ncap: ~{cap:.0f} ms of held-back movement (median covered of the {len(capped)} capped walks; the first capped length is {first} ms)")

    pairs = [(s, d) for label, (s, d) in walks.items() if label.startswith("pairB-")]
    if pairs:
        longest = max(control_speed) if control_speed else None
        print("\nrefill pairs (rest_ms, first held covered_ms, second held covered_ms, refill = second / (rest + length))")
        for s, d in pairs:
            gap = int(s["label"].split("-")[1])
            a = walks.get(f"pairA-{gap}")
            if d is None or not a or a[1] is None or not longest:
                print(f"  {gap:6d}  (missing positions)")
                continue
            speed = control_speed[longest] / longest
            ca, cb = a[1] / speed, d / speed
            print(f"  {gap:6d}  {ca:8.0f}  {cb:8.0f}  {cb / (gap + longest):5.2f}x")
        print("  (between the two bursts the bank refills for the rest plus the second walk's length; the rate only")
        print("   shows where the first walk drained the bank and the second came back below the cap)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
