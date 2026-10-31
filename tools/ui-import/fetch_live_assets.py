#!/usr/bin/env python3
"""Krunker content the v7.1.4 reference tree lacks, fetched from Krunker's live asset hosts.

``assets/`` is a byte copy of Krunker v7.1.4's ``public/`` (``game_assets.py --v7``), but that
offline tree is incomplete: the v7 client and the catalogue maps reference files it never had
(weapon-skin sounds, zombie perk machine textures, AI/zombie models, map user-asset textures,
models and sounds, HUD images...). The v7 client loads them from its CDNs:

  ``UTILS.assetsUrl(p)``     -> https://assets.krunker.io/<p>         (textures/ models/ sound/ img/ ...)
  ``UTILS.userAssetsUrl(p)`` -> https://user-assets.krunker.io/<p>    (``<id>/model.obj``, ``<id>/texture.png``, ...)

``live_assets.lock`` (next to this script) pins every file taken from there: install root
(``assets`` = assets/, ``ui`` = the page's ui/ tree for ``img/``), path, source URL, sha256 and
size. ``live_assets_v10.lock`` (``--lock``) pins the Krunker v10 (live client) content the same
way: files v7.1.4 never had (root ``assets``) and the v10 versions of files whose v7.1.4 copy
differs (root ``assets-v10`` = ``assets/v10/<path>``: v7 content is never replaced). Root
``assets-aac`` is a file Krunker's host serves as MP4/AAC under its ``.mp3`` name (v10.0.3's newer
weapon-skin sounds): the pinned sha256 is the host's bytes, and install writes them transcoded to
MP3 (ffmpeg libmp3lame, 44.1 kHz, 192 kbit/s) at ``assets/<path>`` like the other sounds, since the
native audio decoder (miniaudio) reads MP3 / WAV / FLAC only; check then only asks for an MP3 there.
Installing only ever ADDS files: an existing file is never overwritten (Krunker content
already in the tree wins, a different hash is reported). ``deploy/ansible`` role ``kn_web``
(``--tags web_assets``) syncs ``assets/`` to the web asset host and role ``kn_updates`` re-indexes
the same tree as the launcher's content index, so both pick installed files up; ``ui/img`` files
are tracked in git and ship with the web build / packages.

usage:
  python3 -I tools/ui-import/fetch_live_assets.py install [--lock F] [--assets DIR] [--ui DIR] [--stage DIR] [--roots assets,ui] [--dry-run]
      install the pinned files missing from the tree (from --stage when its copy matches the
      sha256, else downloaded and verified); prints every file added
  python3 -I tools/ui-import/fetch_live_assets.py check [--lock F] [--assets DIR] [--ui DIR]
      list pinned files that are missing (exit 1) or differ in the tree
  python3 -I tools/ui-import/fetch_live_assets.py fetch --list missing.tsv --stage DIR [--manifest F] [--write-lock F | --merge-lock F]
      download the paths of an inventory TSV (category, path[, source]) into a staging tree,
      validate them and record url / HTTP status / sha256 / size per file (inventory tooling)

environment (no secrets here; nothing is needed besides network access to the hosts):
  KN_FETCH_PROXY   HTTP(S) proxy for the downloads, e.g. http://127.0.0.1:8888 (default: none)
  KN_FETCH_UA      User-Agent (default: a current desktop Chrome); the requests carry the
                   usual browser Accept / Accept-Language headers and Referer https://krunker.io/
  KN_FETCH_JOBS    parallel downloads, at most 4 (default 2); KN_FETCH_DELAY seconds between
                   requests per worker (default 0.3)
"""

import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LOCK = os.path.join(HERE, "live_assets.lock")
ASSETS_HOST = "https://assets.krunker.io/"
USER_ASSETS_HOST = "https://user-assets.krunker.io/"
DEFAULT_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/141.0.0.0 Safari/537.36")
ACCEPT = {
    "image": "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8",
    "audio": "audio/webm,audio/ogg,audio/wav,audio/*;q=0.9,application/ogg;q=0.7,video/*;q=0.6,*/*;q=0.5",
    "other": "*/*",
}
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".webp", ".gif", ".svg")
AUDIO_EXT = (".mp3", ".ogg", ".wav", ".mp4", ".webm")


def root_of(path):
    """Install root of an asset path: the page's own images live in ui/img (KN's menu tree)."""
    return "ui" if path.startswith("img/") else "assets"


def url_of(path):
    if path.startswith("user-assets/"):
        return USER_ASSETS_HOST + urllib.parse.quote(path[len("user-assets/"):])
    return ASSETS_HOST + urllib.parse.quote(path)


# ---------------------------------------------------------------- validation

def _ffprobe_ok(data, want):
    try:
        r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=format_name", "-of", "csv=p=0", "-i", "pipe:0"],
                           input=data, capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None  # no ffprobe: header checks only
    names = r.stdout.decode("utf8", "replace").strip()
    return r.returncode == 0 and any(w in names for w in want)


def png_without_bad_ancillary(data):
    """The PNG without its ancillary chunks whose CRC is wrong; None when a critical chunk (IHDR, PLTE,
    IDAT, IEND) is broken, the chunks run past the data or nothing was dropped."""
    out, i, dropped = [data[:8]], 8, False
    while i + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[i:i + 8])
        end = i + 12 + n
        if end > len(data):
            return None
        good = zlib.crc32(data[i + 4:i + 8 + n]) & 0xffffffff == struct.unpack(">I", data[i + 8 + n:end])[0]
        if good:
            out.append(data[i:end])
        elif kind[0] & 0x20:  # lower-case first letter: ancillary
            dropped = True
        else:
            return None
        i = end
        if kind == b"IEND":
            return b"".join(out) if dropped else None
    return None


def validate(path, data):
    """(ok, note): the bytes are a decodable file of the type the extension promises."""
    ext = os.path.splitext(path)[1].lower()
    head = data[:512].lstrip().lower()
    if not data:
        return False, "empty"
    if ext != ".svg" and (head.startswith(b"<!doctype") or head.startswith(b"<html") or head.startswith(b"<?xml")
                          or head.startswith(b"<error")):
        return False, "html/xml error page"
    if ext in (".png", ".jpg", ".jpeg", ".webp", ".gif"):
        magic = {".png": data[:8] == b"\x89PNG\r\n\x1a\n", ".jpg": data[:3] == b"\xff\xd8\xff", ".jpeg": data[:3] == b"\xff\xd8\xff",
                 ".webp": data[:4] == b"RIFF" and data[8:12] == b"WEBP", ".gif": data[:4] == b"GIF8"}[ext]
        try:
            from PIL import Image
            with Image.open(io.BytesIO(data)) as im:
                im.load()
                size = "%dx%d %s" % (im.width, im.height, im.mode)
        except ImportError:
            return magic, "magic only (no Pillow)"
        except Exception as e:  # noqa: BLE001 - any decoder failure means a bad file
            fixed = png_without_bad_ancillary(data) if ext == ".png" and magic else None
            if fixed is None:
                return False, "undecodable image: %s" % e
            try: # Krunker's file has a broken optional chunk (a bad iCCP CRC): browsers and stb_image skip it
                with Image.open(io.BytesIO(fixed)) as im:
                    im.load()
                    return True, "%dx%d %s (an ancillary chunk with a bad CRC, ignored as browsers do)" % (im.width, im.height, im.mode)
            except Exception:  # noqa: BLE001
                return False, "undecodable image: %s" % e
        return True, size if magic else size + " (extension/format mismatch)"
    if ext == ".svg":
        return (b"<svg" in data[:4096].lower()), "svg"
    if ext in (".mp3", ".ogg", ".mp4", ".webm", ".wav"):
        want = {".mp3": ("mp3",), ".ogg": ("ogg",), ".mp4": ("mp4", "mov"), ".webm": ("webm", "matroska"), ".wav": ("wav",)}[ext]
        ok = _ffprobe_ok(data, want)
        if ok is None:
            if ext == ".mp3":
                ok = data[:3] == b"ID3" or (data[0] == 0xFF and (data[1] & 0xE0) == 0xE0)
            elif ext == ".ogg":
                ok = data[:4] == b"OggS"
            else:
                ok = data[4:8] == b"ftyp" or data[:4] == b"\x1a\x45\xdf\xa3"
            return ok, "header only (no ffprobe)"
        return ok, "ffprobe " + ("ok" if ok else "failed")
    if ext == ".obj":
        try:
            text = data.decode("utf8")
        except UnicodeDecodeError:
            text = data.decode("latin-1")
        lines = text.splitlines()
        nv = sum(1 for l in lines if l.startswith("v "))
        nf = sum(1 for l in lines if l.startswith("f "))
        return nv > 0 and nf > 0, "obj %d v, %d f" % (nv, nf)
    if ext == ".gltf":
        try:
            j = json.loads(data)
        except ValueError as e:
            return False, "bad gltf json: %s" % e
        return isinstance(j, dict) and "asset" in j, "gltf %s" % (j.get("asset", {}).get("version") if isinstance(j, dict) else "?")
    if ext == ".glb":
        import struct
        if len(data) < 12 or data[:4] != b"glTF":
            return False, "no glTF magic"
        version, length = struct.unpack("<II", data[4:12])
        return version == 2 and length == len(data), "glb v%d %d bytes" % (version, length)
    if ext == ".fbx":
        return data[:18] == b"Kaydara FBX Binary" or b"FBXHeaderExtension" in data[:4096], "fbx"
    if ext == ".zip":
        import zipfile
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                bad = z.testzip()
                n = len(z.namelist())
        except zipfile.BadZipFile as e:
            return False, "bad zip: %s" % e
        return bad is None, "zip %d entries" % n
    if ext == ".json":
        try:
            json.loads(data)
            return True, "json"
        except ValueError as e:
            return False, "bad json: %s" % e
    return True, "unchecked type"


def gltf_external_uris(data):
    """Relative buffer / image URIs of a .gltf (data: URIs are embedded)."""
    try:
        j = json.loads(data)
    except ValueError:
        return []
    out = []
    for key in ("buffers", "images"):
        for item in j.get(key, []) or []:
            uri = item.get("uri") if isinstance(item, dict) else None
            if uri and not uri.startswith("data:") and "://" not in uri:
                out.append(urllib.parse.unquote(uri))
    return out


# ---------------------------------------------------------------- HTTP

class Fetcher:
    def __init__(self):
        proxy = os.environ.get("KN_FETCH_PROXY", "")
        handlers = [urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {})]
        self.opener = urllib.request.build_opener(*handlers)
        self.ua = os.environ.get("KN_FETCH_UA", DEFAULT_UA)
        self.delay = float(os.environ.get("KN_FETCH_DELAY", "0.3"))
        self.blocked = 0  # consecutive WAF-looking answers
        self.lock = threading.Lock()

    def get(self, url, path):
        """(status, bytes, content_type, note); retries 429 / 5xx / network errors with backoff."""
        ext = os.path.splitext(path)[1].lower()
        kind = "image" if ext in IMAGE_EXT else "audio" if ext in AUDIO_EXT else "other"
        headers = {"User-Agent": self.ua, "Accept": ACCEPT[kind], "Accept-Language": "en-US,en;q=0.9",
                   "Referer": "https://krunker.io/", "Origin": "https://krunker.io", "Sec-Fetch-Site": "cross-site",
                   "Sec-Fetch-Mode": "no-cors" if kind != "other" else "cors",
                   "Sec-Fetch-Dest": {"image": "image", "audio": "audio", "other": "empty"}[kind]}
        last = (0, b"", "", "")
        for attempt in range(5):
            if self.delay:
                time.sleep(self.delay)
            try:
                with self.opener.open(urllib.request.Request(url, headers=headers), timeout=60) as r:
                    return r.status, r.read(), r.headers.get("Content-Type", ""), ""
            except urllib.error.HTTPError as e:
                body = e.read() if hasattr(e, "read") else b""
                last = (e.code, body, e.headers.get("Content-Type", "") if e.headers else "", "")
                if e.code in (429, 500, 502, 503, 504):
                    time.sleep(min(30, 2 ** attempt) + (int(e.headers.get("Retry-After", "0") or 0) if e.headers else 0))
                    continue
                return last
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                last = (0, b"", "", "network: %s" % e)
                time.sleep(min(30, 2 ** attempt))
        return last

    def note_block(self, status, ctype, body):
        """Consecutive 403 / challenge pages: the WAF is refusing us."""
        waf = status in (401, 403) or (status == 200 and "text/html" in ctype and b"challenge" in body[:4096].lower())
        with self.lock:
            self.blocked = self.blocked + 1 if waf else 0
            return self.blocked


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_atomic(dest, data):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kn-fetch-", dir=os.path.dirname(dest))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.chmod(tmp, 0o644)
    os.replace(tmp, dest)


# ---------------------------------------------------------------- lock file

LOCK_HEADER = ("# Krunker live asset host files missing from the v7.1.4 reference tree (tools/ui-import/fetch_live_assets.py).\n"
               "# root\tpath\turl\tsha256\tsize\n")


def read_lock(path):
    entries = []
    with open(path, encoding="utf8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            root, rel, url, digest, size = line.rstrip("\n").split("\t")
            entries.append({"root": root, "path": rel, "url": url, "sha256": digest, "size": int(size)})
    return entries


def lock_header(path):
    """The leading comment lines of an existing lock (its own description), else LOCK_HEADER."""
    if not os.path.exists(path):
        return LOCK_HEADER
    lines = []
    with open(path, encoding="utf8") as f:
        for line in f:
            if not line.startswith("#"):
                break
            lines.append(line)
    return "".join(lines) or LOCK_HEADER


def write_lock(path, entries, header=LOCK_HEADER):
    with open(path, "w", encoding="utf8") as f:
        f.write(header)
        for e in sorted(entries, key=lambda e: (e["root"], e["path"])):
            f.write("%s\t%s\t%s\t%s\t%d\n" % (e["root"], e["path"], e["url"], e["sha256"], e["size"]))


def dest_of(entry, assets, ui):
    # root assets-v10: a Krunker v10 file whose name v7.1.4 already uses with other content
    # (live_assets_v10.lock); it goes to assets/v10/<path> so the v7 file stays as it is
    # root assets-aac: an MP4/AAC file under an .mp3 name, installed transcoded at assets/<path>
    base = ui if entry["root"] == "ui" else os.path.join(assets, "v10") if entry["root"] == "assets-v10" else assets
    rel = os.path.normpath(entry["path"])
    if rel.startswith("..") or os.path.isabs(rel):
        raise SystemExit("refusing path outside the tree: %s" % entry["path"])
    return os.path.join(base, rel)


def webp_sidecar(dest):
    """The native HTML engine reads <name>.webp.png for a .webp URL (game_assets.py)."""
    side = dest + ".png"
    if os.path.exists(side):
        return None
    try:
        from PIL import Image
        with Image.open(dest) as im:
            im.seek(0)
            buf = io.BytesIO()
            im.convert("RGBA").save(buf, "PNG")
        write_atomic(side, buf.getvalue())
        return side
    except Exception:  # noqa: BLE001
        return None


def root_selected(root, roots):
    """--roots: assets-aac files install with the assets root (they land in assets/)."""
    return root in roots or (root == "assets-aac" and "assets" in roots)


def is_mp3(data):
    return data[:3] == b"ID3" or (len(data) > 1 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0)


def transcode_mp3(data):
    """MP4/AAC bytes -> MP3 (the sound/ tree's format), None when ffmpeg is missing or fails."""
    with tempfile.TemporaryDirectory() as tmp:
        src, out = os.path.join(tmp, "in.m4a"), os.path.join(tmp, "out.mp3")
        with open(src, "wb") as f:
            f.write(data)
        try:
            r = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", src, "-vn", "-map_metadata", "-1",
                                "-c:a", "libmp3lame", "-ar", "44100", "-b:a", "192k", out], capture_output=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode != 0 or not os.path.isfile(out):
            return None
        with open(out, "rb") as f:
            mp3 = f.read()
        return mp3 if is_mp3(mp3) else None


# ---------------------------------------------------------------- commands

def cmd_install(a):
    roots = set(a.roots.split(","))
    entries = [e for e in read_lock(a.lock) if root_selected(e["root"], roots)]
    fetcher = None
    added, kept, failed = [], [], []
    for e in entries:
        dest = dest_of(e, a.assets, a.ui)
        if os.path.lexists(dest):
            if os.path.isfile(dest) and e["root"] != "assets-aac":  # an assets-aac file is the transcoded copy
                with open(dest, "rb") as f:
                    if sha256(f.read()) != e["sha256"]:
                        kept.append(dest)
            continue
        data = None
        if a.stage:
            staged = os.path.join(a.stage, "assets" if e["root"] in ("assets-v10", "assets-aac") else e["root"], e["path"])  # fetch stages by host path
            if os.path.isfile(staged):
                with open(staged, "rb") as f:
                    data = f.read()
                if sha256(data) != e["sha256"]:
                    data = None
        if data is None:
            if a.dry_run:
                added.append(dest + " (download)")
                continue
            fetcher = fetcher or Fetcher()
            status, data, _, note = fetcher.get(e["url"], e["path"])
            if status != 200 or sha256(data) != e["sha256"]:
                failed.append("%s: HTTP %s %s%s" % (e["path"], status, note, " sha256 mismatch" if status == 200 else ""))
                continue
        if a.dry_run:
            added.append(dest)
            continue
        if e["root"] == "assets-aac":
            data = transcode_mp3(data)
            if data is None:
                failed.append("%s: the MP3 transcode failed (ffmpeg with libmp3lame needed)" % e["path"])
                continue
        write_atomic(dest, data)
        added.append(dest)
        if dest.endswith(".webp"):
            side = webp_sidecar(dest)
            if side:
                added.append(side)
    for d in added:
        print("added", os.path.relpath(d, REPO) if d.startswith(REPO + os.sep) else d)
    for d in kept:
        print("kept existing (differs from the live host)", d)
    for d in failed:
        print("FAILED", d, file=sys.stderr)
    print("live assets: %d pinned, %d %s, %d kept, %d failed" % (len(entries), len(added), "would be added" if a.dry_run else "added", len(kept), len(failed)))
    if added and not a.dry_run:
        print("next: deploy/ansible --tags web_assets (kn_web asset sync + kn_updates content index)")
    return 1 if failed else 0


def cmd_check(a):
    missing = differ = 0
    for e in read_lock(a.lock):
        dest = dest_of(e, a.assets, a.ui)
        if not os.path.isfile(dest):
            print("missing", e["root"] + "/" + e["path"])
            missing += 1
            continue
        with open(dest, "rb") as f:
            data = f.read()
        if (not is_mp3(data)) if e["root"] == "assets-aac" else sha256(data) != e["sha256"]:
            print("differs", e["root"] + "/" + e["path"])
            differ += 1
    print("live assets: %d missing, %d differ" % (missing, differ))
    return 1 if missing else 0


def cmd_fetch(a):
    todo = []
    with open(a.list, encoding="utf8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            cols = line.rstrip("\n").split("\t")
            path = cols[1] if len(cols) > 1 else cols[0]
            todo.append((cols[0] if len(cols) > 1 else "", path))
    done = set()
    if a.manifest and os.path.exists(a.manifest) and not a.refetch:
        with open(a.manifest, encoding="utf8") as f:
            for line in f:
                if line.startswith("#"):
                    continue
                cols = line.rstrip("\n").split("\t")
                if len(cols) > 2 and cols[2] in ("200", "404", "403"):
                    done.add(cols[1])
    fetcher = Fetcher()
    jobs = max(1, min(4, int(os.environ.get("KN_FETCH_JOBS", "2"))))
    out_lock = threading.Lock()
    man = open(a.manifest, "a", encoding="utf8") if a.manifest else None
    if man and man.tell() == 0:
        man.write("# category\tpath\tstatus\turl\tsha256\tsize\tvalid\tnote\n")
    abort = threading.Event()
    queue = [(c, p) for c, p in todo if p not in done]
    seen = set(done)

    def one(item):
        cat, path = item
        if abort.is_set():
            return []
        url = url_of(path)
        status, data, ctype, note = fetcher.get(url, path)
        valid, vnote, digest, size = False, note, "", 0
        follow = []
        if status == 200:
            valid, vnote = validate(path, data)
            digest, size = sha256(data), len(data)
            if valid:
                write_atomic(os.path.join(a.stage, root_of(path), path), data)
                if path.endswith(".gltf"):
                    base = os.path.dirname(path)
                    follow = [("gltf-dep", os.path.normpath(os.path.join(base, u)).replace(os.sep, "/")) for u in gltf_external_uris(data)]
        if fetcher.note_block(status, ctype, data) >= 12:
            abort.set()
        with out_lock:
            if man:
                man.write("%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\n" % (cat, path, status, url, digest, size, int(valid), (vnote or ctype).replace("\t", " ")))
                man.flush()
            print("%3s %s %s" % (status, path, "" if valid or status != 200 else "INVALID " + vnote), flush=True)
        return follow

    while queue and not abort.is_set():
        nxt = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            for follow in ex.map(one, queue):
                for item in follow:
                    if item[1] not in seen:
                        seen.add(item[1])
                        nxt.append(item)
        queue = nxt
    if man:
        man.close()
    if abort.is_set():
        print("ABORTED: the asset host refused 12 requests in a row (WAF?)", file=sys.stderr)
        return 2
    if (a.write_lock or a.merge_lock) and a.manifest:
        entries = {}
        with open(a.manifest, encoding="utf8") as f:
            for line in f:
                if line.startswith("#"):
                    continue
                cat, path, status, url, digest, size, valid, _ = line.rstrip("\n").split("\t", 7)
                if status == "200" and valid == "1":
                    entries[path] = {"root": root_of(path), "path": path, "url": url, "sha256": digest, "size": int(size)}
        if a.write_lock:
            write_lock(a.write_lock, entries.values())
            print("wrote %s (%d files)" % (a.write_lock, len(entries)))
        if a.merge_lock:
            # add the new files to an existing lock: its entries (and their roots: assets-v10, assets-aac) stay
            have = {e["path"]: e for e in (read_lock(a.merge_lock) if os.path.exists(a.merge_lock) else [])}
            added = [e for p, e in entries.items() if p not in have]
            for e in added:
                have[e["path"]] = e
            write_lock(a.merge_lock, have.values(), lock_header(a.merge_lock))
            print("%s: %d files added (%d in all)" % (a.merge_lock, len(added), len(have)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("install", "check"):
        p = sub.add_parser(name)
        p.add_argument("--lock", default=LOCK)
        p.add_argument("--assets", default=os.path.join(REPO, "assets"))
        p.add_argument("--ui", default=os.path.join(REPO, "ui"))
        if name == "install":
            p.add_argument("--stage", help="staging tree from `fetch` (<stage>/assets/..., <stage>/ui/...)")
            p.add_argument("--dry-run", action="store_true")
            p.add_argument("--roots", default="assets,assets-v10,ui",
                           help="install roots (comma list): assets = assets/ (untracked game content, with the "
                                "assets-aac sounds transcoded to MP3), assets-v10 = assets/v10/ (v10 versions of v7 files), "
                                "ui = ui/img (tracked)")
    p = sub.add_parser("fetch")
    p.add_argument("--list", required=True, help="TSV: category, path (relative to the asset host)")
    p.add_argument("--stage", required=True)
    p.add_argument("--manifest", help="append url / status / sha256 / size per file (TSV)")
    p.add_argument("--write-lock", help="write a lock file from the manifest's valid 200s")
    p.add_argument("--merge-lock", help="add the manifest's valid 200s that a lock lacks to that lock (kept otherwise)")
    p.add_argument("--refetch", action="store_true", help="ignore paths the manifest already answered")
    a = ap.parse_args()
    return {"install": cmd_install, "check": cmd_check, "fetch": cmd_fetch}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
