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
    mm_token.c                      # mm-token: seek-game validation tokens (/generate-token)
    host.c
    ranked.c
    # svc-accounts sources
    accounts.c
    social_ws.c
    social_eb.c
    social_gapi.c
    db_social_v10.c
    clan_v10.c
    clan_logo.c                     # clan logos (0332_clan_logos): the settings tab's upload, its review, GET /api/clans/<id>/logo
    studios.c                       # v10 Studios: the /studios gapi routes (db_studios.c, 0330_studios)
    game_presence.c                 # v10 presence:game (account -> its game, pushed to friends)
    profiles.c
    leaderboards.c
    social_data.c
    economy.c
    creations.c
    store.c
    ../../shared/src/store_spins.c        # v10 wheels and item pools (shared/include/store_spins.h)
    ../../shared/src/store_spins_table.c  # generated: gapi /data/spins (tools/ui-import/gen_viewer_store.js)
    daily_spin.c                    # v10 Daily Spin ("dsp" / "dsl")
    redeem.c                        # v10 voucher codes ("vr": accounts.redeem_codes)
    craft.c                         # the Junkyard: recipes ("recps"), crafting ("crft"), assets/junkyard_recipes.json
    wild_offer.c                    # v10 Wild Offers ("wo" / "wob" / "wor")
    # v10 double XP: the end-screen 2x XP claim ("rqdxp"), the weekend schedules and the offer (0341_double_xp)
    db_double_xp.c                  # the pure rules: ../../shared/src/double_xp.c (every shared/src/*.c is in the brain)
    double_xp_social.c
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
    db_economy.c
    db_junk.c                       # the Junk wallet (0347_junk_wallet; rules ../../shared/src/junk.c, doc/v10/junk.md)
    ../../shared/src/junk.c
    db_mm.c
    db_ranked.c
    db_turf.c                       # v10 Turf Wars (0206_turf_wars); rules shared/src/turf_wars.c
    db_lottery.c                    # v10 lottery windows, entries, draws (0348_lottery; lottery.c)
    lottery.c
    db_shop.c                       # the Junk-priced shop: display, purchase (0349_shop_junk; shop.c, doc/v10/junk.md)
    shop.c
    db_kpd.c                        # the player list's K.P.D distress calls (0209_kpd_calls)
    db_studios.c                    # v10 Studios: members, games, currencies, monetization (0330_studios)
    turf.c
    community.c                     # v10 Community & Events / news feed (/creators/events, /social/news)
    ../../shared/src/turf_wars.c
    # ks-6 (WP6): KrunkScript storage + map KR rewards; the value rules are the game server's
    db_storage.c
    ../../shared/src/ks_storage_data.c
    ../../shared/src/ks/ks_number.c
    ../../shared/src/map_catalog.c  # relative: knsvc_existing() checks paths under this dir
    ../../shared/src/flavor.c       # map_catalog.c: the Krunker Strike map pool (strike_maps.h)
    ../../shared/src/rules/strike/maps_strike.c
    ../../shared/src/rules/strike/strike_data.c # flavor.c: FlavorConfig.rules (strike_rules.h data only)
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
    # v10.0.2 one-time signup perks (0116_v10_rewards)
    db_rewards.c
    rewards.c
    # strike-economy: Krunker Strike's skins, spins, store, krunkies, missions, progression and
    # leaderboards (0320_strike_economy; the rules and the bundle's XP curve: shared/src/strike_economy.c)
    strike_econ.c
    db_strike.c
    ../../shared/src/strike_economy.c
    ../../shared/src/rules/strike/strike_economy_data.c
    # email-verify: v7.1.4 2FA (TOTP), email verification and outbound mail (0109_account_security)
    account_security.c
    db_security.c
    # redis-tier: optional Redis / Valkey hot tier (stubs without hiredis; doc/backend.md 3.11)
    hot.c
    # editor-ai: the map editor's AI proxy (/ai/models, /ai/chat, health probe, server tools)
    ai.c
    ai_provider.c                   # named AI provider profiles: ai.<consumer>.provider routing, keys, limits
    ai_search.c                     # web search: a DuckDuckGo MCP server as a stdio child
    # v10 Live Streams: GET /streams/top from Twitch Helix (streams.c; streams_norm.c is pure)
    streams.c
    streams_norm.c
    stream_thumbs.c                 # the Live Streams cards' Twitch previews: GET /streams/thumb (allow-listed, capped, cached)
    user_assets.c                   # GET /api/user-assets/<id>/<file>: Krunker's user-asset host, mirrored + cached
    ../../shared/src/user_asset_path.c # the user-asset paths and content rules (shared with the client's fetch)
    ../../shared/src/knv.c          # video.mp4.knv: the frame sidecar format (shared with the clients' players)
    ../../shared/src/youtube_link.c # map YouTube video ids (creations.c: uploadM stores the id)
    ../../shared/src/ai_proto.c
    # profile pictures (0123_account_avatars): intake (PNG / JPEG / WebP, libwebp encode), the
    # AI review's verdict rules, the rows; stb_image is the decoder (avatar_stb.c holds its body)
    avatar.c
    avatar_image.c
    avatar_verdict.c
    db_avatar.c
    avatar_stb.c
    # Web Push subscriptions (0344_push_subscriptions): the subscribe side, the send is a later step
    push.c
    push_rules.c
    db_push.c
)

# The publish checks are the editor's validateMap (shared/src/map_validate.c): it needs the prefab,
# trigger and ruleset tables, so kn_brain links the whole shared game core like kn_editor_mcp does,
# except utils.c and build_info.c (the shim above supplies utils' four asset helpers).
list(FILTER KN_BRAIN_SOURCES EXCLUDE REGEX "^\\.\\./\\.\\./shared/src/")
file(GLOB_RECURSE KN_BRAIN_SHARED_CORE CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shared/src/*.c)
list(FILTER KN_BRAIN_SHARED_CORE EXCLUDE REGEX "/(utils|build_info)\\.c$")
foreach (KN_SRC IN LISTS KN_BRAIN_SHARED_CORE) # knsvc_existing() wants paths relative to this directory
    file(RELATIVE_PATH KN_SRC_REL ${CMAKE_CURRENT_SOURCE_DIR} ${KN_SRC})
    list(APPEND KN_BRAIN_SOURCES ${KN_SRC_REL})
endforeach ()

# Sources another agent has listed but not landed yet must not break configure
# (knsvc_existing: services/common/CMakeLists.txt).
knsvc_existing(KN_BRAIN_SOURCES ${KN_BRAIN_SOURCES})
add_executable(kn_brain ${KN_BRAIN_SOURCES})
target_include_directories(kn_brain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/client/include)
# avatar.c: WebP decode / encode for profile pictures (libwebp; without it uploads answer 503
# AvatarsUnavailable and the rest of the brain is unchanged)
if (PkgConfig_FOUND)
    pkg_check_modules(KN_WEBP QUIET IMPORTED_TARGET libwebp)
    if (KN_WEBP_FOUND)
        target_link_libraries(kn_brain PRIVATE PkgConfig::KN_WEBP)
        target_compile_definitions(kn_brain PRIVATE KN_HAVE_WEBP=1)
    endif ()
endif ()
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

# editor-ai: named AI provider profiles (ai_provider.c): three profiles and three consumers, the legacy
# single key, env precedence, routing failures, limits, redaction of every dump (pure, no DB).
add_executable(kn_brain_ai_provider_test tests/test_ai_provider.c ai_provider.c)
target_include_directories(kn_brain_ai_provider_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kn_brain_ai_provider_test PRIVATE knsvc)
add_test(NAME brain_ai_provider COMMAND kn_brain_ai_provider_test)
set_tests_properties(brain_ai_provider PROPERTIES LABELS "knsvc;brain;ai")

# profile pictures (pure, no DB): the verdict table and the picture intake (type, size, dimensions,
# square WebP output). Fixtures are written to the build dir; WebP cases only with libwebp.
add_executable(kn_brain_avatar_test
    tests/test_avatar.c avatar_verdict.c avatar_image.c avatar_stb.c ${CMAKE_SOURCE_DIR}/client/src/png_write.c)
target_include_directories(kn_brain_avatar_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/client/include)
target_link_libraries(kn_brain_avatar_test PRIVATE knsvc)
target_compile_definitions(kn_brain_avatar_test PRIVATE _GNU_SOURCE)
if (KN_WEBP_FOUND)
    target_link_libraries(kn_brain_avatar_test PRIVATE PkgConfig::KN_WEBP)
    target_compile_definitions(kn_brain_avatar_test PRIVATE KN_HAVE_WEBP=1)
endif ()
add_test(NAME brain_avatar COMMAND kn_brain_avatar_test ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(brain_avatar PROPERTIES LABELS "knsvc;brain;avatar")

# Web Push (push_rules.c, pure, no database): the endpoint and key rules of the subscribe route
add_executable(kn_brain_push_test tests/test_push_validation.c push_rules.c)
target_include_directories(kn_brain_push_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME brain_push_validation COMMAND kn_brain_push_test)
set_tests_properties(brain_push_validation PROPERTIES LABELS "knsvc;brain;push")

# v10 double XP: the weekend schedules, the ISO week, the per-match offer gate and the placeholder (pure)
add_executable(kn_brain_double_xp_test tests/test_double_xp.c ${CMAKE_SOURCE_DIR}/shared/src/double_xp.c)
target_include_directories(kn_brain_double_xp_test PRIVATE ${CMAKE_SOURCE_DIR}/shared/include)
add_test(NAME brain_double_xp COMMAND kn_brain_double_xp_test)
set_tests_properties(brain_double_xp PROPERTIES LABELS "knsvc;brain;economy")

# Registry unit test (pure, no DB).
add_executable(kn_brain_registry_test tests/test_registry.c registry.c ${CMAKE_SOURCE_DIR}/shared/src/server_names.c)
target_include_directories(kn_brain_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/shared/include)
target_link_libraries(kn_brain_registry_test PRIVATE knsvc)
add_test(NAME brain_registry COMMAND kn_brain_registry_test)
# mm-token: seek-game validation tokens (pure, no DB)
add_executable(kn_brain_mm_token_test tests/test_mm_token.c mm_token.c)
target_include_directories(kn_brain_mm_token_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kn_brain_mm_token_test PRIVATE knsvc)
add_test(NAME brain_mm_token COMMAND kn_brain_mm_token_test)
set_tests_properties(brain_mm_token PROPERTIES LABELS "knsvc;brain")

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

# v10 Live Streams: the pure normaliser and the token / game answers (streams_norm.c)
add_executable(kn_brain_streams_test tests/test_streams.c streams_norm.c)
target_include_directories(kn_brain_streams_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kn_brain_streams_test PRIVATE knsvc)
add_test(NAME brain_streams COMMAND kn_brain_streams_test)
set_tests_properties(brain_streams PROPERTIES LABELS "knsvc;brain")
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
    # the social socket's rate limits: per-class buckets, refused frames answered, coalesced pushes
    add_test(NAME brain_social_ratelimit
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/social_ratelimit_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_social_ratelimit PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")

    # community maps: uploadM checks (validateMap, category, name), the Games window's category and
    # official / community filters, map_info mode tags, /api/map-data (creations.c)
    add_test(NAME brain_community_maps
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/community_maps_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_community_maps PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")

    # flavor-matchmaking: the Krunker Strike flavor pool in the finder (finder.c, finder_table.c).
    # No database (kn_brain --role finder --no-db); fake UDP KNINFO responders stand in for the game
    # servers, so nothing links libpq and no server_bin is needed.
    add_test(NAME brain_finder_flavor_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/finder_flavor_e2e.py ${CMAKE_BINARY_DIR})
    set_tests_properties(brain_finder_flavor_e2e PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;e2e")
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
        # strike-backend: one coordinator for both apps (doc/backend.md "Flavor pools"): a Krunker
        # Strike and a base server_bin on one brain; seeks, lists and joins per flavor, both clients
        # headless (skips 77 without a database; the client cases without client_bin / fonts)
        add_test(NAME brain_strike_coordinator_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/strike_coordinator_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_strike_coordinator_e2e PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # shutdown-msg: server_bin's shutdown ERROR (SIGTERM / reason file "update" / quit restart)
        # and headless client_bin rejoining a restarted node with a fresh join ticket, also after
        # "Join key expired" (skips 77 without a database or client_bin)
        add_test(NAME brain_restart_rejoin_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/restart_rejoin_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_restart_rejoin_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # mm-token: /mm/generate-token -> seek-game ?validationToken= -> the join ticket, the v10 seek
        # preferences (dsmo / dsma), official krunker.io lobbies (a stub), F4 / F5 (headless client_bin;
        # skips 77 without a database or client_bin)
        add_test(NAME brain_mm_token_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/mm_token_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_mm_token_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # v10 Invite / Join: presence:game to friends, Invite link, Join from the clipboard / window /
        # a friend's game, the server password popup (3 nodes, headless client_bin on ui/)
        add_test(NAME brain_v10_party_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/v10_party_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_v10_party_e2e PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # editor WP6: the map editor's publishing, My Games and asset windows (headless client_bin
        # --editor against kn_brain; skips 77 without a database, websockets or client_bin)
        add_test(NAME brain_editor_publish_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/editor/publish_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_editor_publish_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e;editor")
        # community maps in the Games window: browse (New, mode tags, NEW!), search by name /
        # category / source, the Host Game pick and Play of a published map, a spawnless row refused
        # (headless client_bin on ui/; skips 77 without a database, websockets or client_bin)
        add_test(NAME brain_community_games_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/community_games_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_community_games_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
        # published maps hosted by server_bin (published:<creator>/<name>: the map argument, --set
        # gameserver.maps, server.cfg's mapcycle, a brain host request, a missing map, the disk cache
        # with the brain down) and picked in the Host Game flow; a client joins one
        add_test(NAME brain_published_host_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/server/tests/published_host_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_published_host_e2e PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
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
    # the in-game player list's K.P.D call: kpd_call / kpd_calls / kpd_log (db_kpd.c, 0209_kpd_calls)
    add_test(NAME brain_kpd
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/kpd_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_kpd PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
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
    # v10.0.2 rewards: the signup perks, claim all, krdata / leveldata and gifting (db_rewards.c, 0116_v10_rewards)
    add_test(NAME brain_rewards
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/rewards_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_rewards PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # Krunker Strike's economy: catalogue seeded (0320), spin odds / costs / grants, store, loadout,
    # missions, track, Strike commits and auth, Strike vs Box Office separation, leaderboards
    add_test(NAME brain_strike_economy
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/strike_economy_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_strike_economy PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;economy;strike")
    # an equipped Strike skin renders in game (headless client_bin --flavor strike on a linked Strike server_bin)
    if (TARGET client_bin AND TARGET server_bin)
        add_test(NAME brain_strike_skin_e2e
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/client/tests/strike_skin_e2e.py ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_strike_skin_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "brain;db;economy;strike;e2e")
    endif ()
    # editor-ai: the AI proxy (ai.c) against a stub upstream and a stub MCP server (tests/ai_stub.py; sqlite
    # scratch database unless KN_TEST_DB_DRIVER says otherwise; never the live provider)
    add_test(NAME brain_ai
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ai PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;ai")
    # v10 Live Streams: /streams/top against a stub Twitch (tests/streams_test.py; never the live API)
    add_test(NAME brain_streams_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/streams_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_streams_e2e PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # the clients' mirror of Krunker's user-asset host: /api/user-assets/<id>/<file> against a stub host
    add_test(NAME brain_user_assets_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/user_assets_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_user_assets_e2e PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # the same panel in the web client (headless Chromium on KN_WEB_DIST = a `make web` dist dir; else skips 77)
    add_test(NAME brain_ai_web_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_web_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ai_web_e2e PROPERTIES TIMEOUT 480 SKIP_RETURN_CODE 77 LABELS "brain;db;ai;e2e;web")
    # editor-ai: named provider profiles on kn_brain (ai_provider.c): three stub upstreams with their own keys,
    # the editor routed to its profile, the others see nothing; counters and status (never a key)
    add_test(NAME brain_ai_providers_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_providers_e2e.py $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ai_providers_e2e PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "brain;db;ai;e2e")
    # email-verify: verification mails through a fake SMTP server, links, rate limits, gates, 2FA lock
    add_test(NAME brain_account_security
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/account_security_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_account_security PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;smtp")
    # profile pictures: upload, the AI review through a stubbed gateway (pass / fail / hidden until
    # reviewed), quota, delete, and the gateway key kept out of responses and logs (0123_account_avatars)
    add_test(NAME brain_avatar_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/avatar_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_avatar_e2e PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;avatar")
    # the economy (doc/v7/economy.md): gifts, market, trade offers, idempotency, bans, rate limits,
    # the ledger and parallel gifts / buys / offers / accepts at the same balance and items
    add_test(NAME brain_economy
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/economy_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    # the Junk wallet (doc/v10/junk.md): credit, spend, "Not enough Junk", the social "uj" push, parallel spends
    add_test(NAME brain_junk
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/junk_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_junk PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;junk")
    # v10 lottery (lottery.c, 0348_lottery): the guest view, skin / Junk / RP entries, the timer's draw
    add_test(NAME brain_lottery
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/lottery_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_lottery PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;lottery")
    # the Junk-priced shop (shop.c, 0349_shop_junk): the display, a purchase with Junk, the limits, the ledgers
    add_test(NAME brain_shop
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/shop_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_shop PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;shop")
    set_tests_properties(brain_economy PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;economy")
    # the Junkyard (craft.c): the recipe list, crafting (materials, the world limit, junk refusals, a non-craftable result)
    add_test(NAME brain_craft
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/craft_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_craft PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db;craft")
    # v10.0.2 clan window backend: cln 6/9, clnRA, s7, MOTD / banner / logo / transfer (clan_v10.c, 0113)
    add_test(NAME brain_clan_v10
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/clan_v10_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_clan_v10 PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # v10.0.2 Studios: the /studios gapi routes (studios.c, db_studios.c, 0330_studios)
    add_test(NAME brain_studios
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/studios_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_studios PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # v10 double XP (0341_double_xp): the end-screen 2x XP offer, the claim, the weekend schedules (Box Office commits)
    foreach (drv IN ITEMS postgres sqlite)
        string(TOUPPER ${drv} DRV)
        if (Python3_FOUND AND KNSVC_STORE_${DRV})
            add_test(NAME brain_double_xp_${drv}
                COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/double_xp_test.py
                        $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
            set_tests_properties(brain_double_xp_${drv} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
                ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;economy;${drv}")
        endif ()
    endforeach ()
    # the same through the v10 page: headless client_bin (client/src/ui/v10_studios.c)
    add_test(NAME brain_v10_studios_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/client/tests/v10_studios_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_v10_studios_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 LABELS "brain;db;e2e")
    # v10.0.2 Social hub on the v10 page through two headless clients (v10_social.c, social_gapi.c, social_eb.c)
    add_test(NAME brain_v10_social_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/v10_social_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_v10_social_e2e PROPERTIES TIMEOUT 480 SKIP_RETURN_CODE 77 LABELS "brain;db;social;e2e")
    # creators: v10's creator badge ("crt", 0210_creator_badge) and the maps' YouTube video ids
    add_test(NAME brain_creator
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/creator_test.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_creator PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # the same in headless client_bin runs: the hub's Creator button, a map's YouTube link, the menu's
    # creator mark and the Live Streams previews through /streams/thumb (stub Twitch + stub CDN)
    add_test(NAME brain_creator_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/creator_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_creator_e2e PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 LABELS "brain;db;social;e2e")
    # v10.0.2 Social hub backend: friends event bus + gapi tunnel (social_eb.c, social_gapi.c, 0112)
    add_test(NAME brain_social_v10
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/social_v10_test.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_social_v10 PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # Krunker Strike's friends: game:invite, presence gameFlavor + lobby details (social_eb.c, game_presence.c)
    add_test(NAME brain_strike_social
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/strike_social_test.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_strike_social PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
    # the same through the clients' hub: headless client_bin (KRUNKNATIVE_UI_CMDS) and the web build
    # in headless Chromium (needs KN_WEB_DIST = a `make web` dist dir; else skips 77)
    add_test(NAME brain_economy_hub_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/economy_hub_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_economy_hub_e2e PROPERTIES TIMEOUT 480 SKIP_RETURN_CODE 77 LABELS "brain;db;economy;e2e")
    # the v10 page's Market & Trading popup (client/src/ui/v10_market.c): list, buy, quick sell, offer, accept
    add_test(NAME brain_v10_market_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/client/tests/v10_market_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_v10_market_e2e PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 LABELS "brain;db;economy;e2e")
    # the Junkyard's craft screen from the Store's tile: recipes listed, craft through the confirmation (materials taken, the skin granted), the result popup
    add_test(NAME brain_junkyard_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/client/tests/junkyard_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_junkyard_e2e PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 LABELS "brain;db;craft;e2e")
    add_test(NAME brain_economy_web_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/economy_web_e2e.py ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_economy_web_e2e PROPERTIES TIMEOUT 480 SKIP_RETURN_CODE 77 LABELS "brain;db;economy;e2e;web")
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
        add_test(NAME brain_kpd_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/kpd_test.py
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
        add_test(NAME brain_economy_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/economy_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        add_test(NAME brain_junk_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/junk_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_junk_${drv} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
            ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;junk;${drv}")
        add_test(NAME brain_lottery_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/lottery_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_lottery_${drv} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
            ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;lottery;${drv}")
        add_test(NAME brain_shop_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/shop_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_shop_${drv} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
            ENVIRONMENT KN_TEST_DB_DRIVER=${drv} LABELS "knsvc;brain;db;shop;${drv}")
        add_test(NAME brain_strike_economy_${drv}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/strike_economy_test.py
                    $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
        set_tests_properties(brain_social_ws_${drv} brain_ranked_${drv} brain_turf_${drv} brain_storage_${drv} brain_kpd_${drv} brain_progress_${drv} brain_season_${drv}
            brain_account_security_${drv} brain_economy_${drv} brain_strike_economy_${drv}
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
