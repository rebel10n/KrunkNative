cmake_minimum_required(VERSION 3.10)
project(KrunkNative)

set(CMAKE_C_STANDARD 11)

if (NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif ()

# HTML UI. The in-house HTML/CSS engine (client/src/html/engine + client/lib/lexbor +
# FreeType; client/src/html/engine/README.md, doc/html-ui-engine.md) renders ui/index.html
# natively, driven by the C UI logic in client/src/ui (menu_ui.c, hud_ui.c, ...) through
# the DOM-ops API (client/include/html_dom.h). No JavaScript runs in the native client.
# OFF builds a minimal client without any HTML UI (client/src/html/stub.c: the DOM ops
# are no-ops and html_ui_available() is 0). The web build ignores the option: the
# browser renders the same page and client/src/html/web_dom.c implements the DOM ops.
option(KRUNKNATIVE_HTML_ENGINE "Build the in-house HTML/CSS UI engine (OFF: no HTML UI)" ON)

# Adds client/src/html/<file> sources to a target together with the include paths they
# need. With ENGINE, KRUNKNATIVE_HTML_ENGINE=1 is defined for the whole target: the
# engine backend (or web_dom.c on the web) is compiled in instead of stub.c.
function(krunknative_add_html_sources target)
    cmake_parse_arguments(ARG "ENGINE" "" "SOURCES" ${ARGN})
    foreach (src IN LISTS ARG_SOURCES)
        target_sources(${target} PRIVATE "${CMAKE_SOURCE_DIR}/client/src/html/${src}")
    endforeach ()
    if (ARG_ENGINE)
        target_compile_definitions(${target} PRIVATE KRUNKNATIVE_HTML_ENGINE=1)
    endif ()
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/client/include" "${CMAKE_SOURCE_DIR}/client/src/html")
endfunction ()

# The engine proper: client/src/html/engine/*.c (native only).
file(GLOB HTML_ENGINE_SOURCES client/src/html/engine/*.c)
file(GLOB HTML_ENGINE_HEADERS client/src/html/engine/*.h)

include_directories(shared/include)

file(GLOB_RECURSE SHARED_SOURCES shared/src/*.c)
file(GLOB_RECURSE SHARED_HEADERS shared/include/*.h)

# Build version (shared/include/build_info.h): cmake/kn_version.cmake writes
# generated/kn_version.h (git describe, commit, commit time, build time) at configure time and
# at every build; only shared/src/build_info.c reads it, in library kn_build_info, which the
# client, server_bin and kn_brain link. CI builds without git pass the KRUNKNATIVE_VERSION_*
# values (a non-empty value wins over git).
set(KRUNKNATIVE_VERSION_DESCRIBE "" CACHE STRING "Build version override (git describe)")
set(KRUNKNATIVE_VERSION_COMMIT "" CACHE STRING "Build commit override (hex)")
set(KRUNKNATIVE_VERSION_COMMIT_TIME "" CACHE STRING "Build commit time override (unix seconds)")
set(KRUNKNATIVE_VERSION_BUILD_TIME "" CACHE STRING "Build time override (ISO-8601 UTC)")
set(KN_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/kn_version.h")
set(KN_VERSION_ARGS
    "-DKN_SRC_DIR=${CMAKE_SOURCE_DIR}" "-DKN_OUT=${KN_VERSION_HEADER}"
    "-DKN_OVR_DESCRIBE=${KRUNKNATIVE_VERSION_DESCRIBE}" "-DKN_OVR_COMMIT=${KRUNKNATIVE_VERSION_COMMIT}"
    "-DKN_OVR_COMMIT_TIME=${KRUNKNATIVE_VERSION_COMMIT_TIME}" "-DKN_OVR_BUILD_TIME=${KRUNKNATIVE_VERSION_BUILD_TIME}")
execute_process(COMMAND "${CMAKE_COMMAND}" ${KN_VERSION_ARGS} -P "${CMAKE_SOURCE_DIR}/cmake/kn_version.cmake")
add_custom_target(kn_version
    COMMAND "${CMAKE_COMMAND}" ${KN_VERSION_ARGS} -P "${CMAKE_SOURCE_DIR}/cmake/kn_version.cmake"
    BYPRODUCTS "${KN_VERSION_HEADER}"
    COMMENT "Build version (kn_version.h)"
    VERBATIM)
list(FILTER SHARED_SOURCES EXCLUDE REGEX "/shared/src/build_info\\.c$")
add_library(kn_build_info STATIC shared/src/build_info.c shared/include/build_info.h)
target_include_directories(kn_build_info PRIVATE "${CMAKE_BINARY_DIR}/generated" shared/lib/cJSON)
add_dependencies(kn_build_info kn_version)

set(CLIENT_BINARY client_bin)
file(GLOB_RECURSE CLIENT_SOURCES client/src/*.c)
file(GLOB_RECURSE CLIENT_HEADERS client/include/*.h)

# client/src/html is added per target below (engine, web_dom.c or stub.c); the C UI
# logic in client/src/ui is part of every client build.
list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/client/src/html/")

# Network transport: TCP on a background thread natively, WebSocket on the web.
if (EMSCRIPTEN)
    list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/client/src/net/net\\.c$")
else ()
    list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/client/src/net/net_web\\.c$")
endif ()
# Official-server CHEAT_CHECK eval (client/src/net/krunker_cheat.c): the page's own eval on
# the web, the vendored QuickJS sandbox (krunker_cheat_qjs.c + kn_quickjs below) natively.
if (EMSCRIPTEN)
    list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/client/src/net/krunker_cheat_qjs\\.c$")
endif ()

# Native captcha webview (client/include/kn_webview.h, client/src/ui/kn_webview.c): krunker.io's
# Cloudflare Turnstile shown in a WebKitGTK window for the player to solve (doc/v10/official-captcha.md).
# KN_WEBVIEW = AUTO (default: on when pkg-config finds webkit2gtk-4.1), ON (required) or OFF. Linux
# only; client_bin only (never server/ or shared/). kn_webview.c is then built as its own library so
# GTK's include paths stay out of the rest of the client; without it (macOS, Windows, the web build,
# OFF, not found) the same file compiles to its stub inside the client and the captcha keeps the
# system-browser / paste-box paths.
set(KN_WEBVIEW AUTO CACHE STRING "Native captcha webview (WebKitGTK): AUTO = on when webkit2gtk-4.1 is found, ON, OFF")
set(KN_WEBVIEW_GTK OFF)
string(TOUPPER "${KN_WEBVIEW}" KN_WEBVIEW_MODE)
if (KN_WEBVIEW_MODE MATCHES "^(OFF|0|FALSE|NO|N)$")
    message(STATUS "Webview: KN_WEBVIEW is OFF; the captcha uses the system browser / paste box")
elseif (EMSCRIPTEN OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (NOT EMSCRIPTEN)
        message(STATUS "Webview: no backend on ${CMAKE_SYSTEM_NAME} yet; the captcha uses the system browser / paste box")
    endif ()
else ()
    find_package(PkgConfig QUIET)
    if (PKG_CONFIG_FOUND)
        pkg_check_modules(KN_WEBKITGTK QUIET IMPORTED_TARGET webkit2gtk-4.1)
    endif ()
    if (KN_WEBKITGTK_FOUND)
        set(KN_WEBVIEW_GTK ON)
        list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/client/src/ui/kn_webview\\.c$")
        message(STATUS "Webview: WebKitGTK ${KN_WEBKITGTK_VERSION} (KN_WEBVIEW): krunker.io's captcha in a window")
    elseif (KN_WEBVIEW_MODE MATCHES "^(ON|1|TRUE|YES|Y)$")
        message(FATAL_ERROR "KN_WEBVIEW=ON needs webkit2gtk-4.1 (pkg-config webkit2gtk-4.1)")
    else ()
        message(STATUS "Webview: webkit2gtk-4.1 not found; the captcha uses the system browser / paste box")
    endif ()
endif ()

set(SERVER_BINARY server_bin)
file(GLOB_RECURSE SERVER_SOURCES server/src/*.c)
file(GLOB_RECURSE SERVER_HEADERS server/include/*.h)

# Server core (server/include/server.h): connections, packet dispatch, broadcast, the tick
# step, accounts and config. server_bin links it with its network frontend (the rest of
# server/src: TCP/UDP sockets, rcon, brain link, finder, CLI); the client links it as the
# offline sandbox server (server/src/sandbox.c) with the no-network backend below. No GL,
# sockets or threads of its own.
set(SERVER_CORE_SOURCES
    server/src/net.c
    server/src/accounts.c
    server/src/config.c
    server/src/log.c
    server/src/events.c
    server/src/server_core.c
    server/src/sandbox.c
    server/src/ks_server_host.c
    server/src/ks_storage.c
)
# What the core needs from the backend link in a build without it (the client): brain.c
# compiled without knsvc is the standalone stub; sandbox_backend.c stands in for the
# central auth and rcon hooks.
set(SERVER_CORE_STANDALONE_SOURCES
    server/src/brain.c
    server/src/sandbox_backend.c
)
list(REMOVE_ITEM SERVER_SOURCES ${CMAKE_SOURCE_DIR}/server/src/sandbox_backend.c)
foreach (src IN LISTS SERVER_CORE_SOURCES)
    list(REMOVE_ITEM SERVER_SOURCES ${CMAKE_SOURCE_DIR}/${src})
endforeach ()

# libopus (client/lib/opus, client/lib/opus/KRUNKNATIVE.md): voice chat's codec, compiled into
# every client (native and web) so that no package can ship without it. Static, float API,
# no DNN features (DRED / OSCE / deep PLC), no programs or tests; the server never links it.
macro(krunknative_add_opus)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_DRED OFF CACHE BOOL "" FORCE)
    set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
    set(OPUS_DEEP_PLC OFF CACHE BOOL "" FORCE)
    if (EMSCRIPTEN)
        set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE) # no SSE/NEON in wasm32 (portable C)
        set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
        set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
    endif ()
    if (MINGW)
        # MinGW has no libssp in the static -static builds (tools/release/build-client.sh)
        set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
        set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
    endif ()
    # the root include_directories(shared/include) would shadow celt's own headers (shared's
    # modes.h for celt/modes.h): opus compiles with its own include paths only
    get_directory_property(KN_OPUS_SAVED_INCLUDES INCLUDE_DIRECTORIES)
    set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "")
    add_subdirectory(client/lib/opus EXCLUDE_FROM_ALL)
    set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "${KN_OPUS_SAVED_INCLUDES}")
    # opus builds -O2 even in Debug trees: unoptimised it is several times slower (its own warning)
    if (NOT MSVC)
        target_compile_options(opus PRIVATE -O2)
    endif ()
endmacro()

if (EMSCRIPTEN)
    # ------------------------------------------------------------------
    # Web build (emcmake cmake ...): client only, output dist/web/client.js
    # + client.wasm. Server, replxx, GLAD and the native test are skipped;
    # GLFW and WebGL2 come from the emscripten ports (-sUSE_GLFW=3).
    #
    # FreeType: the vendored client/lib/freetype tree compiles cleanly with
    # emcc once its optional system dependencies (zlib/bzip2/png/harfbuzz/
    # brotli) are disabled, so the same sources are used natively and on the
    # web and no -sUSE_FREETYPE port is required.
    # ------------------------------------------------------------------
    set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)

    add_subdirectory(client/lib/freetype)
    add_subdirectory(shared/lib/cJSON)
    add_subdirectory(shared/lib/pcg_basic)

    set(CMAKE_EXECUTABLE_SUFFIX ".js")
    # -DWEB_DIST_DIR=<dir> assembles the site elsewhere (parallel/test builds)
    if(NOT WEB_DIST_DIR)
        set(WEB_DIST_DIR ${CMAKE_SOURCE_DIR}/dist/web)
    endif()

    add_executable(client ${CLIENT_HEADERS} ${CLIENT_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES}
        ${SERVER_CORE_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES}) # the offline sandbox server (server_sandbox.h)
    target_compile_definitions(client PRIVATE KRUNKNATIVE_CLIENT)
    target_include_directories(client PRIVATE client/include server/include)
    target_link_libraries(client PRIVATE freetype cJSON pcg_basic kn_build_info m)
    krunknative_add_opus()
    target_link_libraries(client PRIVATE opus) # voice chat: Opus decode of every clip (voice.c)
    target_compile_definitions(client PRIVATE KRUNKNATIVE_HAVE_OPUS=1)
    # the map editor AI panel's tools (tools/editor-mcp, in-process: client/src/editor/editor_ai_tools.c)
    include(tools/editor-mcp/core.cmake)
    kn_editor_mcp_embed(client)

    # HTML UI on the web: the browser page is the UI. The C UI logic drives the page's
    # DOM through web_dom.c (html_dom.h) and ui/js/kn-shim.js; the engine itself is not
    # compiled for the web. The lexbor target is still declared (EXCLUDE_FROM_ALL) so
    # `--target lexbor` checks it builds with emcc.
    add_subdirectory(client/lib/lexbor EXCLUDE_FROM_ALL)
    krunknative_add_html_sources(client ENGINE SOURCES html_ui.c html_dom_core.c web_dom.c)

    # Link flags:
    #   USE_GLFW/WEBGL2            GLFW port on top of a WebGL2 (GLES 3.0) context. No FULL_ES3: its
    #                              client-side array emulation costs a getParameter() per
    #                              glBindVertexArray and an attribute scan per draw, and the
    #                              renderer only draws from buffer objects
    #   ASYNCIFY                   synchronous asset downloads (emscripten_wget_data in ensure_asset_file)
    #   ASYNCIFY_STACK_SIZE        deep load paths (map -> prefab -> texture -> fetch) need more than the 4 KiB default
    #   STACK_SIZE                 the OBJ loader keeps whole models in VLAs; match the 8 MiB native stack
    #   FORCE_FILESYSTEM           MEMFS backing for fopen()-based loaders
    #   -lwebsocket.js             emscripten/websocket.h implementation (net_web.c)
    #   EXPORTED_RUNTIME_METHODS   UTF8ToString/stringToNewUTF8 are used from EM_ASM blocks; stack* let page JS ccall() with string arguments
    #   EXPORTED_FUNCTIONS         JS -> C entry points called by ui/js/kn-shim.js (page events, ResizeObserver)
    #   --preload-file             the window templates, read with fopen() from /ui/windows (MEMFS), and
    #                              the English string tables (v10 has them inline; the other languages
    #                              are fetched when picked: client/src/ui/i18n.c)
    target_link_options(client PRIVATE
        -sUSE_GLFW=3
        -sUSE_WEBGL2=1
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sALLOW_MEMORY_GROWTH=1
        -sASYNCIFY
        -sASYNCIFY_STACK_SIZE=65536
        -sSTACK_SIZE=8388608
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToNewUTF8,stackSave,stackRestore,stackAlloc
        -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_kn_ui_event,_kn_ui_resize_observed,_kn_ui_key_prevent
        -lwebsocket.js
        --preload-file ${CMAKE_SOURCE_DIR}/ui/windows@/ui/windows
        # SHELL: keeps each pair together (CMake de-duplicates a repeated --preload-file option)
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/ui/i18n/en.json@/ui/i18n/en.json"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/ui/i18n/kn/en.json@/ui/i18n/kn/en.json"
    )

    # Assemble the static site: the ui/ page tree (its js/loader.js pulls in
    # client.js when served over HTTP), the wasm, the preloaded ui/windows package
    # (client.data), and assets/ linked in so the client can fetch game content on
    # demand from /assets/.
    # A custom target (no OUTPUT) runs on every build, so ui/-only edits are
    # picked up without relinking the wasm; copy_if_different keeps it cheap.
    # js/ is replaced wholesale so scripts deleted from ui/js do not linger.
    # copy_directory_if_different (CMake 3.26+) leaves unchanged files alone, so a
    # rebuild never rewrites (and half-serves) fonts/images a running server hands out.
    if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
        set(KN_WEB_COPY_UI copy_directory_if_different)
    else ()
        set(KN_WEB_COPY_UI copy_directory)
    endif ()
    # platform/web/pwa.py (emsdk ships a Python for emcc anyway)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(web_dist ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WEB_DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${WEB_DIST_DIR}/js
        COMMAND ${CMAKE_COMMAND} -E ${KN_WEB_COPY_UI} ${CMAKE_SOURCE_DIR}/ui ${WEB_DIST_DIR}
        # page trees of earlier layouts (ui/v10 before it became ui/, the removed version picker's
        # ui/v3 / ui/v156) left in an old dist dir
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${WEB_DIST_DIR}/v10 ${WEB_DIST_DIR}/v156 ${WEB_DIST_DIR}/v3
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:client> ${WEB_DIST_DIR}/client.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:client>/client.wasm ${WEB_DIST_DIR}/client.wasm
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:client>/client.data ${WEB_DIST_DIR}/client.data
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${WEB_DIST_DIR}/assets
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/assets ${WEB_DIST_DIR}/assets
        # Add to Home Screen (PWA): manifest, icons and the <head> links of index.html
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/platform/web/pwa.py ${WEB_DIST_DIR}
        DEPENDS client
        COMMENT "Assembling web client in ${WEB_DIST_DIR}"
    )
else ()
    # LIBRARIES
    add_subdirectory(client/lib/freetype)
    add_subdirectory(client/lib/glad)
    add_subdirectory(client/lib/glfw)

    # GL BACKEND (client/include/gl.h; README.md "ANGLE / Metal (macOS)"):
    #   desktop  OpenGL 4.5 core through client/lib/glad (4.1 core on macOS: Apple's deprecated GL)
    #   angle    OpenGL ES 3.0 on ANGLE's libEGL/libGLESv2 through client/lib/glad_gles: Metal on
    #            macOS, Vulkan/OpenGL on Linux (testing). Compiles the renderer with KN_GLES, the
    #            same GL subset as the web build. ANGLE_DIR = tools/angle/build-angle.sh output
    #            (lib/ + include/); its runtime libraries are copied next to client_bin, which
    #            finds them through an @executable_path / $ORIGIN rpath.
    #   auto     (default) angle on macOS when ANGLE_DIR holds an ANGLE build, else desktop
    set(KRUNKNATIVE_GL_BACKEND auto CACHE STRING
        "Client GL backend: auto (macOS: angle if ANGLE_DIR is set), desktop (OpenGL 4.5 / macOS 4.1 core) or angle (OpenGL ES 3.0 on ANGLE: Metal on macOS)")
    set_property(CACHE KRUNKNATIVE_GL_BACKEND PROPERTY STRINGS auto desktop angle)
    set(ANGLE_DIR "$ENV{ANGLE_DIR}" CACHE PATH "ANGLE build for KRUNKNATIVE_GL_BACKEND=angle (lib/libEGL, lib/libGLESv2, include/EGL)")
    if (NOT ANGLE_DIR AND NOT "$ENV{ANGLE_DIR}" STREQUAL "") # `make angle` after a first configure
        set(ANGLE_DIR "$ENV{ANGLE_DIR}" CACHE PATH "ANGLE build for KRUNKNATIVE_GL_BACKEND=angle" FORCE)
    endif ()
    set(KRUNKNATIVE_GL_ANGLE OFF)
    if (KRUNKNATIVE_GL_BACKEND STREQUAL "angle" OR (KRUNKNATIVE_GL_BACKEND STREQUAL "auto" AND APPLE))
        find_library(ANGLE_EGL_LIBRARY NAMES EGL PATHS ${ANGLE_DIR}/lib ${ANGLE_DIR} NO_DEFAULT_PATH)
        find_library(ANGLE_GLESV2_LIBRARY NAMES GLESv2 PATHS ${ANGLE_DIR}/lib ${ANGLE_DIR} NO_DEFAULT_PATH)
        find_path(ANGLE_INCLUDE_DIR EGL/eglext_angle.h PATHS ${ANGLE_DIR}/include NO_DEFAULT_PATH)
        if (ANGLE_EGL_LIBRARY AND ANGLE_GLESV2_LIBRARY AND ANGLE_INCLUDE_DIR)
            set(KRUNKNATIVE_GL_ANGLE ON)
        elseif (KRUNKNATIVE_GL_BACKEND STREQUAL "angle")
            message(FATAL_ERROR "KRUNKNATIVE_GL_BACKEND=angle needs -DANGLE_DIR=<dir> (or $ANGLE_DIR) holding "
                "lib/libEGL, lib/libGLESv2 and include/EGL/eglext_angle.h: build one with "
                "`make angle` / tools/angle/build-angle.sh <dir> (ANGLE_DIR='${ANGLE_DIR}')")
        else ()
            message(WARNING "macOS without ANGLE (ANGLE_DIR='${ANGLE_DIR}'): falling back to Apple's deprecated "
                "OpenGL 4.1. Build ANGLE (Metal) with `make angle` and reconfigure with ANGLE_DIR=<dir>.")
        endif ()
    endif ()
    if (KRUNKNATIVE_GL_ANGLE)
        add_subdirectory(client/lib/glad_gles)
        get_filename_component(ANGLE_LIB_DIR ${ANGLE_GLESV2_LIBRARY} DIRECTORY)
        message(STATUS "GL backend: ANGLE (OpenGL ES 3.0) from ${ANGLE_LIB_DIR}")
        # GLFW dlopens EGL/GLESv2 itself: point it at ANGLE's (already loaded through the client's
        # link to libEGL), never the system libEGL.so.1.
        if (APPLE)
            target_compile_definitions(glfw PRIVATE _GLFW_EGL_LIBRARY="@rpath/libEGL.dylib" _GLFW_GLESV2_LIBRARY="@rpath/libGLESv2.dylib")
            file(GLOB ANGLE_RUNTIME_FILES ${ANGLE_LIB_DIR}/*.dylib ${ANGLE_LIB_DIR}/*.json)
        else ()
            target_compile_definitions(glfw PRIVATE _GLFW_EGL_LIBRARY="libEGL.so" _GLFW_GLESV2_LIBRARY="libGLESv2.so")
            file(GLOB ANGLE_RUNTIME_FILES ${ANGLE_LIB_DIR}/*.so ${ANGLE_LIB_DIR}/*.so.* ${ANGLE_LIB_DIR}/*.json)
        endif ()
        # ANGLE runtime (libEGL, libGLESv2, SwiftShader Vulkan ICD when built) next to the binaries.
        add_custom_target(angle_runtime
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ANGLE_RUNTIME_FILES} ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Copying ANGLE runtime libraries next to the client")
    else ()
        message(STATUS "GL backend: desktop OpenGL")
    endif ()
    # Links `target` to the selected GL flavour: loader, KN_GLES defines, ANGLE libEGL + rpath.
    function(krunknative_link_gl target)
        if (KRUNKNATIVE_GL_ANGLE)
            target_link_libraries(${target} PRIVATE glad_gles ${ANGLE_EGL_LIBRARY})
            target_include_directories(${target} PRIVATE ${ANGLE_INCLUDE_DIR})
            target_compile_definitions(${target} PRIVATE KRUNKNATIVE_GL_ANGLE=1)
            add_dependencies(${target} angle_runtime)
            # Only the binary's own directory: no absolute ANGLE_DIR path in shipped binaries.
            if (APPLE)
                set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@executable_path")
            else ()
                set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
            endif ()
        else ()
            target_link_libraries(${target} PRIVATE glad)
        endif ()
    endfunction ()

    add_subdirectory(server/lib/replxx)

    add_subdirectory(shared/lib/cJSON)
    add_subdirectory(shared/lib/pcg_basic)

    # QuickJS (client/lib/quickjs: Fabrice Bellard's release pinned in its VERSION, MIT
    # LICENSE; https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz, sha256
    # b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a, the engine's sources
    # copied unmodified; to bump, replace them and VERSION together): the native client's sandbox for the official Krunker servers' CHEAT_CHECK
    # scripts (client/src/net/krunker_cheat_qjs.c). The engine only, without quickjs-libc
    # (no std/os modules). Client-side only: never link it into server/ or shared/
    # targets; the web build evals in the page and does not compile it.
    add_library(kn_quickjs STATIC
        client/lib/quickjs/quickjs.c client/lib/quickjs/dtoa.c client/lib/quickjs/libregexp.c
        client/lib/quickjs/libunicode.c client/lib/quickjs/cutils.c)
    file(STRINGS client/lib/quickjs/VERSION KN_QUICKJS_VERSION LIMIT_COUNT 1)
    target_compile_definitions(kn_quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="${KN_QUICKJS_VERSION}")
    target_link_libraries(kn_quickjs PUBLIC Threads::Threads m)
    if (NOT MSVC)
        # upstream's flags: signed overflow wraps; -O2 like opus (unoptimised it is several times
        # slower, which eats into the per-challenge time limit); vendored code, warnings off
        target_compile_options(kn_quickjs PRIVATE -fwrapv -O2 -w)
    endif ()

    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_C_FLAGS --static)

        set(CLIENT_BINARY client)
        list(APPEND CLIENT_SOURCES client/win.rc)

        set(SERVER_BINARY server)
    endif ()

    link_libraries(cJSON pcg_basic)

    # Headless GPU mode (client/include/headless.h: KRUNKNATIVE_HEADLESS=1 / --headless):
    # an EGL context on a device display, no X11/Wayland window. Linux with libEGL only
    # (libglvnd: NVIDIA's and Mesa's EGL vendors); without it the mode reports itself unavailable.
    # With KRUNKNATIVE_GL_BACKEND=angle the EGL is ANGLE's on every platform (macOS included).
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT KRUNKNATIVE_GL_ANGLE)
        set(OpenGL_GL_PREFERENCE GLVND)
        find_package(OpenGL COMPONENTS EGL)
        if (NOT TARGET OpenGL::EGL)
            message(STATUS "Headless: libEGL/EGL headers not found; KRUNKNATIVE_HEADLESS is unavailable")
        endif ()
    endif ()
    function(krunknative_link_headless target)
        if (KRUNKNATIVE_GL_ANGLE)
            target_compile_definitions(${target} PRIVATE KRUNKNATIVE_HEADLESS_EGL=1) # libEGL: krunknative_link_gl
        elseif (TARGET OpenGL::EGL)
            target_link_libraries(${target} PRIVATE OpenGL::EGL)
            target_compile_definitions(${target} PRIVATE KRUNKNATIVE_HEADLESS_EGL=1)
        endif ()
    endfunction ()

    # CLIENT
    add_executable(${CLIENT_BINARY} ${CLIENT_HEADERS} ${CLIENT_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_CLIENT)
    target_link_libraries(${CLIENT_BINARY} PRIVATE glfw freetype kn_build_info Threads::Threads m)
    # the offline sandbox server (server_sandbox.h): the server core + its no-network backend
    target_sources(${CLIENT_BINARY} PRIVATE ${SERVER_CORE_STANDALONE_SOURCES})
    target_link_libraries(${CLIENT_BINARY} PRIVATE kn_server_core)
    target_link_libraries(${CLIENT_BINARY} PRIVATE kn_quickjs) # krunker_cheat_qjs.c
    if (KN_WEBVIEW_GTK) # the captcha webview (KN_WEBVIEW above); PRIVATE: GTK's includes stay in kn_webview
        add_library(kn_webview STATIC client/src/ui/kn_webview.c client/include/kn_webview.h)
        target_include_directories(kn_webview PRIVATE client/include)
        target_compile_definitions(kn_webview PRIVATE KN_WEBVIEW=1)
        target_link_libraries(kn_webview PRIVATE PkgConfig::KN_WEBKITGTK)
        target_link_libraries(${CLIENT_BINARY} PRIVATE kn_webview)
    endif ()
    krunknative_link_gl(${CLIENT_BINARY})
    # miniaudio (client/src/miniaudio.c) loads its audio backends with dlopen().
    target_link_libraries(${CLIENT_BINARY} PRIVATE ${CMAKE_DL_LIBS})
    # voice chat (client/src/voice.c): the vendored libopus, linked statically on every platform
    krunknative_add_opus()
    target_link_libraries(${CLIENT_BINARY} PRIVATE opus)
    target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_HAVE_OPUS=1)
    krunknative_link_headless(${CLIENT_BINARY})
    target_include_directories(${CLIENT_BINARY} PRIVATE client/include)
    # the map editor AI panel's tools (tools/editor-mcp, in-process: client/src/editor/editor_ai_tools.c);
    # POSIX only like kn_editor_mcp (a Windows client offers the AI chat without the editor tools)
    if (NOT WIN32)
        include(tools/editor-mcp/core.cmake)
        kn_editor_mcp_embed(${CLIENT_BINARY})
    endif ()
    # Optional TLS for the backend client (client/src/net/backend_native.c: https:// auth service
    # and server finder, wss:// social socket). Without OpenSSL only http:// / ws:// work.
    find_package(OpenSSL COMPONENTS SSL Crypto QUIET)
    # KRUNKNATIVE_TLS_STATIC (with -DOPENSSL_USE_STATIC_LIBS=TRUE: the polyfilled Linux and the
    # macOS packages, tools/release/build-client-linux.sh / build-client.sh): the static libssl runs
    # on other systems, so client_bin and kn_launcher (client/src/net/http_conn.c) neither load their
    # openssl.cnf nor rely on the build host's OPENSSLDIR for the CA store.
    option(KRUNKNATIVE_TLS_STATIC "Client TLS: static OpenSSL for portable packages (no system openssl.cnf, probe the CA bundle)" OFF)
    if (OpenSSL_FOUND AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(${CLIENT_BINARY} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_HAVE_TLS=1)
        if (KRUNKNATIVE_TLS_STATIC)
            target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_TLS_STATIC=1)
        endif ()
    else ()
        message(STATUS "Client: OpenSSL (libssl) not found; the backend client is http:// only")
    endif ()

    # Client updates (launcher/, doc/backend.md "Client updates"): the menu's Outdated check reads
    # the updates manifest itself (launcher/src/update_core.c + http_fetch.c, WinHTTP on Windows)
    # and "Update now" restarts through the launcher (install.c / fs.c: kn_proc_spawn_detached).
    set(KN_UPDATE_CLIENT_SOURCES launcher/src/update_core.c launcher/src/http_fetch.c launcher/src/fs.c launcher/src/install.c)
    target_sources(${CLIENT_BINARY} PRIVATE ${KN_UPDATE_CLIENT_SOURCES})
    target_include_directories(${CLIENT_BINARY} PRIVATE launcher/include)

    if (KRUNKNATIVE_HTML_ENGINE)
        # HTML UI (in-house engine): Lexbor + engine/*.c + html_ui.c + html_dom_core.c,
        # KRUNKNATIVE_HTML_ENGINE=1 on the whole client target.
        add_subdirectory(client/lib/lexbor)
        krunknative_add_html_sources(${CLIENT_BINARY} ENGINE SOURCES html_ui.c html_dom_core.c)
        target_sources(${CLIENT_BINARY} PRIVATE ${HTML_ENGINE_SOURCES} ${HTML_ENGINE_HEADERS})
        target_link_libraries(${CLIENT_BINARY} PRIVATE lexbor)
    else ()
        # No HTML UI: no-op DOM ops and backend, html_ui_available() == 0.
        message(STATUS "HTML UI: KRUNKNATIVE_HTML_ENGINE is OFF; building the client without it (stub.c)")
        krunknative_add_html_sources(${CLIENT_BINARY} SOURCES html_ui.c html_dom_core.c stub.c)
    endif ()

    # SERVER: the core library + server_bin's network frontend
    add_library(kn_server_core STATIC ${SERVER_CORE_SOURCES} ${SERVER_HEADERS})
    target_include_directories(kn_server_core PUBLIC server/include)
    target_link_libraries(kn_server_core PUBLIC kn_build_info Threads::Threads m)

    add_executable(${SERVER_BINARY} ${SERVER_HEADERS} ${SERVER_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_compile_definitions(${SERVER_BINARY} PRIVATE KRUNKNATIVE_SERVER)
    target_link_libraries(${SERVER_BINARY} PRIVATE kn_server_core replxx Threads::Threads m)
    target_include_directories(${SERVER_BINARY} PRIVATE server/include)
    # https:// for the finder heartbeat and central auth hasJoined (server/src/http_client.c);
    # without libcurl server_bin only speaks plain http:// to them.
    find_package(CURL QUIET)
    if (CURL_FOUND)
        target_link_libraries(${SERVER_BINARY} PRIVATE CURL::libcurl)
        target_compile_definitions(${SERVER_BINARY} PRIVATE KRUNKNATIVE_HAVE_CURL=1)
    endif ()

    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(${CLIENT_BINARY} PRIVATE ws2_32 winhttp)
        target_link_libraries(${SERVER_BINARY} PRIVATE ws2_32)
        target_link_libraries(kn_server_core PUBLIC ws2_32)
    endif ()

    # LAUNCHER (launcher/, doc/backend.md "Client updates"): what players start. It updates the
    # executables + ui/ and syncs the game content from the updates server, then runs client_bin.
    # Output: krunknative (KrunkNative.exe, a GUI-subsystem exe), next to client_bin in packages.
    # Same GL backend, FreeType and OpenSSL as client_bin, no other runtime dependency.
    set(LAUNCHER_SOURCES
        launcher/src/launcher.c launcher/src/launcher_ui.c launcher/src/launcher_ui.h
        launcher/src/update_core.c launcher/src/http_fetch.c launcher/src/archive.c launcher/src/fs.c
        launcher/src/install.c launcher/src/assets_sync.c
        client/src/net/http_conn.c client/src/net/http_proxy.c client/src/headless.c client/src/png_write.c client/src/stb_image.c
        shared/src/auth_join.c)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        list(APPEND LAUNCHER_SOURCES client/win.rc) # the exe icon + "Krunker Box Office" version info
    endif ()
    add_executable(kn_launcher WIN32 ${LAUNCHER_SOURCES})
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(kn_launcher PROPERTIES OUTPUT_NAME KrunkNative)
        target_link_libraries(kn_launcher PRIVATE winhttp)
    else ()
        set_target_properties(kn_launcher PROPERTIES OUTPUT_NAME krunknative)
    endif ()
    target_include_directories(kn_launcher PRIVATE launcher/include client/include shared/include)
    target_link_libraries(kn_launcher PRIVATE glfw freetype kn_build_info Threads::Threads m)
    krunknative_link_gl(kn_launcher)
    krunknative_link_headless(kn_launcher)
    if (OpenSSL_FOUND AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(kn_launcher PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(kn_launcher PRIVATE KRUNKNATIVE_HAVE_TLS=1)
        if (KRUNKNATIVE_TLS_STATIC)
            target_compile_definitions(kn_launcher PRIVATE KRUNKNATIVE_TLS_STATIC=1)
        endif ()
    endif ()
    target_link_libraries(kn_launcher PRIVATE ${CMAKE_DL_LIBS})

    # TESTS
    add_executable(proc_input_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/proc_input_test.c)
    target_link_libraries(proc_input_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(proc_input_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # knet UDP transport (shared/src/knet.c only: no game code, no sockets)
    add_executable(knet_test shared/include/knet.h shared/src/knet.c shared/tests/knet_test.c)

    # game ids and their short ?game= link form (shared/src/server_names.c: brain + client)
    add_executable(server_names_test shared/include/server_names.h shared/src/server_names.c shared/tests/server_names_test.c)
    add_test(NAME server_names_test COMMAND server_names_test)

    # official Krunker socket codec (shared/src/krunker_proto.c: msgpack + ahNum trailer, no I/O).
    # Built with ASan + UBSan whenever the toolchain links them: its malformed-frame and fuzz cases
    # are meant to run sanitized.
    add_executable(krunker_proto_test shared/include/krunker_proto.h shared/include/krunker_build.h
        shared/src/krunker_proto.c shared/tests/krunker_proto_test.c)
    target_link_libraries(krunker_proto_test PRIVATE m)
    if (NOT WIN32)
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
        check_c_source_compiles("int main(void) { return 0; }" KRUNKNATIVE_HAVE_ASAN_UBSAN)
        unset(CMAKE_REQUIRED_FLAGS)
        if (KRUNKNATIVE_HAVE_ASAN_UBSAN)
            target_compile_options(krunker_proto_test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
            target_link_libraries(krunker_proto_test PRIVATE -fsanitize=address,undefined)
        endif ()
    endif ()
    add_test(NAME krunker_proto_test COMMAND krunker_proto_test)

    # editor bus (shared/src/editor_bus.c: framing, scopes, tokens, unix socket + TCP fallback)
    add_executable(editor_bus_test shared/include/editor_bus.h shared/src/editor_bus.c shared/tests/editor_bus_test.c)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(editor_bus_test PRIVATE ws2_32)
    endif ()

    # config_test (map-logic WP-E): v7.1.4 custom game config against v7's own setupGameConfig /
    # applyConfig results (shared/tests/data/v7/config_cases.json), map config, sync, match votes
    add_executable(config_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/config_test.c)
    target_link_libraries(config_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(config_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    add_executable(class_spawn_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/class_spawn_test.c)
    target_link_libraries(class_spawn_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(class_spawn_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # weapons_v7_test / combat_test (WP-B): v7.1.4 class / weapon / effect / streak tables against
    # shared/tests/data/v7 (tools/kref/v7-dump.js), and the charge / hitbox / streak / effect maths
    # zombies_test (map-logic WP-H): waves, power-ups, machines, perks, finiteAmmo, airdrops
    add_executable(zombies_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/zombies_test.c)
    target_link_libraries(zombies_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(zombies_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # charge_grapple_test: the Charge Rifle charge / blast and the Grappler rope against v7.1.4
    # piss_test: the KrunkNative Piss's stream, puddles, caps and their SPAWNED_OBJ replication
    # ruleset_test: the Krunker v10.0.2 ruleset, KN id ranges and the NET_PACKET_EXT envelope
    # hitboxes_test: the v10.0.2 posed hit boxes (shared/src/hitboxes.c) at known poses
    # v10_gameplay_test: the v10.0.2-only gameplay (Boulder, Compressor, Stalker rework, ranked rules)
    foreach (wpb_test weapons_v7_test combat_test charge_grapple_test piss_test ruleset_test hitboxes_test v10_gameplay_test)
        add_executable(${wpb_test} ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/${wpb_test}.c)
        target_link_libraries(${wpb_test} PRIVATE m)
        if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
            target_link_libraries(${wpb_test} PRIVATE ws2_32) # shared/src/net.c
        endif ()
    endforeach ()

    # recon_test: client prediction + server reconciliation over a simulated laggy link
    add_executable(recon_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/recon_test.c)
    target_link_libraries(recon_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(recon_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # match_test: combat / kills / score / chat / round end through the shared game hooks
    add_executable(match_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/match_test.c)
    target_link_libraries(match_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(match_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # lagcomp_test: v3.1.1 storeState / fetchState lag compensation (hit with, miss without; window, selection)
    add_executable(lagcomp_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/lagcomp_test.c)
    target_link_libraries(lagcomp_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(lagcomp_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # modes_test: every v3.1.1 game mode (scoring, objectives, picks, lives, win rules) with simulated players
    add_executable(modes_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/modes_test.c)
    target_link_libraries(modes_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(modes_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # comp_ranked_test: v7.1.4 ranked rating rules and the comp lobby (lobbyPlayers, canSpawn)
    add_executable(comp_ranked_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/comp_ranked_test.c)
    target_link_libraries(comp_ranked_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(comp_ranked_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # season_test: v7.1.4 seasons, the Battle Pass (levels, daily BPP cap, claims, skips), season
    # badges and the challenge contexts (shared/src/season.c, progression.c)
    add_executable(season_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/season_test.c)
    target_link_libraries(season_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(season_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # ai_test: v3.1.1 AI (map BOTs, behaviour, damage, sync, /summon /ais /kill, Traitor ghosts)
    add_executable(ai_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/ai_test.c)
    target_link_libraries(ai_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(ai_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # bot_test: Krunker v7.1.4 bot players (shared/src/bots.c) and the bot_* console commands
    add_executable(bot_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/bot_test.c)
    target_link_libraries(bot_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(bot_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # target_select_test: player target selectors (shared/src/target_select.c: *, @me, @bots, @team,
    # Joel* ...) and the chat commands over them (god * / ungod * / give * / kick *, permissions)
    add_executable(target_select_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/target_select_test.c)
    target_link_libraries(target_select_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(target_select_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # navgen_test: the Krunker v7.1.4 generated nav mesh (shared/src/navgen.c) against v7's navgen.ts
    add_executable(navgen_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/navgen_test.c)
    target_link_libraries(navgen_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(navgen_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # spawn_test: v3.1.1 getSpawnPoint rules, distinct points for players spawning in one tick
    add_executable(spawn_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/spawn_test.c)
    target_link_libraries(spawn_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(spawn_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # sandbox_test: the offline sandbox server (server_sandbox.h) over the loopback transport
    add_executable(sandbox_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/sandbox_test.c)
    target_link_libraries(sandbox_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(sandbox_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # social_test (player-viewer WP-E part 2b): v7 chat / voice channels, multi-spray, taunts over the sandbox
    add_executable(social_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/social_test.c)
    target_link_libraries(social_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(social_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # object_manager_test: the KrunkNative Object Manager (F1 spawn menu) over the sandbox: extra
    # loadout weapons, pickups at the crosshair, permissions, rate limit, join sync; the Properties
    # tool's fields against the map editor's data model (client/src/editor/editor_model.c, GL-free)
    add_executable(object_manager_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/object_manager_test.c
                   client/src/editor/editor_model.c client/src/editor/editor_data.c)
    target_include_directories(object_manager_test PRIVATE client/src/editor)
    target_link_libraries(object_manager_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(object_manager_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # weapons_audit_test: every v7 weapon through the class loadout, an Object Manager extra slot and
    # a dropped pickup over the sandbox (equip, ADS, fire, ammo, reloads, the special mechanics)
    add_executable(weapons_audit_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/weapons_audit_test.c)
    target_link_libraries(weapons_audit_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(weapons_audit_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # sandbox_config_test (map-logic WP-E): the offline sandbox plays a map's embedded config, its
    # players count as having an account; GAME_CONFIG / NETWORK_DATA only to protocol-1.3 peers
    add_executable(sandbox_config_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/sandbox_config_test.c)
    target_link_libraries(sandbox_config_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(sandbox_config_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # ks_server_test: the KrunkScript server host (server/src/ks_server_host.c) over the sandbox
    add_executable(ks_server_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/ks_server_test.c)
    target_link_libraries(ks_server_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(ks_server_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # ks_storage_test (KrunkScript WP6): GAME.STORAGE / Save / Load Data on the offline store, the
    # kn_brain frames, KR rewards, ADMIN.ban
    add_executable(ks_storage_test ${SHARED_HEADERS} ${SHARED_SOURCES} ${SERVER_CORE_STANDALONE_SOURCES} server/tests/ks_storage_test.c)
    target_link_libraries(ks_storage_test PRIVATE kn_server_core m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(ks_storage_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # triggers_test: the v7.1.4 trigger runtime (WP-C): events, conditions, actions, values, delays, chains,
    # costs, pickups, and every trigger of the v7-format custom maps
    add_executable(triggers_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/triggers_test.c)
    target_link_libraries(triggers_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(triggers_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # trigger_authority_test: map triggers / interact fire on an authority game only (is_local or a server)
    add_executable(trigger_authority_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/trigger_authority_test.c)
    target_link_libraries(trigger_authority_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(trigger_authority_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # map_catalog_test: every v3.1.1 map (15 official + 21 community) loads, parses and plays
    add_executable(map_catalog_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/map_catalog_test.c)
    target_link_libraries(map_catalog_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(map_catalog_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # map_v7_test (map-logic WP-A): the v7.1.4 map schema, prefab table, shims, round trip and validation
    add_executable(map_v7_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/map_v7_test.c)
    target_link_libraries(map_v7_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(map_v7_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # map_v10_test: the Krunker v10.0.2 map format (doc/v10/map-format.md), and
    # the v7 dialect on the same input: prefabs 41 / 97 / 98, dyn, mp, shadowNB, archetypes,
    # entities, scripts.shared, botPaths walking, trigger condition 12 / actions 72-74 / 8 actions
    add_executable(map_v10_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/map_v10_test.c)
    target_link_libraries(map_v10_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(map_v10_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # world_v7_test (map-logic WP-B): tris, terrain, BR zone, score / check points / resets, zones, PvP, moves, builds
    add_executable(world_v7_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/world_v7_test.c)
    target_link_libraries(world_v7_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(world_v7_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # editor_serialize_test (doc/v7/editor.md WP1): the editor's GL-free data model
    # (client/src/editor/editor_model.c) against v7.1.4's own ObjectInstance serialization
    # (client/tests/data/editor_v7_golden.json) and import / export round trips over assets/maps
    add_executable(editor_serialize_test client/tests/editor_serialize_test.c client/src/editor/editor_model.c
                   client/src/editor/editor_model.h client/src/editor/editor_data.c client/src/editor/editor_data.h
                   client/src/editor/editor_script_lang.c shared/src/ks/ks_lex.c shared/src/ks/ks_parse.c
                   shared/src/ks/ks_check.c shared/src/ks/ks_api_table.c)
    target_include_directories(editor_serialize_test PRIVATE client/src/editor shared/include)
    target_link_libraries(editor_serialize_test PRIVATE cJSON m)
    enable_testing()
    add_test(NAME editor_serialize_test COMMAND editor_serialize_test ${CMAKE_SOURCE_DIR}
             --golden ${CMAKE_SOURCE_DIR}/doc/ref/krunker-v7.1.4/editor/inspector-edits.json) # WP3: the inspector edits

    # editor_export_test (doc/v7/editor.md WP6): File > Export GLTF / Export MTL
    # (client/src/editor/editor_export.c, GL-free) against goldens and read back with fast_obj / cgltf
    add_executable(editor_export_test client/tests/editor_export_test.c client/src/editor/editor_export.c
                   client/src/editor/editor_export.h client/src/editor/editor_model.c client/src/editor/editor_data.c
                   client/src/fast_obj.c ${SHARED_SOURCES} ${SHARED_HEADERS})
    target_compile_definitions(editor_export_test PRIVATE ED_EXPORT_NO_EDITOR)
    target_include_directories(editor_export_test PRIVATE client/src/editor client/include shared/include)
    target_link_libraries(editor_export_test PRIVATE cJSON pcg_basic m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(editor_export_test PRIVATE ws2_32)
    endif ()
    add_test(NAME editor_export_test COMMAND editor_export_test ${CMAKE_SOURCE_DIR})

    # krunker_link_test (client/include/krunker_net.h, KN_KRUNKER_OFFICIAL): the official-Krunker
    # packet translation (client/src/net/krunker_link.c) over the codec, no sockets
    add_executable(krunker_link_test client/tests/krunker_link_test.c client/src/net/krunker_link.c client/src/net/krunker_altcha.c
                   shared/src/krunker_proto.c shared/src/auth_join.c)
    target_include_directories(krunker_link_test PRIVATE client/include shared/include)
    target_link_libraries(krunker_link_test PRIVATE cJSON m)
    add_test(NAME krunker_link_test COMMAND krunker_link_test)

    # update_test (launcher/, doc/backend.md "Client updates"): manifest parsing, version
    # comparison, asset diffing, URL resolution, package extraction and the journaled swap
    add_executable(update_test launcher/tests/update_test.c launcher/src/update_core.c launcher/src/archive.c
                   launcher/src/fs.c launcher/src/install.c client/src/stb_image.c shared/src/auth_join.c)
    target_include_directories(update_test PRIVATE launcher/include client/include shared/include)
    target_link_libraries(update_test PRIVATE cJSON m)
    add_test(NAME update_test COMMAND update_test)

    # asset_fetch_test (client/include/asset_fetch.h): the native on-demand content fetch's queue over
    # an in-memory transport (verification, fallback, backoff, cap, checkout guard). POSIX.
    if (NOT WIN32)
        add_executable(asset_fetch_test client/tests/asset_fetch_test.c client/src/net/asset_fetch.c
                       launcher/src/update_core.c launcher/src/http_fetch.c launcher/src/fs.c launcher/src/install.c
                       launcher/src/archive.c client/src/net/http_conn.c client/src/net/http_proxy.c client/src/stb_image.c shared/src/auth_join.c)
        target_include_directories(asset_fetch_test PRIVATE launcher/include client/include shared/include)
        target_link_libraries(asset_fetch_test PRIVATE cJSON Threads::Threads m)
        if (OpenSSL_FOUND)
            target_link_libraries(asset_fetch_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
            target_compile_definitions(asset_fetch_test PRIVATE KRUNKNATIVE_HAVE_TLS=1)
        endif ()
        add_test(NAME asset_fetch_test COMMAND asset_fetch_test)
    endif ()

    # assets_bench (launcher/tests/assets_bench.c): the content sync into a directory with timings,
    # against the live origins by hand or a local server in assets_sync_e2e (keep-alive, pipelining,
    # parallel origins, resume, hash mismatch, fallback, cancel). POSIX (the e2e is Python + sockets).
    if (NOT WIN32)
        add_executable(assets_bench launcher/tests/assets_bench.c launcher/src/assets_sync.c
                       launcher/src/update_core.c launcher/src/http_fetch.c launcher/src/fs.c launcher/src/archive.c
                       client/src/net/http_conn.c client/src/net/http_proxy.c client/src/stb_image.c shared/src/auth_join.c)
        target_include_directories(assets_bench PRIVATE launcher/include client/include shared/include)
        target_link_libraries(assets_bench PRIVATE cJSON Threads::Threads m)
        if (OpenSSL_FOUND)
            target_link_libraries(assets_bench PRIVATE OpenSSL::SSL OpenSSL::Crypto)
            target_compile_definitions(assets_bench PRIVATE KRUNKNATIVE_HAVE_TLS=1)
        endif ()
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if (Python3_FOUND)
            add_test(NAME assets_sync_e2e COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/launcher/tests/assets_sync_e2e.py
                     $<TARGET_FILE:assets_bench>)
            set_tests_properties(assets_sync_e2e PROPERTIES TIMEOUT 300)
        endif ()
    endif ()

    # krunker_cheat_test: the official servers' CHEAT_CHECK crypto (keystream, XOR, base64, the
    # double-rounded FNV-1a) against hand-derived vectors, cc -> ccr round trips through the
    # QuickJS sandbox, and the sandbox limits (client/src/net/krunker_cheat*.c)
    add_executable(krunker_cheat_test client/tests/krunker_cheat_test.c client/src/net/krunker_cheat.c
                   client/src/net/krunker_cheat_qjs.c)
    target_include_directories(krunker_cheat_test PRIVATE client/include client/src/net)
    target_link_libraries(krunker_cheat_test PRIVATE kn_quickjs)
    add_test(NAME krunker_cheat_test COMMAND krunker_cheat_test)

    # qr_code_test (email-verify): the v7.1.4 2FA popup's QR code encoder (client/src/ui/qr_code.c)
    # against python-qrcode goldens (client/tests/data/qr_golden.json)
    add_executable(qr_code_test client/tests/qr_code_test.c client/src/ui/qr_code.c)
    target_include_directories(qr_code_test PRIVATE client/src/ui)
    target_link_libraries(qr_code_test PRIVATE cJSON)
    add_test(NAME qr_code_test COMMAND qr_code_test ${CMAKE_SOURCE_DIR})

    # vic_title_test: the round-end title's fade / zoom (client/src/ui/vic_title.c) frame by frame
    # against v7.1.4's overlay.vicTitle code run in node (client/tests/data/vic_title_golden.json)
    add_executable(vic_title_test client/tests/vic_title_test.c client/src/ui/vic_title.c)
    target_include_directories(vic_title_test PRIVATE client/src/ui)
    target_link_libraries(vic_title_test PRIVATE cJSON m)
    add_test(NAME vic_title_test COMMAND vic_title_test ${CMAKE_SOURCE_DIR})

    # window_state_test: the window state F4 / the Settings relaunch pass to the new process
    # (KRUNKNATIVE_RESTORE_WINDOW) and a normal restart keeps (client/src/window_state.c)
    add_executable(window_state_test client/tests/window_state_test.c client/src/window_state.c)
    target_include_directories(window_state_test PRIVATE client/include)
    add_test(NAME window_state_test COMMAND window_state_test)

    # win_geom_test: the shared geometry rule of the draggable / resizable windows (developer
    # console, editor Quick GUI and side panels): off-screen saves, small windows, grip resizes
    add_executable(win_geom_test client/tests/win_geom_test.c client/src/ui/win_geom.c)
    target_include_directories(win_geom_test PRIVATE client/include)
    target_link_libraries(win_geom_test PRIVATE m)
    add_test(NAME win_geom_test COMMAND win_geom_test)

    # pointer_lock_test: when the pointer is locked and the mouse turns the camera (v7 controls.js
    # CONTROLS.enabled / pointerlockchange): Esc menu, end screen, focus loss, KrunkScript
    # unlockMouse / freeMouse (client/src/pointer_lock.c)
    add_executable(pointer_lock_test client/tests/pointer_lock_test.c client/src/pointer_lock.c)
    target_include_directories(pointer_lock_test PRIVATE client/include)
    add_test(NAME pointer_lock_test COMMAND pointer_lock_test)

    # backend_online_test: online mode's defaults (the public finder / account service, never
    # localhost) and the native brain transport over loopback (client/src/net/online_mode.c,
    # backend_common.c, backend_native.c; on Windows backend_winhttp.c, which CI runs there too)
    add_executable(backend_online_test client/tests/backend_online_test.c client/src/net/online_mode.c shared/src/net_ext.c
                   client/src/net/backend_common.c client/src/net/backend_native.c client/src/net/backend_winhttp.c
                   client/src/net/http_conn.c client/src/net/http_proxy.c)
    target_include_directories(backend_online_test PRIVATE client/include shared/include)
    target_link_libraries(backend_online_test PRIVATE glfw freetype cJSON Threads::Threads m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(backend_online_test PRIVATE ws2_32 winhttp)
    elseif (OpenSSL_FOUND)
        target_link_libraries(backend_online_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(backend_online_test PRIVATE KRUNKNATIVE_HAVE_TLS=1)
    endif ()
    add_test(NAME backend_online_test COMMAND backend_online_test)

    # http_proxy_test: the client's proxy URLs and per-host choice (client/src/net/http_proxy.c:
    # Settings > Client > Proxy and its scope, HTTPS_PROXY / HTTP_PROXY / ALL_PROXY / NO_PROXY). POSIX
    # (setenv); the tunnels themselves run in server/tests/client_proxy_e2e.py (make client-proxy-e2e).
    if (NOT WIN32)
        add_executable(http_proxy_test client/tests/http_proxy_test.c client/src/net/http_proxy.c)
        target_include_directories(http_proxy_test PRIVATE client/include)
        target_link_libraries(http_proxy_test PRIVATE Threads::Threads)
        add_test(NAME http_proxy_test COMMAND http_proxy_test)
    endif ()

    # meshopt_test: the EXT_meshopt_compression decoder (client/lib/meshopt, client/src/gltf_meshopt.c)
    # against the meshoptimizer npm decoder's goldens (client/tests/data/meshopt_golden.json)
    add_executable(meshopt_test client/tests/meshopt_test.c client/src/gltf_meshopt.c)
    target_include_directories(meshopt_test PRIVATE client/include)
    target_link_libraries(meshopt_test PRIVATE cJSON m)
    add_test(NAME meshopt_test COMMAND meshopt_test ${CMAKE_SOURCE_DIR})

    # mod_pack_test: map resource packs (client/src/mod_pack.c, zip_read.c): the override applies on
    # the pack's map and is gone after a map change (client/tests/data/mod_pack_test.zip). POSIX.
    if (NOT WIN32)
        add_executable(mod_pack_test client/tests/mod_pack_test.c client/src/mod_pack.c client/src/zip_read.c
                       client/src/stb_image.c ${SHARED_HEADERS} ${SHARED_SOURCES})
        target_include_directories(mod_pack_test PRIVATE client/include)
        target_link_libraries(mod_pack_test PRIVATE cJSON m)
        add_test(NAME mod_pack_test COMMAND mod_pack_test ${CMAKE_SOURCE_DIR})
    endif ()

    # KrunkScript (doc/v7/krunkscript.md 6.6): ksc (check; WP2 adds run) over the ks
    # sources only, and ks_front_test (lexer/parser/checker + shared/tests/ks suite)
    file(GLOB KS_SOURCES shared/src/ks/*.c)
    add_executable(ksc tools/ks/ksc.c tools/ks/ksc_run.c ${KS_SOURCES})
    target_link_libraries(ksc PRIVATE m)
    set(KS_FRONT_SOURCES shared/src/ks/ks_lex.c shared/src/ks/ks_parse.c shared/src/ks/ks_check.c shared/src/ks/ks_api_table.c
        shared/src/ks/ks_import_js.c)
    add_executable(ks_front_test shared/tests/ks_front_test.c ${KS_FRONT_SOURCES})
    enable_testing()
    add_test(NAME ks_front_test COMMAND ks_front_test ${CMAKE_SOURCE_DIR}/shared/tests/ks)
    # ks_vm_test (WP2): runtime conformance programs (shared/tests/ks/run/*.krnk + .out), number,
    # serialisation and GC unit tests, and the corpus runtime smoke test, over the ks sources
    # only. -DKRUNKNATIVE_KS_SANITIZE=ON builds it with ASan + UBSan (GCC needs libasan).
    option(KRUNKNATIVE_KS_SANITIZE "Build ks_vm_test with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
    add_executable(ks_vm_test shared/tests/ks_vm_test.c ${KS_SOURCES})
    target_compile_definitions(ks_vm_test PRIVATE KS_TEST_DIR="${CMAKE_SOURCE_DIR}/shared/tests/ks")
    target_link_libraries(ks_vm_test PRIVATE m)
    if (KRUNKNATIVE_KS_SANITIZE)
        target_compile_options(ks_vm_test PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_libraries(ks_vm_test PRIVATE -fsanitize=address,undefined)
    endif ()
    add_test(NAME ks_vm_test COMMAND ks_vm_test ${CMAKE_SOURCE_DIR}/shared/tests/ks)

    # kn_editor_mcp: Krunker map editor MCP server (tools/editor-mcp/README.md)
    add_subdirectory(tools/editor-mcp)

    # html_runtime_sheet_test: the cascade with the v7 page's sheets plus a runtime one (devconsole.css),
    # and the v10 page with more than 14 author sheets plus runtime sheets (the old 4-bit sheet index)
    if (KRUNKNATIVE_HTML_ENGINE)
        set(KN_ENGINE_DIR client/src/html/engine)
        add_executable(html_runtime_sheet_test ${KN_ENGINE_DIR}/tests/test_runtime_sheet.c ${KN_ENGINE_DIR}/dom.c
                       ${KN_ENGINE_DIR}/css_sheet.c ${KN_ENGINE_DIR}/css_cascade.c ${KN_ENGINE_DIR}/css_values.c
                       ${KN_ENGINE_DIR}/util.c)
        target_include_directories(html_runtime_sheet_test PRIVATE ${KN_ENGINE_DIR} client/include client/lib/lexbor)
        target_link_libraries(html_runtime_sheet_test PRIVATE lexbor m)
        add_test(NAME html_runtime_sheet_test COMMAND html_runtime_sheet_test ${CMAKE_SOURCE_DIR})
    endif ()

    # ui_test: engine driver for dumps, probes, renders and benchmarks
    # (client/src/html/ui_test.c holds the subcommand table). It is the engine plus
    # the minimum of the client it needs: shader.c (shader_compile) and the shared
    # sources (assets_path, read_file). Built with `make ui-test`.
    if (KRUNKNATIVE_HTML_ENGINE)
        add_executable(ui_test
            client/src/html/ui_test.c
            client/src/shader.c
            client/src/png_write.c
            client/src/headless.c
            ${HTML_ENGINE_SOURCES} ${HTML_ENGINE_HEADERS}
            ${SHARED_HEADERS} ${SHARED_SOURCES})
        krunknative_add_html_sources(ui_test ENGINE SOURCES html_ui.c html_dom_core.c)
        target_link_libraries(ui_test PRIVATE lexbor freetype glfw Threads::Threads m)
        krunknative_link_gl(ui_test)
        krunknative_link_headless(ui_test)
        if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
            target_link_libraries(ui_test PRIVATE ws2_32) # shared/src/net.c
        endif ()
    endif ()

    # canvas2d_test: the KrunkScript GAME.OVERLAY canvas (client/src/ks/canvas2d.c + ks_overlay.c)
    # replaying a tools/canvas2d scene into a PNG; tools/canvas2d/run.sh diffs it against
    # headless Chromium's Canvas 2D. Needs only GL, FreeType and the shared sources.
    add_executable(canvas2d_test
        tools/canvas2d/canvas2d_test.c
        client/src/ks/canvas2d.c
        client/src/ks/ks_overlay.c
        client/src/ks/ks_overlay_scene.c
        client/src/shader.c
        client/src/png_write.c
        client/src/headless.c
        client/src/stb_image.c
        ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_include_directories(canvas2d_test PRIVATE client/include)
    target_link_libraries(canvas2d_test PRIVATE freetype glfw Threads::Threads m)
    krunknative_link_gl(canvas2d_test)
    krunknative_link_headless(canvas2d_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(canvas2d_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # billboard_ad_test: the server's billboard ad (client/src/billboard_ad.c): packets -> INIT,
    # the BILLBOARD surfaces of five maps, the createCanvasAD canvas (headless GL, --headless;
    # run from the repo root).
    add_executable(billboard_ad_test
        client/tests/billboard_ad_test.c
        client/src/billboard_ad.c
        client/src/ks/canvas2d.c
        client/src/shader.c
        client/src/png_write.c
        client/src/headless.c
        client/src/stb_image.c
        ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_include_directories(billboard_ad_test PRIVATE client/include)
    target_link_libraries(billboard_ad_test PRIVATE freetype glfw Threads::Threads m)
    krunknative_link_gl(billboard_ad_test)
    krunknative_link_headless(billboard_ad_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(billboard_ad_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # gltf_test: the GLTF loader (client/src/gltf.c) on v7.1.4 animated store models through the
    # pet path (client/src/pets.c), the pets.js follow AI and the skin animations (skin_fx.c).
    # Headless GL (--headless); run from the repo root (WP-F, doc/v7/player-viewer.md).
    add_executable(gltf_test
        tools/gltf_test/gltf_test.c
        client/src/gltf.c
        client/src/gltf_meshopt.c
        client/src/pets.c
        client/src/skin_fx.c
        client/src/cosmetics.c
        client/src/viewer/viewer_store.c
        client/src/viewer/viewer_store_table.c
        client/src/mesh.c
        client/src/loaders.c
        client/src/fast_obj.c
        client/src/shader.c
        client/src/materials/basic.c
        client/src/headless.c
        client/src/stb_image.c
        ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_include_directories(gltf_test PRIVATE client/include client/lib)
    target_link_libraries(gltf_test PRIVATE freetype glfw Threads::Threads m)
    krunknative_link_gl(gltf_test)
    krunknative_link_headless(gltf_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(gltf_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # projectiles_render_test: thrown-knife visuals over the owner's "pr" / "pre" sequences (stuck
    # knives keep their end, the throw's charge speed). GL-free: it includes projectiles_render.c and
    # stubs the mesh / scene / loader calls, so the GL target only provides headers.
    add_executable(projectiles_render_test client/tests/projectiles_render_test.c ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_include_directories(projectiles_render_test PRIVATE client/include client/lib)
    target_link_libraries(projectiles_render_test PRIVATE freetype glfw m)
    krunknative_link_gl(projectiles_render_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(projectiles_render_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # death_cam_test: the death chase camera (client/src/menu_view.c) stays near the death position
    # and out of solid boxes (walls behind the victim, long frames, Kanji / Sandstorm spawns). GL-free:
    # it includes menu_view.c and stubs spectate_active, the GL target only provides headers.
    add_executable(death_cam_test client/tests/death_cam_test.c ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_include_directories(death_cam_test PRIVATE client/include client/lib)
    target_link_libraries(death_cam_test PRIVATE freetype glfw m)
    krunknative_link_gl(death_cam_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(death_cam_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # third_person_test: the KrunkNative third-person camera (client/src/third_person_cam.c): the
    # collision pull-in (world_solid_fraction: boxes, map triangles, terrain) on synthetic walls and
    # every Kanji / Sandstorm spawn, the aim correction, the server's `thirdperson` permission.
    # GL-free: it includes third_person_cam.c, the GL target only provides headers.
    add_executable(third_person_test client/tests/third_person_test.c ${SHARED_HEADERS} ${SHARED_SOURCES})
    target_include_directories(third_person_test PRIVATE client/include client/lib)
    target_link_libraries(third_person_test PRIVATE freetype glfw m)
    krunknative_link_gl(third_person_test)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(third_person_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # BACKEND SERVICES (services/, doc/backend.md): knsvc (+ knsvc_db, kn_migrate and the
    # brain when libpq is available). Optional; never affects the game targets. Tests: ctest.
    option(KRUNKNATIVE_SERVICES "Build the backend services (services/; needs OpenSSL, libpq for the DB parts)" ON)
    if (KRUNKNATIVE_SERVICES AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" AND EXISTS ${CMAKE_SOURCE_DIR}/services/CMakeLists.txt)
        enable_testing()
        add_subdirectory(services)
    endif ()

    # Game server brain client (server/src/brain.c, doc/backend.md section 8): links the
    # libpq-free knsvc when it was built; without it server_bin stays standalone-only.
    if (TARGET knsvc)
        target_link_libraries(${SERVER_BINARY} PRIVATE knsvc)
        target_compile_definitions(${SERVER_BINARY} PRIVATE KRUNKNATIVE_HAVE_KNSVC=1)
    endif ()
endif ()
