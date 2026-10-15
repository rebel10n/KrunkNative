"""A private Xvfb and a watcher of its top-level windows, for tests that must see where (and whether)
a window is mapped: the client window opening centred (client/tests/window_center_e2e.py), the
captcha webview staying hidden (server/tests/krunker_official_e2e.py).

    xvfb = PrivateXvfb("1280x800x24")   # its own display number (-displayfd), no TCP, killed by stop()
    watch = X11Watch(xvfb.display)       # libX11 over ctypes: no python-xlib needed
    ... run the program with DISPLAY=xvfb.display ...
    watch.maps()                         # every MapNotify of a top-level window, with its geometry then
    watch.moves_after_map(win)           # ConfigureNotify position changes after that map
    watch.stop(); xvfb.stop()

There is no window manager on the display, so a window maps exactly where its program put it: a
program that maps first and moves after shows up as a map at one place and a move to another.
The watcher selects SubstructureNotify on the root: CreateNotify (initial geometry), ConfigureNotify,
MapNotify, UnmapNotify and DestroyNotify of every top-level window.
"""
import ctypes
import ctypes.util
import os
import select
import subprocess
import threading
import time

CREATE_NOTIFY, DESTROY_NOTIFY, UNMAP_NOTIFY, MAP_NOTIFY, CONFIGURE_NOTIFY = 16, 17, 18, 19, 22
SUBSTRUCTURE_NOTIFY_MASK = 1 << 19

c_window = ctypes.c_ulong


class _Any(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p)]


class _Create(ctypes.Structure):
    _fields_ = _Any._fields_ + [("parent", c_window), ("window", c_window), ("x", ctypes.c_int), ("y", ctypes.c_int),
                                ("width", ctypes.c_int), ("height", ctypes.c_int), ("border_width", ctypes.c_int),
                                ("override_redirect", ctypes.c_int)]


class _Configure(ctypes.Structure):
    _fields_ = _Any._fields_ + [("event", c_window), ("window", c_window), ("x", ctypes.c_int), ("y", ctypes.c_int),
                                ("width", ctypes.c_int), ("height", ctypes.c_int), ("border_width", ctypes.c_int),
                                ("above", c_window), ("override_redirect", ctypes.c_int)]


class _Map(ctypes.Structure):
    _fields_ = _Any._fields_ + [("event", c_window), ("window", c_window), ("override_redirect", ctypes.c_int)]


class _XEvent(ctypes.Union):
    _fields_ = [("type", ctypes.c_int), ("create", _Create), ("configure", _Configure), ("map", _Map),
                ("pad", ctypes.c_long * 24)]


def _libx11():
    name = ctypes.util.find_library("X11") or "libX11.so.6"
    return ctypes.CDLL(name)


class PrivateXvfb:
    """Xvfb on a display number of its own (-displayfd), never the user's; stop() kills it."""

    def __init__(self, screen="1280x800x24"):
        r, w = os.pipe()
        self.proc = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", screen, "-nolisten", "tcp", "-noreset"],
                                     pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        os.close(w)
        number = b""
        end = time.time() + 15
        while time.time() < end and not number.endswith(b"\n"):
            ready, _, _ = select.select([r], [], [], 0.5)
            if ready:
                chunk = os.read(r, 16)
                if not chunk:
                    break
                number += chunk
        os.close(r)
        if not number.strip():
            self.stop()
            raise RuntimeError("Xvfb did not report its display")
        self.display = ":" + number.decode().strip()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class X11Watch:
    def __init__(self, display):
        self.x = _libx11()
        self.x.XOpenDisplay.restype = ctypes.c_void_p
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x.XDefaultRootWindow.restype = c_window
        self.x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x.XSelectInput.argtypes = [ctypes.c_void_p, c_window, ctypes.c_long]
        self.x.XPending.argtypes = [ctypes.c_void_p]
        self.x.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.POINTER(_XEvent)]
        self.x.XConnectionNumber.argtypes = [ctypes.c_void_p]
        self.x.XFetchName.argtypes = [ctypes.c_void_p, c_window, ctypes.POINTER(ctypes.c_char_p)]
        self.x.XFree.argtypes = [ctypes.c_void_p]
        self.x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.x.XFlush.argtypes = [ctypes.c_void_p]
        # a window gone between its event and our query must not end the test process
        self._handler = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(lambda d, e: 0)
        self.x.XSetErrorHandler(self._handler)
        self.dpy = self.x.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError("cannot open display " + display)
        self.root = self.x.XDefaultRootWindow(self.dpy)
        self.x.XSelectInput(self.dpy, self.root, SUBSTRUCTURE_NOTIFY_MASK)
        self.x.XFlush(self.dpy)
        self.lock = threading.Lock()
        self.geom = {}       # window -> [x, y, w, h] (latest)
        self.mapped = {}     # window -> currently mapped
        self.events = []     # (t, kind, window, x, y, w, h, name, override_redirect)
        self._stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _name(self, win):
        p = ctypes.c_char_p()
        if self.x.XFetchName(self.dpy, win, ctypes.byref(p)) and p.value is not None:
            name = p.value.decode(errors="replace")
            self.x.XFree(p)
            return name
        return ""

    def _run(self):
        fd = self.x.XConnectionNumber(self.dpy)
        ev = _XEvent()
        while not self._stop:
            while self.x.XPending(self.dpy):
                self.x.XNextEvent(self.dpy, ctypes.byref(ev))
                t = time.time()
                with self.lock:
                    if ev.type == CREATE_NOTIFY:
                        c = ev.create
                        self.geom[c.window] = [c.x, c.y, c.width, c.height]
                        self.events.append((t, "create", c.window, c.x, c.y, c.width, c.height, "", c.override_redirect))
                    elif ev.type == CONFIGURE_NOTIFY:
                        c = ev.configure
                        if c.event != self.root:
                            continue
                        self.geom[c.window] = [c.x, c.y, c.width, c.height]
                        self.events.append((t, "configure", c.window, c.x, c.y, c.width, c.height, "", c.override_redirect))
                    elif ev.type == MAP_NOTIFY:
                        m = ev.map
                        if m.event != self.root:
                            continue
                        g = self.geom.get(m.window, [0, 0, 0, 0])
                        self.mapped[m.window] = True
                        self.events.append((t, "map", m.window, g[0], g[1], g[2], g[3], self._name(m.window), m.override_redirect))
                    elif ev.type in (UNMAP_NOTIFY, DESTROY_NOTIFY):
                        m = ev.map
                        if m.event != self.root:
                            continue
                        self.mapped[m.window] = False
                        self.events.append((t, "unmap" if ev.type == UNMAP_NOTIFY else "destroy", m.window, 0, 0, 0, 0, "", 0))
            select.select([fd], [], [], 0.05)

    def maps(self, include_override=False):
        """Every map of a top-level window: dicts with window, name, x, y, w, h, t."""
        with self.lock:
            return [{"window": e[2], "name": e[7], "x": e[3], "y": e[4], "w": e[5], "h": e[6], "t": e[0]}
                    for e in self.events if e[1] == "map" and (include_override or not e[8])]

    def moves_after_map(self, win):
        """Position changes of win after its first map: [(x, y), ...]."""
        with self.lock:
            out, seen, at = [], False, None
            for e in self.events:
                if e[2] != win:
                    continue
                if e[1] == "map" and not seen:
                    seen, at = True, (e[3], e[4])
                elif seen and e[1] == "configure" and (e[3], e[4]) != at:
                    out.append((e[3], e[4]))
                    at = (e[3], e[4])
            return out

    def stop(self):
        self._stop = True
        self.thread.join(timeout=2)
        if self.dpy:
            self.x.XCloseDisplay(self.dpy)
            self.dpy = None
