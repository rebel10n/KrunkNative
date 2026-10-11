# KrunkNative backend services (doc/backend.md).
#
#   knsvc       static lib, services/common: config, log, loop, HTTP, WebSocket,
#               crypto (OpenSSL libcrypto), msgpack. No libpq: game servers link it.
#   knsvc_db    static lib, services/common: PostgreSQL pool + migrations (libpq).
#   kn_migrate  migrations tool (make db-migrate)
#   kn_brain    services/brain (when present and libpq is available)
#
# Everything here is optional: without OpenSSL nothing is built, without libpq
# only knsvc is, and the game build is never affected.

find_package(OpenSSL COMPONENTS Crypto)
if (NOT OpenSSL_FOUND)
    message(STATUS "services: OpenSSL (libcrypto) not found; backend services are not built")
    set(KNSVC_AVAILABLE OFF PARENT_SCOPE)
    return()
endif ()

set(KN_LIBPQ_TARGET PostgreSQL::PostgreSQL)
find_package(PostgreSQL QUIET)
if (NOT PostgreSQL_FOUND)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(KN_LIBPQ QUIET IMPORTED_TARGET libpq)
        if (KN_LIBPQ_FOUND)
            add_library(kn_libpq INTERFACE)
            target_link_libraries(kn_libpq INTERFACE PkgConfig::KN_LIBPQ)
            set(KN_LIBPQ_TARGET kn_libpq)
            set(PostgreSQL_FOUND TRUE)
        endif ()
    endif ()
endif ()

set(KNSVC_AVAILABLE ON PARENT_SCOPE)
if (PostgreSQL_FOUND)
    set(KNSVC_DB_AVAILABLE ON PARENT_SCOPE)
    set(KNSVC_DB_AVAILABLE ON)
    message(STATUS "services: libpq found; building knsvc, knsvc_db and the brain")
else ()
    set(KNSVC_DB_AVAILABLE OFF PARENT_SCOPE)
    set(KNSVC_DB_AVAILABLE OFF)
    message(STATUS "services: libpq not found; building knsvc only (no knsvc_db / brain)")
endif ()

add_subdirectory(common)

if (KNSVC_DB_AVAILABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/brain/CMakeLists.txt)
    add_subdirectory(brain)
endif ()

# `make services`: every backend library, tool, test and the brain.
add_custom_target(services_all)
foreach (t IN ITEMS knsvc knsvc_db kn_migrate kn_brain)
    if (TARGET ${t})
        add_dependencies(services_all ${t})
    endif ()
endforeach ()
get_property(_kn_tests DIRECTORY common PROPERTY BUILDSYSTEM_TARGETS)
foreach (t IN LISTS _kn_tests)
    if (t MATCHES "^knsvc_test_")
        add_dependencies(services_all ${t})
    endif ()
endforeach ()
