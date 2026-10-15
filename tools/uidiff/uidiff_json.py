#!/usr/bin/env python3
"""Tiny JSON -> shell helper for uidiff.sh (run with ``python3 -I``).

  uidiff_json.py scenario <scenario.json>   -> one line: ref<TAB>w<TAB>h<TAB>background<TAB>frames<TAB>anim_times<TAB>pending
                                               (anim_times: comma list of ms, "-" for a single-frame scenario;
                                               skip: "pending: <text>" / "unreachable offline: <text>" of a scenario that is not
                                               rendered, "-" without)
  uidiff_json.py regions  <regions.json> [t] -> one line per region (of frame t of a sequence: the regions
                                               whose "times" include t, with their "at" {"<t>": {...}} overrides):
                                               name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>fuzz<TAB>max_pct<TAB>skip(0/1/pending text)
                                               <TAB>mask(none|ink|colors)<TAB>mask_kernel<TAB>mask_threshold
                                               <TAB>mask_colors(comma list)<TAB>mask_color_fuzz
                                               <TAB>ref_dx<TAB>ref_dy<TAB>expect(reference|background)
  uidiff_json.py timing   <regions.json>    -> one line per "timing" entry of a sequence:
                                               name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>threshold<TAB>tolerance_ms<TAB>skip(0/1/pending text)
  uidiff_json.py crossing <threshold>       -> stdin "t value" lines (a tracked region's change from the first
                                               frame); prints the time (ms, linearly interpolated between the
                                               two frames around it) where value / max(value) first reaches
                                               threshold, and max(value); "none <max>" when nothing changed
"""
import json
import sys

FRAME_MS = 1000.0 / 60.0
MIN_CHANGE = 0.002  # mean |difference| (0..1) below which a region did not move at all


def fmt_t(t):
    return ("%g" % t)


def one_line(text):
    return " ".join(str(text).split()) or "-"


def skip_field(r):
    # "pending": "<owner: what is missing>" reports the region as skipped with that text: the
    # native side of it is not built yet (UI-PARITY-AUDIT section 4); the owner removes it
    if r.get("pending"):
        return one_line(r["pending"])
    return 1 if r.get("skip") else 0


def scenario_skip(data):
    # a scenario that is not rendered: "pending" (the native side is not built yet) or "unreachable"
    # (the offline v10 sandbox cannot produce the state: no reference exists, nothing is faked)
    if data.get("unreachable"):
        return "unreachable offline: " + one_line(data["unreachable"])
    if data.get("pending"):
        return "pending: " + one_line(data["pending"])
    return "-"


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    mode, arg = argv[0], argv[1]
    if mode == "crossing":
        threshold = float(arg)
        pts = []
        for line in sys.stdin:
            parts = line.split()
            if len(parts) == 2:
                pts.append((float(parts[0]), float(parts[1])))
        peak = max((v for _, v in pts), default=0.0)
        if peak < MIN_CHANGE:
            print("none %.5f" % peak)
            return 0
        prev_t, prev_v = None, None
        for t, v in pts:
            r = v / peak
            if r >= threshold:
                if prev_t is None:
                    print("%.1f %.5f" % (t, peak))
                else:
                    pr = prev_v / peak
                    print("%.1f %.5f" % (prev_t + (t - prev_t) * (threshold - pr) / max(r - pr, 1e-9), peak))
                return 0
            prev_t, prev_v = t, v
        print("none %.5f" % peak)
        return 0
    with open(arg, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if mode == "scenario":
        size = data.get("size") or [0, 0]
        times = data.get("anim_times") or []
        print("\t".join(str(v) for v in (
            data.get("ref", "") or "-", int(size[0]), int(size[1]), data.get("background", "") or "-", int(data.get("frames", 1)),
            ",".join(fmt_t(t) for t in times) or "-", scenario_skip(data))))
        return 0
    if mode == "regions":
        defaults = data.get("defaults") or {}
        frame = float(argv[2]) if len(argv) > 2 else None
        for r in data.get("regions", []):
            if frame is not None:
                times = r.get("times")
                if times is not None and not any(abs(float(t) - frame) < 1e-6 for t in times):
                    continue
                for k, over in (r.get("at") or {}).items():
                    if abs(float(k) - frame) < 1e-6:
                        r = dict(r, **over)
            x, y, w, h = r["rect"]
            print("\t".join(str(v) for v in (
                r["name"], int(x), int(y), int(w), int(h),
                r.get("fuzz", defaults.get("fuzz", 6)),
                r.get("max_pct", defaults.get("max_pct", 0.5)),
                skip_field(r),
                r.get("mask", defaults.get("mask", "none")),
                int(r.get("mask_kernel", defaults.get("mask_kernel", 15))),
                r.get("mask_threshold", defaults.get("mask_threshold", 25)),
                ",".join(r.get("mask_colors", defaults.get("mask_colors", []))) or "-",
                r.get("mask_color_fuzz", defaults.get("mask_color_fuzz", 12)),
                int((r.get("ref_offset") or [0, 0])[0]), int((r.get("ref_offset") or [0, 0])[1]),
                r.get("expect", "reference"))))
        return 0
    if mode == "timing":
        for r in data.get("timing", []):
            x, y, w, h = r["rect"]
            print("\t".join(str(v) for v in (
                r["name"], int(x), int(y), int(w), int(h), r.get("threshold", 0.5),
                r.get("tolerance_ms", FRAME_MS), skip_field(r))))
        return 0
    sys.stderr.write("unknown mode %r\n" % mode)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
