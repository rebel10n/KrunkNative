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
    ../../shared/src/map_catalog.c  # relative: knsvc_existing() checks paths under this dir
)

# Sources another agent has listed but not landed yet must not break configure
# (knsvc_existing: services/common/CMakeLists.txt).
knsvc_existing(KN_BRAIN_SOURCES ${KN_BRAIN_SOURCES})
add_executable(kn_brain ${KN_BRAIN_SOURCES})
target_include_directories(kn_brain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# svc-accounts: shared/include for map_catalog.h (creations.c; shim in shared_shim.c)
target_include_directories(kn_brain PRIVATE ${CMAKE_SOURCE_DIR}/shared/include)
target_link_libraries(kn_brain PRIVATE knsvc_db)
target_compile_options(kn_brain PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(kn_brain PRIVATE _GNU_SOURCE)

# Registry unit test (pure, no DB).
add_executable(kn_brain_registry_test tests/test_registry.c registry.c)
target_include_directories(kn_brain_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kn_brain_registry_test PRIVATE knsvc)
add_test(NAME brain_registry COMMAND kn_brain_registry_test)

# coord-auth: auth join records / limiters and the finder table (pure, no DB).
add_executable(kn_brain_auth_test tests/test_auth_finder.c auth_state.c finder_table.c)
target_include_directories(kn_brain_auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/shared/include)
target_sources(kn_brain_auth_test PRIVATE ${CMAKE_SOURCE_DIR}/shared/src/auth_join.c)
target_link_libraries(kn_brain_auth_test PRIVATE knsvc)
add_test(NAME brain_auth_finder COMMAND kn_brain_auth_test)
set_tests_properties(brain_auth_finder PROPERTIES LABELS "knsvc;brain")
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

# Ranked queue end to end (2 brains, fake game server, 4 accounts; per-run schema too).
if (Python3_FOUND)
    add_test(NAME brain_ranked
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ranked_test.py
                $<TARGET_FILE:kn_brain> ${CMAKE_SOURCE_DIR})
    set_tests_properties(brain_ranked PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "knsvc;brain;db")
endif ()

# Fake game server node for integration tests (speaks the internal WS protocol).
add_executable(kn_fakenode tools/kn_fakenode.c)
target_link_libraries(kn_fakenode PRIVATE knsvc)
target_compile_definitions(kn_fakenode PRIVATE _GNU_SOURCE)

# Build the test + tool with the brain (services_all picks up kn_brain).
add_dependencies(kn_brain kn_brain_registry_test kn_fakenode)
