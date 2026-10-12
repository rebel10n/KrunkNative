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
    target_link_libraries(client PRIVATE freetype cJSON pcg_basic m)

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
    #   --preload-file             the window templates, read with fopen() from /ui/windows (MEMFS)
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
    add_custom_target(web_dist ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WEB_DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${WEB_DIST_DIR}/js
        COMMAND ${CMAKE_COMMAND} -E ${KN_WEB_COPY_UI} ${CMAKE_SOURCE_DIR}/ui ${WEB_DIST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:client> ${WEB_DIST_DIR}/client.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:client>/client.wasm ${WEB_DIST_DIR}/client.wasm
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:client>/client.data ${WEB_DIST_DIR}/client.data
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${WEB_DIST_DIR}/assets
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/assets ${WEB_DIST_DIR}/assets
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
    target_link_libraries(${CLIENT_BINARY} PRIVATE glfw freetype Threads::Threads m)
    # the offline sandbox server (server_sandbox.h): the server core + its no-network backend
    target_sources(${CLIENT_BINARY} PRIVATE ${SERVER_CORE_STANDALONE_SOURCES})
    target_link_libraries(${CLIENT_BINARY} PRIVATE kn_server_core)
    krunknative_link_gl(${CLIENT_BINARY})
    # miniaudio (client/src/miniaudio.c) loads its audio backends with dlopen().
    target_link_libraries(${CLIENT_BINARY} PRIVATE ${CMAKE_DL_LIBS})
    krunknative_link_headless(${CLIENT_BINARY})
    target_include_directories(${CLIENT_BINARY} PRIVATE client/include)
    # Optional TLS for the backend client (client/src/net/backend_native.c: https:// auth service
    # and server finder, wss:// social socket). Without OpenSSL only http:// / ws:// work.
    find_package(OpenSSL COMPONENTS SSL Crypto QUIET)
    if (OpenSSL_FOUND AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(${CLIENT_BINARY} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_HAVE_TLS=1)
    else ()
        message(STATUS "Client: OpenSSL (libssl) not found; the backend client is http:// only")
    endif ()

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
    target_link_libraries(kn_server_core PUBLIC Threads::Threads m)

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
        target_link_libraries(${CLIENT_BINARY} PRIVATE ws2_32)
        target_link_libraries(${SERVER_BINARY} PRIVATE ws2_32)
        target_link_libraries(kn_server_core PUBLIC ws2_32)
    endif ()

    # TESTS
    add_executable(proc_input_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/proc_input_test.c)
    target_link_libraries(proc_input_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(proc_input_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

    # knet UDP transport (shared/src/knet.c only: no game code, no sockets)
    add_executable(knet_test shared/include/knet.h shared/src/knet.c shared/tests/knet_test.c)

    # editor bus (shared/src/editor_bus.c: framing, scopes, tokens, unix socket + TCP fallback)
    add_executable(editor_bus_test shared/include/editor_bus.h shared/src/editor_bus.c shared/tests/editor_bus_test.c)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(editor_bus_test PRIVATE ws2_32)
    endif ()

    add_executable(class_spawn_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/class_spawn_test.c)
    target_link_libraries(class_spawn_test PRIVATE m)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(class_spawn_test PRIVATE ws2_32) # shared/src/net.c
    endif ()

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

    # KrunkScript (doc/v7/krunkscript.md 6.6): ksc (check; WP2 adds run) over the ks
    # sources only, and ks_front_test (lexer/parser/checker + shared/tests/ks suite)
    file(GLOB KS_SOURCES shared/src/ks/*.c)
    add_executable(ksc tools/ks/ksc.c ${KS_SOURCES})
    target_link_libraries(ksc PRIVATE m)
    set(KS_FRONT_SOURCES shared/src/ks/ks_lex.c shared/src/ks/ks_parse.c shared/src/ks/ks_check.c shared/src/ks/ks_api_table.c)
    add_executable(ks_front_test shared/tests/ks_front_test.c ${KS_FRONT_SOURCES})
    enable_testing()
    add_test(NAME ks_front_test COMMAND ks_front_test ${CMAKE_SOURCE_DIR}/shared/tests/ks)

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
