"""Krunker v10 window templates (step of import.py).

Splits ``tools/ui-import/krunker-windows-v10.json.gz`` -- the ``windows.json`` of
``capture_v10_live.js`` (every ``showWindow(n)`` of the v10 client: header, ``#menuWindow``
class / inline style / markup, every settings tab of both views and the Change Controls tabs) --
into ``ui/windows/``, with the layout and rules of ``windows_v7.py`` (``<n>.html``,
``settings-{basic,advanced}-<t>.html``, ``controls-<t>.html``, ``windows.json``), so
``client/src/ui/ui_windows.c`` reads them.

``pack <capture>/windows.json`` rewrites the .gz from a new capture (gzip mtime 0, sorted keys).

Differences from the v7 split, all intentional:

- The capture's client ran offline (``KN_V10_PUBLIC``: the mirror, no API, a logged-out guest),
  so windows built from server data come out empty or fail. ``FROM_V7`` lists the windows taken
  from v7.1.4's capture instead (``"from": "v7.1.4"`` in windows.json), because the C UI needs a
  template for it: 14 (Store: v10's generator needs the store API). Window 5 (Account) stays
  v10's own, empty: v10 opens its login popup for a guest instead of the window, and the C UI
  generates the signed-in one (v7's guest form carried "Register or Login" and v7's recovery
  address, neither of which v10 shows).
- ``UNUSED`` slots: v10's empty ``windows[]`` entries, plus 28 (v7's Partner Dashboard, which
  v10 dropped).
- ``<canvas id="classPreviewCanvas">``: v10 moves the menu's class preview canvas into the
  loadout windows (``#charPreviewSlot``). The capture holds that one element in the window
  markup; the templates drop it, so the page keeps one ``#classPreviewCanvas`` (the menu's,
  which client/src/ui/menu_ui.c positions).
"""

import gzip
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import windows_v7 as w7  # noqa: E402  (trim/drop helpers, client_download_template, _clean)

SOURCE = os.path.join(HERE, "krunker-windows-v10.json.gz")
UNUSED = {"11", "12", "22", "28", "29"}
FROM_V7 = {"14": "Store: needs the store API"}
CANVAS_RE = re.compile(r'<canvas id="classPreviewCanvas"[^>]*>\s*</canvas>')


def load_capture(path=SOURCE):
    with gzip.open(path, "rt", encoding="utf-8") as fh:
        return json.load(fh)


def pack(windows_json, path=SOURCE):
    """The capture's windows.json -> krunker-windows-v10.json.gz (reproducible bytes)."""
    with open(windows_json, encoding="utf-8") as fh:
        data = json.load(fh)
    for key in ("windows", "settings", "controls"):
        if key not in data:
            raise SystemExit("windows_v10.py: %s has no %r (capture_v10_live.js output?)" % (windows_json, key))
    text = json.dumps(data, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
    with open(path, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
        gz.write(text.encode("utf-8"))
    return path


def _clean(html):
    return CANVAS_RE.sub("", w7._clean(html))


def split_windows_v10(out_dir, write_if_changed, log, path=SOURCE):
    data = load_capture(path)
    v7 = w7.load_capture()
    index = {"windows": {}, "settings": {}, "controls": {}}
    files = []
    written = 0
    os.makedirs(out_dir, exist_ok=True)

    def emit(name, html):
        nonlocal written
        if write_if_changed(os.path.join(out_dir, name), html):
            written += 1
        files.append((name, len(html.encode("utf-8"))))

    for wid, win in sorted(data["windows"].items(), key=lambda kv: int(kv[0])):
        if wid in UNUSED:
            continue
        source = None
        if wid in FROM_V7:
            win, source = v7["windows"].get(wid, {}), "v7.1.4"
        if win.get("error"):
            raise SystemExit("windows_v10.py: window %s failed in the capture (%s); add it to FROM_V7 or UNUSED"
                             % (wid, win["error"].splitlines()[0]))
        html = _clean(win.get("html", ""))
        trimmed = 0
        if 'id="%s"' % w7.PICKER_LIST in html and wid not in ("1", "7"):
            html, trimmed = w7.trim_children(html, w7.PICKER_LIST, w7.ITEM_KEEP)
        if wid == "1":
            html = w7.drop_per_weapon_rows(html)
        if wid == "8":
            html = w7.empty_big_blocks(html, '<div class="hostTb2 hide"', w7.HOST_TAB_MAX)
        if wid == w7.CLIENT_WINDOW:
            html = w7.client_download_template(html)
        name = "%s.html" % wid
        emit(name, html)
        entry = {"header": win.get("header") or "", "sticky": bool(win.get("sticky")), "file": name,
                 "label": win.get("label"), "cls": win.get("cls") or "", "style": win.get("style") or ""}
        if trimmed:
            entry["trimmed"] = trimmed
        if source:
            entry["from"] = source
        index["windows"][wid] = entry

    first = data["windows"].get("1", {})
    s = {"header": first.get("header") or "", "sticky": bool(first.get("sticky")), "cls": first.get("cls") or "",
         "style": first.get("style") or ""}
    for view in ("basic", "advanced"):
        tabs = []
        for t, tab in enumerate(data["settings"].get(view, [])):
            if tab.get("error"):
                raise SystemExit("windows_v10.py: settings %s/%s failed in the capture" % (view, tab.get("name")))
            name = "settings-%s-%d.html" % (view, t)
            emit(name, w7.drop_per_weapon_rows(_clean(tab["html"])))
            tabs.append({"name": tab["name"], "file": name})
        s[view] = tabs
    index["settings"] = s

    seven = data["windows"].get("7", {})
    c = {"header": seven.get("header") or "", "sticky": bool(seven.get("sticky")), "cls": seven.get("cls") or "",
         "style": seven.get("style") or "", "tabs": []}
    for t, tab in enumerate(data["controls"]):
        if tab.get("error"):
            raise SystemExit("windows_v10.py: controls %s failed in the capture" % tab.get("name"))
        name = "controls-%d.html" % t
        emit(name, _clean(tab["html"]))
        c["tabs"].append({"name": tab["name"], "file": name})
    index["controls"] = c

    text = json.dumps(index, indent=2, ensure_ascii=False) + "\n"
    if write_if_changed(os.path.join(out_dir, "windows.json"), text):
        written += 1
    files.append(("windows.json", len(text.encode("utf-8"))))
    total = sum(size for _, size in files)
    log("windows (v10): %d files (%d bytes) from %s, %d rewritten (%s from v7.1.4)"
        % (len(files), total, os.path.basename(path), written, ", ".join(sorted(FROM_V7, key=int))))
    return index, files, written


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "pack":
        print(pack(sys.argv[2]))
    else:
        sys.exit("usage: python3 -I tools/ui-import/windows_v10.py pack <stage>/v10-capture/windows.json")
