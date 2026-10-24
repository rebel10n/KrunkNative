#!/usr/bin/env python3
"""<video>.knv frame sidecars for the native HTML engine and the video skins (``make video-frames``).

The native engine (client/src/html/engine/video.c) has no video decoder: it plays a ``<video>``
whose source is ``<name>.mp4`` / ``<name>.webm`` from ``<name>.<ext>.knv`` next to it, a sequence
of baseline JPEG frames that the stb_image already in the tree decodes, one frame at a time, as
playback reaches it. The v10.0.3 video skins (client/src/skin_video.c: videos/video_<n>.mp4 in a
weapon / dye material) play the same way natively. The web build plays the real files in the
browser and needs no sidecar.

Format (little-endian):
    "KNVF"  u32 version (1)
    u16 natural_w, natural_h      the source video's size (the element's intrinsic size)
    u16 frame_w, frame_h          the stored frames' size (may be smaller: drawn scaled)
    u32 fps_num, fps_den          frame rate of the stored frames
    u32 frame_count, duration_ms
    u32 offsets[frame_count + 1]  byte offsets of each JPEG from the file start (last = file size)
    JPEG frames

Every KrunkNative video the v10 page plays, and every skin video, is listed in VIDEOS (path under
assets/, stored width, stored fps, ffmpeg -q:v quality); the sizes keep the sidecars small: frames no
larger than the page draws them. Re-run after the source videos change:

    python3 -I tools/ui-import/video_frames.py [--assets DIR] [--check] [path ...]

Needs ffmpeg/ffprobe. Writes only the .knv files; a sidecar whose content is unchanged is left alone.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (asset path, stored frame width (0: source width), stored fps (0: source fps), JPEG -q:v)
VIDEOS = (
    # v10 Eterno Shop header (svelte-gh77t9 .shop-background-video, g:128793): 900x132, drawn 1:1
    ("videos/limited_shop_banner.webm", 0, 0, 3),
    # v10 Guide "Basics" (tutorialPopup, g:197258): 1280x720, drawn 700px wide
    ("videos/tutorial/basics.mp4", 700, 15, 5),
    # v10 jumpScare(1) (#jumpSVID, g:222989): 384x480, 1.3 s
    ("scares/1.mp4", 0, 0, 3),
    # v10.0.3 video skins (getVideo, deob-10.0.3.js:232003-232016), 10 s loops kept at their own size and rate
    # (a screen-space `projection` skin stretches the frame over the whole view): video_0 the Deep Space dye and
    # the Mysterious Orb melee (736x336, 30 fps), video_2 the upgraded weapons' altSkin (480x360, 25 fps),
    # video_3 the Glitch dye (736x336, 30 fps)
    ("videos/video_0.mp4", 0, 0, 6),
    ("videos/video_2.mp4", 0, 0, 6),
    ("videos/video_3.mp4", 0, 0, 6),
)


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height,r_frame_rate:format=duration", "-of", "json", path],
                         check=True, stdout=subprocess.PIPE).stdout
    info = json.loads(out)
    st = info["streams"][0]
    num, den = (int(x) for x in st["r_frame_rate"].split("/"))
    return int(st["width"]), int(st["height"]), num, den, float(info["format"]["duration"])


def build(src, width, fps, quality):
    nat_w, nat_h, num, den, duration = probe(src)
    if fps:
        num, den = fps, 1
    fw = width or nat_w
    fw -= fw % 2
    fh = round(nat_h * fw / nat_w)
    fh -= fh % 2
    with tempfile.TemporaryDirectory(prefix="kn-video-frames-") as tmp:
        vf = "fps=%d/%d,scale=%d:%d:flags=lanczos,format=yuvj420p" % (num, den, fw, fh)
        subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", src, "-an", "-vf", vf, "-q:v", str(quality),
                        "-f", "image2", os.path.join(tmp, "f%05d.jpg")], check=True)
        names = sorted(n for n in os.listdir(tmp) if n.endswith(".jpg"))
        frames = []
        for n in names:
            with open(os.path.join(tmp, n), "rb") as fh_:
                frames.append(fh_.read())
    if not frames:
        sys.exit("video_frames.py: ffmpeg wrote no frames for %s" % src)
    head = struct.pack("<4sIHHHHIIII", b"KNVF", 1, nat_w, nat_h, fw, fh, num, den, len(frames), round(duration * 1000))
    table_size = 4 * (len(frames) + 1)
    offsets, pos = [], len(head) + table_size
    for f in frames:
        offsets.append(pos)
        pos += len(f)
    offsets.append(pos)
    return head + struct.pack("<%dI" % len(offsets), *offsets) + b"".join(frames), (nat_w, nat_h, fw, fh, num, den, len(frames))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--assets", default=os.path.join(REPO, "assets"))
    ap.add_argument("--check", action="store_true", help="report missing / stale sidecars, write nothing")
    ap.add_argument("paths", nargs="*", help="only these asset paths (default: every entry of VIDEOS)")
    args = ap.parse_args()
    bad = 0
    for rel, width, fps, quality in VIDEOS:
        if args.paths and rel not in args.paths:
            continue
        src = os.path.join(args.assets, rel)
        if not os.path.exists(src):
            print("%s: missing, skipped" % rel)
            continue
        data, (nw, nh, fw, fh, num, den, n) = build(src, width, fps, quality)
        dest = src + ".knv"
        same = os.path.exists(dest) and open(dest, "rb").read() == data
        print("%s.knv: %dx%d -> %dx%d, %d frames at %.2f fps, %d KiB%s" % (
            rel, nw, nh, fw, fh, n, num / den, len(data) // 1024, " (unchanged)" if same else ""))
        if same:
            continue
        if args.check:
            bad += 1
            continue
        tmp = dest + ".tmp"
        with open(tmp, "wb") as fh_:
            fh_.write(data)
        os.replace(tmp, dest)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
