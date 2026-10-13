"""Krunker v7.1.4 window templates (``make ui-import`` step, loaded by import.py --ref v7).

Splits ``tools/ui-import/krunker-windows-v7.json.gz`` -- every menu window captured from
v7's own client by ``capture_v7_windows.js`` (``showWindow(n)``: header, ``#menuWindow``
class/inline style and markup) -- into ``ui/windows/``:

- ``<n>.html``: the markup of ``showWindow(n)`` (``windows[n - 1]``), for a returning
  guest with default settings. Item lists the C UI regenerates from game data (the
  cosmetic pickers' ``#skinList``, the host window's map list) keep only their first
  ``ITEM_KEEP`` cards, as templates (``TRIM``).
- ``settings-basic-<t>.html`` / ``settings-advanced-<t>.html``: ``windows[0]`` (Game
  Settings) for every tab of the basic and the advanced view. The per-weapon clones v7
  appends hidden to each tab (``<key>_<weapon>_div`` rows) are dropped: KrunkNative has
  no per-weapon settings storage yet.
- ``controls-<t>.html``: ``windows[6]`` (Change Controls), the five tabs.
- ``windows.json``: ``{"windows": {"<n>": {header, sticky, file, label, cls, style}},
  "settings": {header, sticky, cls, style, basic: [{name, file}], advanced: [...]},
  "controls": {header, sticky, cls, style, tabs: [{name, file}]}}``.

``?build=`` query strings of the captured URLs are stripped and ``https://assets.krunker.io/``
URLs made relative (as v7's hook.js rewrites them). Window 25 (Download Client) keeps only v7's
header and description: KrunkNative fills its package list (``client_download_template``).
Nothing else is changed.
"""

import gzip
import json
import os
import re
from html.parser import HTMLParser

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "krunker-windows-v7.json.gz")
# the whole query up to the URL's delimiter: v7 sometimes writes past it ("other.png?build=X.png",
# which its server answers with other.png)
QUERY_RE = re.compile(r"\?(?:build|v)=[^\"'\s)&]*")
ITEM_KEEP = 4

# showWindow id -> id of the element whose children are trimmed to ITEM_KEEP
TRIM = {}
# Host Custom Game (windows[7]): its hidden "Advanced" tab bodies (per-weapon / per-class
# value matrices, ~600 KB) are emptied: hosting is server_bin's job in KrunkNative.
HOST_TAB_MAX = 32 * 1024
PICKER_LIST = "skinList"  # every cosmetic picker (ui_loadout.c)
UNUSED = {"11", "12", "19", "22", "29"}  # windows[] slots v7 leaves empty / unused


class _Children(HTMLParser):
    """Finds the [start, end) offsets of the direct children of the element with `target_id`."""

    VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}

    def __init__(self, text, target_id):
        super().__init__(convert_charrefs=False)
        self.text = text
        self.target_id = target_id
        self.depth = 0
        self.target_depth = None
        self.children = []  # [start, end]
        self.inner = None   # [start, end] of the target's content
        self._line_starts = [0]
        for m in re.finditer("\n", text):
            self._line_starts.append(m.end())

    def _pos(self):
        line, col = self.getpos()
        return self._line_starts[line - 1] + col

    def handle_starttag(self, tag, attrs):
        pos = self._pos()
        end = pos + len(self.get_starttag_text())
        if self.target_depth is not None and self.depth == self.target_depth + 1:
            self.children.append([pos, end])
        if tag in self.VOID:
            if self.target_depth is not None and self.depth == self.target_depth + 1:
                self.children[-1][1] = end
            return
        self.depth += 1
        if self.target_depth is None and dict(attrs).get("id") == self.target_id:
            self.target_depth = self.depth
            self.inner = [end, None]

    def handle_endtag(self, tag):
        if tag in self.VOID:
            return
        pos = self._pos()
        end = self.text.index(">", pos) + 1
        if self.target_depth is not None:
            if self.depth == self.target_depth + 1 and self.children:
                self.children[-1][1] = end
            if self.depth == self.target_depth and self.inner and self.inner[1] is None:
                self.inner[1] = pos
                self.target_depth = None
        self.depth -= 1


def trim_children(html, element_id, keep):
    p = _Children(html, element_id)
    p.feed(html)
    p.close()
    if not p.inner or p.inner[1] is None or len(p.children) <= keep:
        return html, 0
    cut_from = p.children[keep - 1][1] if keep else p.inner[0]
    cut_to = p.inner[1]
    return html[:cut_from] + html[cut_to:], len(p.children) - keep


def drop_per_weapon_rows(html):
    """Remove v7's hidden per-weapon clones: <div class="settName" ... id="<key>_<n>_div" style="display:none">...</div>."""
    out = []
    pos = 0
    pat = re.compile(r'<div class="settName"[^>]*id="[A-Za-z0-9]+_\d+_div"[^>]*>')
    for m in pat.finditer(html):
        if m.start() < pos:
            continue
        # matching </div>: count nested divs
        depth, i = 1, m.end()
        while depth and i < len(html):
            nxt_open = html.find("<div", i)
            nxt_close = html.find("</div>", i)
            if nxt_close < 0:
                break
            if 0 <= nxt_open < nxt_close:
                depth += 1
                i = nxt_open + 4
            else:
                depth -= 1
                i = nxt_close + 6
        out.append(html[pos:m.start()])
        pos = i
    out.append(html[pos:])
    return "".join(out)


def empty_big_blocks(html, opener, limit):
    """Empty every <div> starting with `opener` whose content is longer than `limit`."""
    out, pos = [], 0
    while True:
        at = html.find(opener, pos)
        if at < 0:
            break
        start = html.index(">", at) + 1
        depth, i = 1, start
        while depth and i < len(html):
            o, c = html.find("<div", i), html.find("</div>", i)
            if c < 0:
                break
            if 0 <= o < c:
                depth, i = depth + 1, o + 4
            else:
                depth, i = depth - 1, c + 6
        end = i - 6
        if end - start > limit:
            out.append(html[pos:start])
            pos = end
        else:
            out.append(html[pos:i])
            pos = i
    out.append(html[pos:])
    return "".join(out)


CLIENT_WINDOW = "25"  # showWindow(25), windows[24] "client_download"
CLIENT_PLACEHOLDER = '<div id="knDownloads"></div>'


def client_download_template(html):
    """windows[24] (Download Client) as KrunkNative's template (intentional difference).

    v7's markup links Krunker's own installers (https://krunker.zip/glorp/Krunker.io_Client_*);
    KrunkNative keeps v7's header and description and replaces the installer list with
    CLIENT_PLACEHOLDER, which client/src/ui/download_ui.c fills with the KrunkNative packages
    of the GitHub release `client-latest` (the visitor's platform highlighted). A re-import
    therefore never brings the krunker.zip links back; a capture this no longer matches fails
    the import instead of passing them through.
    """
    start = html.find("<div><ul")
    end = html.rfind("</ul></div>")
    if "Download Krunker Client" not in html or start < 0 or end < start:
        raise SystemExit("windows_v7.py: window %s no longer looks like v7's Download Client window" % CLIENT_WINDOW)
    html = html[:start] + CLIENT_PLACEHOLDER + html[end + len("</ul></div>"):]
    html = html.replace("Download Krunker Client", "Download Krunker Box Office")
    if "krunker.zip" in html:
        raise SystemExit("windows_v7.py: window %s still links krunker.zip" % CLIENT_WINDOW)
    return html


def load_capture(path=SOURCE):
    with gzip.open(path, "rt", encoding="utf-8") as fh:
        return json.load(fh)


ASSETS_HOST_RE = re.compile(r"https?://assets\.krunker\.io/")


def _clean(html):
    """?build= stripped; the asset host (rewritten to the local tree by v7's hook.js) made relative."""
    return ASSETS_HOST_RE.sub("", QUERY_RE.sub("", html or ""))


def split_windows_v7(out_dir, write_if_changed, log, path=SOURCE):
    data = load_capture(path)
    index = {"windows": {}, "settings": {}, "controls": {}}
    files = []
    written = 0

    def emit(name, html):
        nonlocal written
        if write_if_changed(os.path.join(out_dir, name), html):
            written += 1
        files.append((name, len(html.encode("utf-8"))))

    for wid, win in sorted(data["windows"].items(), key=lambda kv: int(kv[0])):
        if wid in UNUSED or win.get("error"):
            continue
        html = _clean(win.get("html", ""))
        trimmed = 0
        if wid in TRIM:
            html, trimmed = trim_children(html, TRIM[wid], ITEM_KEEP)
        elif 'id="%s"' % PICKER_LIST in html and wid not in ("1", "7"):
            html, trimmed = trim_children(html, PICKER_LIST, ITEM_KEEP)
        if wid == "1":
            html = drop_per_weapon_rows(html)
        if wid == "8":
            html = empty_big_blocks(html, '<div class="hostTb2 hide"', HOST_TAB_MAX)
        if wid == CLIENT_WINDOW:
            html = client_download_template(html)
        name = "%s.html" % wid
        emit(name, html)
        entry = {"header": win.get("header") or "", "sticky": bool(win.get("sticky")), "file": name,
                 "label": win.get("label"), "cls": win.get("cls") or "", "style": win.get("style") or ""}
        if trimmed:
            entry["trimmed"] = trimmed
        index["windows"][wid] = entry

    settings = data.get("settings", {})
    first = data["windows"].get("1", {})
    s = {"header": first.get("header") or "", "sticky": bool(first.get("sticky")), "cls": first.get("cls") or "",
         "style": first.get("style") or ""}
    for view in ("basic", "advanced"):
        tabs = []
        for t, tab in enumerate(settings.get(view, [])):
            name = "settings-%s-%d.html" % (view, t)
            emit(name, drop_per_weapon_rows(_clean(tab["html"])))
            tabs.append({"name": tab["name"], "file": name})
        s[view] = tabs
    index["settings"] = s

    seven = data["windows"].get("7", {})
    c = {"header": seven.get("header") or "", "sticky": bool(seven.get("sticky")), "cls": seven.get("cls") or "",
         "style": seven.get("style") or "", "tabs": []}
    for t, tab in enumerate(data.get("controls", [])):
        name = "controls-%d.html" % t
        emit(name, _clean(tab["html"]))
        c["tabs"].append({"name": tab["name"], "file": name})
    index["controls"] = c

    text = json.dumps(index, indent=2, ensure_ascii=False) + "\n"
    if write_if_changed(os.path.join(out_dir, "windows.json"), text):
        written += 1
    files.append(("windows.json", len(text.encode("utf-8"))))
    total = sum(size for _, size in files)
    log("windows (v7): %d files (%d bytes) from %s, %d rewritten" % (len(files), total, os.path.basename(path), written))
    return index, files, written
