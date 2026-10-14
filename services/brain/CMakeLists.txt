# services/brain: kn_brain, the matchmaker "brain" (doc/backend.md).
# Owners: svc-matchmaker (main, registry, routes, internal WS, federation) and
# svc-accounts (accounts, social WS). Add sources to KN_BRAIN_SOURCES with an
# anchored edit.

set(KN_BRAIN_SOURCES
    main.c
    brain_util.c
    registry.c
    federation.c
    internal_ws.c
    public_routes.c
    host.c
    ranked.c
    # svc-accounts sources
    accounts.c
    social_ws.c
    profiles.c
    leaderboards.c
    social_data.c
    creations.c
    store.c
    shared_shim.c
    # coord-auth: roles, central auth, server finder
    roles.c
    auth.c
    auth_state.c
    finder.c
    finder_table.c
    # ci-release: latest server release (GitHub Releases poll) + the outdated rule
    release_info.c
    releases.c
    # coord-store: multi-statement database operations (kn_procs, db_procs.h)
    db_common.c
    db_accounts.c
    db_social.c
    db_mm.c
    db_ranked.c
    db_turf.c                       # v10 Turf Wars (0206_turf_wars); rules shared/src/turf_wars.c
    turf.c
    ../../shared/src/turf_wars.c
    # ks-6 (WP6): KrunkScript storage + map KR rewards; the value rules are the game server's
    db_storage.c
    ../../shared/src/ks_storage_data.c
    ../../shared/src/ks/ks_number.c
    ../../shared/src/map_catalog.c  # relative: knsvc_existing() checks paths under this dir
    ../../shared/src/ranked.c       # v7 ranked rating rules (ranked.c)
    # player-viewer WP-E part 2c: badges, v7 challenges, medal counts (0107_progression); the
    # challenge / badge rules are the game's (shared/src/progression.c, badges.c)
    db_progress.c
    progress.c
    ../../shared/src/progression.c
    ../../shared/src/badges.c
    # server-names: game ids and their short link form (registry_lobby_find)
    ../../shared/src/server_names.c
    # v7.1.4 seasons and the Battle Pass (0108_seasons); the rules are shared/src/season.c
    db_season.c
    season.c
    ../../shared/src/season.c
    # email-verify: v7.1.4 2FA (TOTP), email verification and outbound mail (0109_account_security)
    account_security.c
    db_security.c
    # redis-tier: optional Redis / Valkey hot tier (stubs without hiredis; doc/backend.md 3.11)
    hot.c
    # editor-ai: the map editor's AI proxy (/ai/models, /ai/chat, health probe, server tools)
    ai.c
    ai_search.c                     # web search: a DuckDuckGo MCP server as a stdio child
    ../../shared/src/ai_proto.c
)

# Sources another agent has listed but not landed yet must not break configure
# (knsvc_existing: services/common/CMakeLists.txt).
knsvc_existing(KN_BRAIN_SOURCES ${KN_BRAIN_SOURCES})
add_executable(kn_brain ${KN_BRAIN_SOURCES})
target_include_directories(kn_brain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# svc-accounts: shared/include for map_catalog.h (creations.c; shim in shared_shim.c)
target_include_directories(kn_brain PRIVATE ${CMAKE_SOURCE_DIR}/shared/include)
target_link_libraries(kn_brain PRIVATE knsvc_db kn_build_info)
# coord-auth: community hasJoined over https:// (auth.c) needs libcurl; http:// works without
find_package(CURL QUIET)
if (CURL_FOUND)
    target_link_libraries(kn_brain PRIVATE CURL::libcurl)
    target_compile_definitions(kn_brain PRIVATE KN_HAVE_CURL=1)
endif ()
target_compile_options(kn_brain PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(kn_brain PRIVATE _GNU_SOURCE)
# redis-tier: the hot tier links hiredis only through knsvc_redis (KNSVC_REDIS); without it
# hot.c compiles to stubs and the brain runs on its database alone.
if (TARGET knsvc_redis)
    target_link_libraries(kn_brain PRIVATE knsvc_redis)
    target_compile_definitions(kn_brain PRIVATE KN_HAVE_REDIS=1)
endif ()

# Registry unit test (pure, no DB).
add_executable(kn_brain_registry_test tests/test_registry.c registry.c ${CMAKE_SOURCE_DIR}/shared/src/server_names.c)
target_include_directories(kn_brain_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/shared/include)
target_link_libraries(kn_brain_registry_test PRIVATE knsvc)
add_test(NAME brain_registry COMMAND kn_brain_registry_test)

# coord-auth: auth join records / limiters and the finder table (pure, no DB).
add_executable(kn_brain_auth_test tests/test_auth_finder.c auth_state.c finder_table.c)
target_include_directories(kn_brain_auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/shared/include)
target_sources(kn_brain_auth_test PRIVATE ${CMAKE_SOURCE_DIR}/shared/src/auth_join.c)
target_link_libraries(kn_brain_auth_test PRIVATE knsvc)
add_test(NAME brain_auth_finder COMMAND kn_brain_auth_test)
set_tests_properties(brain_auth_finder PROPERTIES LABELS "knsvc;brain")

# ci-release: kn-build release metadata + the outdated / incompatible rule (pure).
add_executable(kn_brain_releases_test tests/test_releases.c release_info.c)
target_include_directories(kn_brain_releases_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kn_brain_releases_test PRIVATE knsvc)
add_test(NAME brain_releases COMMAND kn_brain_releases_test)
set_tests_properties(brain_releases PROPERTIES LABELS "knsvc;brain")
set_tests_properties(brain_registry PROPERTIES LABELS "knsvc;brain")

# svc-accounts: accounts + social socket end to end against PostgreSQL (own schema;
# skips with 77 when the database or python websockets is unavailable).
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_FOUND)
    add_test(NAME brain_social_ws
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/social_ws_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_social_ws PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
endif ()

# Two brains sharing one database, fake game servers in two regions, failover.
# Each run uses its own schema (config database, or $KN_MM_TEST_DATABASE) and
# ephemeral ports; skipped (77) when PostgreSQL is unreachable.
add_test(NAME brain_federation
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/federation_test.sh ${CMAKE_BINARY_DIR})
set_tests_properties(brain_federation PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")

# coord-auth: central auth + finder + community coordinator + 2 dedicated servers + headless
# clients in online and offline mode (needs server_bin and client_bin; skip 77 without a DB).
if (TARGET ${SERVER_BINARY} OR TARGET server_bin)
    add_test(NAME brain_auth_e2e
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/federation_auth_e2e.sh ${CMAKE_BINARY_DIR})
    set_tests_properties(brain_auth_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
    # ks-6 (KrunkScript WP6): GAME.STORAGE of a hosted published map through server_bin + kn_brain
    if (Python3_FOUND)
        add_test(NAME brain_ks_storage_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/ks_storage_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_ks_storage_e2e PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # mm-fill: quick join fills the populated lobby (3 server_bin nodes; sequential seekers
        # inside the lobby-up debounce and concurrent seekers all land together)
        add_test(NAME brain_mm_fill_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/mm_fill_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_mm_fill_e2e PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # shutdown-msg: server_bin's shutdown ERROR (SIGTERM / reason file "update" / quit restart)
        # and headless client_bin rejoining a restarted node with a fresh join ticket, also after
        # "Join key expired" (skips 77 without a database or client_bin)
        add_test(NAME brain_restart_rejoin_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/restart_rejoin_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_restart_rejoin_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # editor WP6: the map editor's publishing, My Games and asset windows (headless client_bin
        # --editor against kn_brain; skips 77 without a database, websockets or client_bin)
        add_test(NAME brain_editor_publish_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/editor/publish_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_editor_publish_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e;editor")
        # editor-ai: the editor's AI panel against kn_brain + a stub upstream / stub MCP (headless
        # client_bin --editor: guest gating, a streamed reply, editor tools, approvals, web search, history)
        add_test(NAME brain_editor_ai_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/editor/ai_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_editor_ai_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e;editor;ai")
    endif ()
endif ()

# Ranked queue end to end (2 brains, fake game server, 4 accounts; per-run schema too).
if (Python3_FOUND)
    add_test(NAME brain_ranked
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ranked_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ranked PROPERTIES TIMEOUT 420 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # v10 Turf Wars: turf-kills from a fake game server, the daily settlement, loot configs, routes
    add_test(NAME brain_turf
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/turf_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_turf PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # ks-6 (KrunkScript WP6): game-server storage / map KR reward ops on the database
    add_test(NAME brain_storage
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_storage PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # player-viewer WP-E part 2c: badges, v7 challenges, medal counts (db_progress.c, 0107_progression)
    add_test(NAME brain_progress
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/progress_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_progress PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # v7.1.4 seasons and the Battle Pass (db_season.c, season.c, 0108_seasons)
    add_test(NAME brain_season
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/season_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_season PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # editor-ai: the AI proxy (ai.c) against a stub upstream and a stub MCP server (tests/ai_stub.py; sqlite
    # scratch database unless KN_TEST_DB_DRIVER says otherwise; never the live provider)
    add_test(NAME brain_ai
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ai PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;ai")
    # the same panel in the web client (headless Chromium on KN_WEB_DIST = a `make web` dist dir; else skips 77)
    add_test(NAME brain_ai_web_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_web_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ai_web_e2e PROPERTIES TIMEOUT 480 SKIP_RETURN_CODE 77 LABELS "brain;db;ai;e2e;web")
    # email-verify: verification mails through a fake SMTP server, links, rate limits, gates, 2FA lock
    add_test(NAME brain_account_security
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/account_security_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_account_security PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;smtp")
    # the same through the web client in headless Chromium (needs KN_WEB_DIST = a `make web` dist dir; else skips 77)
    add_test(NAME brain_email_web_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/email_verify_web_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_email_web_e2e PROPERTIES TIMEOUT 480 SKIP_RETURN_CODE 77 LABELS "brain;db;smtp;e2e;web")
endif ()

# coord-store: the same three suites on the other store drivers (KN_TEST_DB_DRIVER,
# tests/db_scratch.py). sqlite needs no server (temp file); mysql uses
# KN_TEST_MYSQL_HOST/PORT/USER/PASSWORD (default 127.0.0.1:3306 root) and skips
# (77) when no MySQL/MariaDB server answers. ctest -L sqlite / -L mysql.
foreach (drv IN ITEMS sqlite mysql)
    string(TOUPPER ${drv} DRV)
    if (NOT KNSVC_STORE_${DRV})
        continue()
    endif ()
    if (Python3_FOUND)
        add_test(NAME brain_social_ws_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/social_ws_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_ranked_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ranked_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_turf_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/turf_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_storage_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_progress_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/progress_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_season_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/season_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_account_security_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/account_security_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_social_ws_${drv} brain_ranked_${drv} brain_turf_${drv} brain_storage_${drv} brain_progress_${drv} brain_season_${drv}
            brain_account_security_${drv}
            PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
            ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;${drv}")
        set_tests_properties(brain_ranked_${drv} PROPERTIES TIMEOUT 420)
    endif ()
    add_test(NAME brain_federation_${drv}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/federation_test.sh ${CMAKE_BINARY_DIR})
    set_tests_properties(brain_federation_${drv} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
        ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;${drv}")
endforeach ()

# redis-tier: the hot tier (doc/backend.md 3.11). The suites again with every brain on a
# throwaway redis-server / valkey-server (tests/with_redis.py: KN_REDIS_URL; fails when the
# brains never used it), on sqlite and postgres; plus the failover test (two brains, Redis
# killed mid-run). All skip (77) without a server binary / database / python websockets.
if (Python3_FOUND AND TARGET knsvc_redis)
    set(_kn_wr ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/with_redis.py)
    foreach (drv IN ITEMS postgres sqlite)
        string(TOUPPER ${drv} DRV)
        if (NOT KNSVC_STORE_${DRV})
            continue()
        endif ()
        set(_sfx _${drv}_redis)
        if (drv STREQUAL "postgres")
            set(_sfx _redis)
        endif ()
        foreach (suite IN ITEMS social_ws ranked season)
            add_test(NAME brain_${suite}${_sfx}
                COMMAND ${_kn_wr} ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}_test.py
                        $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
            set_tests_properties(brain_${suite}${_sfx} PROPERTIES TIMEOUT 450 SKIP_RETURN_CODE 77
                ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;redis;${drv}")
        endforeach ()
        add_test(NAME brain_federation${_sfx}
            COMMAND ${_kn_wr} ${CMAKE_CURRENT_SOURCE_DIR}/tests/federation_test.sh ${CMAKE_BINARY_DIR})
        set_tests_properties(brain_federation${_sfx} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
            ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;redis;${drv}")
        add_test(NAME brain_redis_failover_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/redis_failover_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_redis_failover_${drv} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
            ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;redis;${drv}")
    endforeach ()
endif ()

# redis-tier: kn_dbcopy SQLite -> PostgreSQL (the production cutover's copy + verification)
if (Python3_FOUND AND TARGET kn_dbcopy)
    add_test(NAME brain_dbcopy
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/dbcopy_test.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_dbcopy PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;postgres;sqlite")
endif ()

# Fake game server node for integration tests (speaks the internal WS protocol).
add_executable(kn_fakenode tools/kn_fakenode.c)
target_link_libraries(kn_fakenode PRIVATE knsvc)
target_compile_definitions(kn_fakenode PRIVATE _GNU_SOURCE)

# redis-tier: closed-loop load generator for the hot endpoints (tests/load_test.py)
add_executable(kn_loadgen tools/kn_loadgen.c)
target_link_libraries(kn_loadgen PRIVATE knsvc Threads::Threads)
target_compile_definitions(kn_loadgen PRIVATE _GNU_SOURCE)

# Build the test + tool with the brain (services_all picks up kn_brain).
add_dependencies(kn_brain kn_brain_registry_test kn_fakenode kn_brain_releases_test kn_loadgen)
