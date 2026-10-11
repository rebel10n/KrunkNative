#!/usr/bin/env python3
"""Tiny JSON -> shell helper for uidiff.sh (run with ``python3 -I``).

  uidiff_json.py scenario <scenario.json>   -> one line: ref<TAB>w<TAB>h<TAB>background<TAB>frames
  uidiff_json.py regions  <regions.json>    -> one line per region:
                                               name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>fuzz<TAB>max_pct<TAB>skip(0/1)
                                               <TAB>mask(none|ink|colors)<TAB>mask_kernel<TAB>mask_threshold
                                               <TAB>mask_colors(comma list)<TAB>mask_color_fuzz
                                               <TAB>ref_dx<TAB>ref_dy<TAB>expect(reference|background)
"""
import json
import sys


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    mode, path = argv
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if mode == "scenario":
        size = data.get("size") or [0, 0]
        print("\t".join(str(v) for v in (
            data.get("ref", "") or "-", int(size[0]), int(size[1]), data.get("background", "") or "-", int(data.get("frames", 1)))))
        return 0
    if mode == "regions":
        defaults = data.get("defaults") or {}
        for r in data.get("regions", []):
            x, y, w, h = r["rect"]
            print("\t".join(str(v) for v in (
                r["name"], int(x), int(y), int(w), int(h),
                r.get("fuzz", defaults.get("fuzz", 6)),
                r.get("max_pct", defaults.get("max_pct", 0.5)),
                1 if r.get("skip") else 0,
                r.get("mask", defaults.get("mask", "none")),
                int(r.get("mask_kernel", defaults.get("mask_kernel", 15))),
                r.get("mask_threshold", defaults.get("mask_threshold", 25)),
                ",".join(r.get("mask_colors", defaults.get("mask_colors", []))) or "-",
                r.get("mask_color_fuzz", defaults.get("mask_color_fuzz", 12)),
                int((r.get("ref_offset") or [0, 0])[0]), int((r.get("ref_offset") or [0, 0])[1]),
                r.get("expect", "reference"))))
        return 0
    sys.stderr.write("unknown mode %r\n" % mode)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
