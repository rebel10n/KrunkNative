#!/usr/bin/env python3
"""Compiles (and, for the surface pairs, links) the shaders client/tests/mod_shader_test writes with --dump, on Mesa's
llvmpipe through EGL (surfaceless, no window): GLSL ES 3.00 on an OpenGL ES 3 context and GLSL 4.50 core on a desktop
core context, the two languages the client builds them in (web / ANGLE and desktop). glslangValidator
(tools/modshader/check_glsl.sh) checks the post fragments where it is installed; this needs only Mesa.

    tools/modshader/check_gl.py <mod_shader_test binary>

Software GL only: LIBGL_ALWAYS_SOFTWARE=1 is forced and the run stops (exit 77, a skip) unless the renderer is llvmpipe.
"""
import ctypes
import os
import subprocess
import sys
import tempfile

os.environ["LIBGL_ALWAYS_SOFTWARE"] = "1"
os.environ.setdefault("EGL_PLATFORM", "surfaceless")

EGL_PLATFORM_SURFACELESS_MESA = 0x31DD
EGL_NONE, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT = 0x3038, 0x3033, 0x0001
EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_OPENGL_BIT = 0x3040, 0x0040, 0x0008
EGL_OPENGL_ES_API, EGL_OPENGL_API = 0x30A0, 0x30A2
EGL_CONTEXT_MAJOR_VERSION, EGL_CONTEXT_MINOR_VERSION = 0x3098, 0x30FB
EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x30FD, 0x1
GL_VERTEX_SHADER, GL_FRAGMENT_SHADER = 0x8B31, 0x8B30
GL_COMPILE_STATUS, GL_LINK_STATUS, GL_RENDERER = 0x8B81, 0x8B82, 0x1F01


def skip(msg):
    print("check_gl: " + msg + ", skipped")
    sys.exit(77)


try:
    egl = ctypes.CDLL("libEGL.so.1")
except OSError:
    skip("no libEGL")
egl.eglGetProcAddress.restype = ctypes.c_void_p
egl.eglGetProcAddress.argtypes = [ctypes.c_char_p]


def proc(name, restype, *args):
    addr = egl.eglGetProcAddress(name.encode())
    if not addr:
        raise RuntimeError("no " + name)
    return ctypes.CFUNCTYPE(restype, *args)(addr)


vp, i32, u32 = ctypes.c_void_p, ctypes.c_int, ctypes.c_uint
get_display = proc("eglGetPlatformDisplayEXT", vp, i32, vp, vp)
egl.eglInitialize.argtypes = [vp, vp, vp]
egl.eglChooseConfig.argtypes = [vp, ctypes.POINTER(i32), ctypes.POINTER(vp), i32, ctypes.POINTER(i32)]
egl.eglBindAPI.argtypes = [u32]
egl.eglCreateContext.restype = vp
egl.eglCreateContext.argtypes = [vp, vp, vp, ctypes.POINTER(i32)]
egl.eglMakeCurrent.argtypes = [vp, vp, vp, vp]
egl.eglDestroyContext.argtypes = [vp, vp]

dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, None, None)
if not dpy or not egl.eglInitialize(dpy, None, None):
    skip("no surfaceless EGL display")


def context(api, renderable, attribs):
    cfg_attr = (i32 * 5)(EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, renderable, EGL_NONE)
    cfg, n = vp(), i32(0)
    if not egl.eglChooseConfig(dpy, cfg_attr, ctypes.byref(cfg), 1, ctypes.byref(n)) or n.value < 1:
        return None
    egl.eglBindAPI(api)
    ctx_attr = (i32 * (len(attribs) + 1))(*attribs, EGL_NONE)
    ctx = egl.eglCreateContext(dpy, cfg, None, ctx_attr)
    if not ctx or not egl.eglMakeCurrent(dpy, None, None, ctx):
        return None
    return ctx


class GL:
    def __init__(self):
        self.create_shader = proc("glCreateShader", u32, u32)
        self.shader_source = proc("glShaderSource", None, u32, i32, ctypes.POINTER(ctypes.c_char_p), vp)
        self.compile_shader = proc("glCompileShader", None, u32)
        self.get_shader_iv = proc("glGetShaderiv", None, u32, u32, ctypes.POINTER(i32))
        self.shader_log = proc("glGetShaderInfoLog", None, u32, i32, vp, ctypes.c_char_p)
        self.delete_shader = proc("glDeleteShader", None, u32)
        self.create_program = proc("glCreateProgram", u32)
        self.attach = proc("glAttachShader", None, u32, u32)
        self.link = proc("glLinkProgram", None, u32)
        self.get_program_iv = proc("glGetProgramiv", None, u32, u32, ctypes.POINTER(i32))
        self.program_log = proc("glGetProgramInfoLog", None, u32, i32, vp, ctypes.c_char_p)
        self.delete_program = proc("glDeleteProgram", None, u32)
        get_string = proc("glGetString", ctypes.c_char_p, u32)
        self.renderer = (get_string(GL_RENDERER) or b"").decode()

    def compile(self, kind, text):
        sh = self.create_shader(kind)
        src = (ctypes.c_char_p * 1)(text.encode())
        self.shader_source(sh, 1, src, None)
        self.compile_shader(sh)
        ok = i32(0)
        self.get_shader_iv(sh, GL_COMPILE_STATUS, ctypes.byref(ok))
        if ok.value:
            return sh, ""
        log = ctypes.create_string_buffer(2048)
        self.shader_log(sh, 2048, None, log)
        self.delete_shader(sh)
        return 0, log.value.decode(errors="replace")

    def link_pair(self, vert, frag):
        p = self.create_program()
        self.attach(p, vert)
        self.attach(p, frag)
        self.link(p)
        ok = i32(0)
        self.get_program_iv(p, GL_LINK_STATUS, ctypes.byref(ok))
        log = ctypes.create_string_buffer(2048)
        if not ok.value:
            self.program_log(p, 2048, None, log)
        self.delete_program(p)
        return ok.value, log.value.decode(errors="replace")


def desktop(text):
    lines = text.split("\n")
    lines[0] = "#version 450 core"
    return "\n".join(lines)


def check(gl, root, convert, label):
    fails = total = 0
    post = sorted(f for f in os.listdir(root) if f.endswith(".frag"))
    for name in post:
        total += 1
        sh, log = gl.compile(GL_FRAGMENT_SHADER, convert(open(os.path.join(root, name)).read()))
        if sh:
            gl.delete_shader(sh)
        else:
            fails += 1
            print("== %s post/%s\n%s" % (label, name, log.strip()[:400]))
    sdir = os.path.join(root, "surface")
    names = sorted(f[:-5] for f in os.listdir(sdir) if f.endswith(".vert")) if os.path.isdir(sdir) else []
    for name in names:
        total += 1
        vs, vlog = gl.compile(GL_VERTEX_SHADER, convert(open(os.path.join(sdir, name + ".vert")).read()))
        fs, flog = gl.compile(GL_FRAGMENT_SHADER, convert(open(os.path.join(sdir, name + ".frag")).read()))
        ok, llog = (gl.link_pair(vs, fs) if vs and fs else (0, ""))
        for sh in (vs, fs):
            if sh:
                gl.delete_shader(sh)
        if not ok:
            fails += 1
            print("== %s surface/%s\n%s" % (label, name, (vlog or flog or llog).strip()[:400]))
    print("check_gl: %s: %d/%d programs valid" % (label, total - fails, total))
    return fails, total


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    with tempfile.TemporaryDirectory(prefix="kn-modshader-gl.", dir=os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")) as d:
        if subprocess.run([sys.argv[1], "--dump", d], stdout=subprocess.DEVNULL).returncode:
            print("check_gl: the dump failed")
            return 1
        fails = 0
        es = context(EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, [EGL_CONTEXT_MAJOR_VERSION, 3])
        if not es:
            skip("no OpenGL ES 3 context")
        gl = GL()
        if "llvmpipe" not in gl.renderer:
            skip("renderer is %r, not llvmpipe" % gl.renderer)
        fails += check(gl, d, lambda t: t, "GLSL ES 3.00")[0]
        egl.eglMakeCurrent(dpy, None, None, None)
        egl.eglDestroyContext(dpy, es)
        core = context(EGL_OPENGL_API, EGL_OPENGL_BIT, [EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                                         EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT])
        if core:
            fails += check(GL(), d, desktop, "GLSL 4.50")[0]
            egl.eglMakeCurrent(dpy, None, None, None)
            egl.eglDestroyContext(dpy, core)
        else:
            print("check_gl: no OpenGL 4.5 core context, GLSL 4.50 not checked")
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
