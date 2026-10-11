#!/usr/bin/env bash
# Builds ANGLE (libEGL + libGLESv2) from a pinned commit and stages it as an ANGLE_DIR for
# -DKRUNKNATIVE_GL_BACKEND=angle (README.md "ANGLE / Metal"):
#
#   <prefix>/lib/libEGL.{dylib,so}  <prefix>/lib/libGLESv2.{dylib,so}   (+ SwiftShader Vulkan ICD
#   <prefix>/include/{EGL,KHR,GLES2,GLES3}                                when --swiftshader)
#   <prefix>/ANGLE_BUILD                                                 commit + gn args (cache key)
#
# Usage: tools/angle/build-angle.sh <prefix> [--work <dir>] [--cpu x64|arm64] [--swiftshader] [--jobs N]
#
#   --work        checkout + depot_tools + out/ (default: <prefix>.work); several GB, reusable
#   --cpu         target_cpu (default: host)
#   --swiftshader also build ANGLE's Vulkan backend with SwiftShader (CPU Vulkan): renders where
#                 no GPU backend exists (macOS VMs without Metal, GPU-less CI); slower to build
#
# macOS: Metal (default backend on Apple) + OpenGL (CGL, fallback) backends; dylib install names
# are @rpath/lib*.dylib so they load from next to client_bin (CMake sets the rpath).
# Linux: Vulkan + OpenGL (EGL/GLX) backends, for testing the ANGLE code path off a Mac.
#
# Pinned versions (bump together, then the CI cache key changes with this file):
#   ANGLE        chromium/8037 branch head (Chrome 154 stable)
#   depot_tools  a commit that bootstraps on Python 3.9 (macOS Command Line Tools)
set -euo pipefail

ANGLE_COMMIT="${ANGLE_COMMIT:-b52e7ee002499a95cb740fb74ed729fd2ecb906d}"
DEPOT_TOOLS_COMMIT="${DEPOT_TOOLS_COMMIT:-a03dfbf7f2d1cefa2795d81d59ca4f16c8616a8f}"

usage() { sed -n '2,20p' "$0" >&2; exit 2; }
[ $# -ge 1 ] || usage
PREFIX="$1"; shift
WORK="" CPU="" SWIFTSHADER=0 JOBS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --work) WORK="$2"; shift 2 ;;
        --cpu) CPU="$2"; shift 2 ;;
        --swiftshader) SWIFTSHADER=1; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) usage ;;
    esac
done
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
WORK="${WORK:-$PREFIX.work}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"

case "$(uname -s)" in
    Darwin) OS=mac; SO=dylib ;;
    Linux) OS=linux; SO=so ;;
    *) echo "build-angle.sh: unsupported host $(uname -s)" >&2; exit 1 ;;
esac
if [ -z "$CPU" ]; then
    case "$(uname -m)" in arm64|aarch64) CPU=arm64 ;; *) CPU=x64 ;; esac
fi

GN_ARGS="is_debug=false is_component_build=false target_cpu=\"$CPU\" symbol_level=0
angle_build_tests=false angle_enable_null=false angle_enable_wgpu=false angle_has_frame_capture=false
angle_enable_gl=true treat_warnings_as_errors=false"
if [ "$OS" = mac ]; then
    GN_ARGS="$GN_ARGS angle_enable_metal=true mac_deployment_target=\"11.0\""
    if [ "$SWIFTSHADER" = 1 ]; then
        GN_ARGS="$GN_ARGS angle_enable_vulkan=true angle_enable_swiftshader=true"
    else
        GN_ARGS="$GN_ARGS angle_enable_vulkan=false angle_enable_swiftshader=false"
    fi
else
    GN_ARGS="$GN_ARGS angle_enable_vulkan=true angle_enable_swiftshader=$([ "$SWIFTSHADER" = 1 ] && echo true || echo false)"
fi
GN_ARGS="$(echo "$GN_ARGS" | tr '\n' ' ')"
STAMP="angle=$ANGLE_COMMIT depot_tools=$DEPOT_TOOLS_COMMIT os=$OS gn: $GN_ARGS"

if [ -f "$PREFIX/ANGLE_BUILD" ] && [ "$(cat "$PREFIX/ANGLE_BUILD")" = "$STAMP" ] && [ -f "$PREFIX/lib/libGLESv2.$SO" ]; then
    echo "ANGLE already staged in $PREFIX ($ANGLE_COMMIT)"
    exit 0
fi

# depot_tools at a fixed commit; DEPOT_TOOLS_UPDATE=0 keeps gclient from moving it.
export DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0 GCLIENT_PY3=1
if [ ! -d "$WORK/depot_tools/.git" ]; then
    git init -q "$WORK/depot_tools"
    git -C "$WORK/depot_tools" remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
git -C "$WORK/depot_tools" fetch -q --depth 1 origin "$DEPOT_TOOLS_COMMIT"
git -C "$WORK/depot_tools" checkout -q --force FETCH_HEAD
export PATH="$WORK/depot_tools:$PATH"
# With updates off nothing else bootstraps depot_tools' own Python/CIPD (python3_bin_reldir.txt).
"$WORK/depot_tools/ensure_bootstrap"

# ANGLE standalone checkout at the pinned commit (the .gclient solution URL carries the
# revision, so `gclient sync` never moves to the branch tip).
mkdir -p "$WORK/angle"
cat > "$WORK/angle/.gclient" <<EOF
solutions = [{
    "name": ".",
    "url": "https://chromium.googlesource.com/angle/angle.git@$ANGLE_COMMIT",
    "deps_file": "DEPS",
    "managed": False,
    # No OpenCL / WebGPU (dawn) backends: skips ~4 GB of their sources (llvm, dawn).
    "custom_vars": {"checkout_angle_cl_deps": False, "checkout_angle_dawn_deps": False},
}]
EOF
if [ ! -d "$WORK/angle/.git" ]; then
    git init -q "$WORK/angle"
    git -C "$WORK/angle" remote add origin https://chromium.googlesource.com/angle/angle.git
fi
git -C "$WORK/angle" fetch -q --depth 1 origin "$ANGLE_COMMIT"
git -C "$WORK/angle" checkout -q --force FETCH_HEAD
(cd "$WORK/angle" && gclient sync --no-history --shallow --force --delete_unversioned_trees --nohooks --jobs 8)
# ANGLE's DEPS vendors a second depot_tools (third_party/depot_tools) that its hooks run.
if [ -x "$WORK/angle/third_party/depot_tools/ensure_bootstrap" ]; then
    "$WORK/angle/third_party/depot_tools/ensure_bootstrap"
fi
(cd "$WORK/angle" && gclient runhooks)

if [ "$OS" = mac ]; then
    # Chromium's mac toolchain wants full Xcode (`xcodebuild -version`, SDKs under
    # Platforms/MacOSX.platform). On a Command Line Tools-only Mac give it an Xcode-shaped
    # view of the CLT (SDK + toolchain symlinks, three tiny shims) instead of requiring Xcode.
    if ! xcodebuild -version > /dev/null 2>&1; then
        clt="$(/usr/bin/xcode-select -p)"
        sdk_version="$(xcrun --sdk macosx --show-sdk-version)"
        fake="$WORK/clt-xcode" shim="$WORK/clt-shim"
        mkdir -p "$fake/Platforms/MacOSX.platform/Developer/SDKs" "$fake/Toolchains/XcodeDefault.xctoolchain/usr" "$shim"
        ln -sfn "$(xcrun --sdk macosx --show-sdk-path)" "$fake/Platforms/MacOSX.platform/Developer/SDKs/MacOSX$sdk_version.sdk"
        ln -sfn "$clt/usr/bin" "$fake/Toolchains/XcodeDefault.xctoolchain/usr/bin"
        printf '#!/bin/sh\ncase "$*" in *-version*) printf "Xcode %s\\nBuild version CLT\\n"; exit 0 ;; esac\necho "xcodebuild (CLT shim): $*: needs Xcode" >&2; exit 1\n' "$sdk_version" > "$shim/xcodebuild"
        printf '#!/bin/sh\ncase "$*" in -p|*-print-path*) echo "%s"; exit 0 ;; esac\nexec /usr/bin/xcode-select "$@"\n' "$fake" > "$shim/xcode-select"
        printf '#!/bin/sh\ncase "$*" in *--show-sdk-platform-path*) echo "%s/Platforms/MacOSX.platform"; exit 0 ;; esac\nexec /usr/bin/xcrun "$@"\n' "$fake" > "$shim/xcrun"
        chmod +x "$shim"/*
        export PATH="$shim:$PATH"
        echo "build-angle.sh: no Xcode, building against the Command Line Tools SDK $sdk_version"
    fi
    # No Metal compiler (Command Line Tools, or Xcode 26+ without the MetalToolchain component):
    # ANGLE then compiles its internal Metal shaders at runtime (its iOS / cross-compile path)
    # instead of embedding a prebuilt metallib. Patched in the checkout, reset by the checkout above.
    if ! xcrun metal -v > /dev/null 2>&1; then
        perl -0pi -e 's/metal_internal_shader_compilation_supported =\s*\n/metal_internal_shader_compilation_supported = false && /' \
            "$WORK/angle/src/libANGLE/renderer/metal/metal_backend.gni"
        echo "build-angle.sh: no Metal toolchain, ANGLE's internal Metal shaders compile at runtime"
    fi
fi

OUT="$WORK/angle/out/kn-$OS-$CPU"
(cd "$WORK/angle" && gn gen "$OUT" --args="$GN_ARGS")
TARGETS="libEGL libGLESv2"
[ "$SWIFTSHADER" = 1 ] && TARGETS="$TARGETS third_party/SwiftShader/src/Vulkan:swiftshader_libvulkan third_party/SwiftShader/src/Vulkan:icd_file"
# shellcheck disable=SC2086
autoninja -C "$OUT" ${JOBS:+-j "$JOBS"} $TARGETS

rm -rf "${PREFIX:?}/lib" "${PREFIX:?}/include"
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp "$OUT/libEGL.$SO" "$OUT/libGLESv2.$SO" "$PREFIX/lib/"
if [ "$SWIFTSHADER" = 1 ]; then
    cp "$OUT/libvk_swiftshader.$SO" "$PREFIX/lib/"
    cp "$OUT/vk_swiftshader_icd.json" "$PREFIX/lib/" 2>/dev/null || true
    # ANGLE's Vulkan backend loads the Vulkan loader from its own directory.
    for loader in "$OUT"/libvulkan.*; do [ -e "$loader" ] && cp "$loader" "$PREFIX/lib/"; done
fi
for dir in EGL KHR GLES2 GLES3; do cp -R "$WORK/angle/include/$dir" "$PREFIX/include/"; done
if [ "$OS" = mac ]; then
    # @rpath install names (Chromium's toolchain already uses them), then an ad-hoc signature:
    # arm64 refuses unsigned or stale-signed code, x86_64 builds come out unsigned.
    for lib in "$PREFIX"/lib/*.dylib; do
        id="@rpath/$(basename "$lib")"
        [ "$(otool -D "$lib" | tail -1)" = "$id" ] || install_name_tool -id "$id" "$lib"
        codesign --force --sign - "$lib"
    done
    otool -L "$PREFIX/lib/libEGL.dylib" "$PREFIX/lib/libGLESv2.dylib"
fi
echo "$STAMP" > "$PREFIX/ANGLE_BUILD"
echo "ANGLE $ANGLE_COMMIT staged in $PREFIX"
