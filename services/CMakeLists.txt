# KrunkNative backend services (doc/backend.md).
#
#   knsvc       static lib, services/common: config, log, loop, HTTP, WebSocket,
#               crypto (OpenSSL libcrypto), msgpack. No database code: game servers link it.
#   knsvc_db    static lib, services/common: database store layer + drivers
#               (postgres/sqlite/mysql, KNSVC_STORE_* options) + migrations.
#   kn_migrate  migrations tool (make db-migrate)
#   kn_brain    services/brain, the coordinator (when any store driver is built)
#
# Everything here is optional: without OpenSSL nothing is built, without any
# database client library only knsvc is, and the game build is never affected.

find_package(OpenSSL COMPONENTS Crypto)
if (NOT OpenSSL_FOUND)
    message(STATUS "services: OpenSSL (libcrypto) not found; backend services are not built")
    set(KNSVC_AVAILABLE OFF PARENT_SCOPE)
    return()
endif ()

# ---- database client libraries: one store driver each (knsvc/store.h). Each
# KNSVC_STORE_<NAME> option defaults to ON when its library is found.
find_package(PkgConfig QUIET)

set(KN_LIBPQ_TARGET PostgreSQL::PostgreSQL)
find_package(PostgreSQL QUIET)
if (NOT PostgreSQL_FOUND AND PkgConfig_FOUND)
    pkg_check_modules(KN_LIBPQ QUIET IMPORTED_TARGET libpq)
    if (KN_LIBPQ_FOUND)
        add_library(kn_libpq INTERFACE)
        target_link_libraries(kn_libpq INTERFACE PkgConfig::KN_LIBPQ)
        set(KN_LIBPQ_TARGET kn_libpq)
        set(PostgreSQL_FOUND TRUE)
    endif ()
endif ()

find_package(SQLite3 QUIET)
if (TARGET SQLite3::SQLite3)       # CMake >= 4.3
    set(KN_SQLITE_TARGET SQLite3::SQLite3)
else ()
    set(KN_SQLITE_TARGET SQLite::SQLite3)
endif ()
if (NOT SQLite3_FOUND AND PkgConfig_FOUND)
    pkg_check_modules(KN_SQLITE QUIET IMPORTED_TARGET sqlite3)
    if (KN_SQLITE_FOUND)
        set(KN_SQLITE_TARGET PkgConfig::KN_SQLITE)
        set(SQLite3_FOUND TRUE)
    endif ()
endif ()

# MariaDB Connector/C (libmariadb), else MySQL's libmysqlclient (same API).
set(KN_MYSQL_FOUND FALSE)
if (PkgConfig_FOUND)
    pkg_check_modules(KN_MARIADB QUIET IMPORTED_TARGET libmariadb)
    if (KN_MARIADB_FOUND)
        set(KN_MYSQL_TARGET PkgConfig::KN_MARIADB)
        set(KN_MYSQL_FOUND TRUE)
    else ()
        pkg_check_modules(KN_MYSQLCLIENT QUIET IMPORTED_TARGET mysqlclient)
        if (KN_MYSQLCLIENT_FOUND)
            set(KN_MYSQL_TARGET PkgConfig::KN_MYSQLCLIENT)
            set(KN_MYSQL_FOUND TRUE)
        endif ()
    endif ()
endif ()

option(KNSVC_STORE_POSTGRES "Build the PostgreSQL store driver (libpq)" ${PostgreSQL_FOUND})
option(KNSVC_STORE_SQLITE "Build the SQLite store driver (libsqlite3)" ${SQLite3_FOUND})
option(KNSVC_STORE_MYSQL "Build the MySQL/MariaDB store driver (libmariadb / libmysqlclient)" ${KN_MYSQL_FOUND})
foreach (_d IN ITEMS POSTGRES:PostgreSQL_FOUND SQLITE:SQLite3_FOUND MYSQL:KN_MYSQL_FOUND)
    string(REPLACE ":" ";" _p ${_d})
    list(GET _p 0 _name)
    list(GET _p 1 _found)
    if (KNSVC_STORE_${_name} AND NOT ${_found})
        message(FATAL_ERROR "services: KNSVC_STORE_${_name}=ON but its client library was not found")
    endif ()
endforeach ()

# Optional Redis / Valkey client (knsvc_redis, knsvc/redis.h): the brain's hot tier
# (doc/backend.md 3.11). hiredis >= 0.14; only services link it, never the game targets.
set(KN_HIREDIS_FOUND FALSE)
if (PkgConfig_FOUND)
    pkg_check_modules(KN_HIREDIS QUIET IMPORTED_TARGET hiredis)
endif ()
option(KNSVC_REDIS "Build the optional Redis/Valkey hot tier client (hiredis)" ${KN_HIREDIS_FOUND})
if (KNSVC_REDIS AND NOT KN_HIREDIS_FOUND)
    message(FATAL_ERROR "services: KNSVC_REDIS=ON but hiredis (pkg-config hiredis) was not found")
endif ()
message(STATUS "services: redis hot tier (hiredis ${KN_HIREDIS_VERSION}) = ${KNSVC_REDIS}")

set(KNSVC_AVAILABLE ON PARENT_SCOPE)
if (KNSVC_STORE_POSTGRES OR KNSVC_STORE_SQLITE OR KNSVC_STORE_MYSQL)
    set(KNSVC_DB_AVAILABLE ON PARENT_SCOPE)
    set(KNSVC_DB_AVAILABLE ON)
    message(STATUS "services: store drivers postgres=${KNSVC_STORE_POSTGRES} sqlite=${KNSVC_STORE_SQLITE} "
                   "mysql=${KNSVC_STORE_MYSQL}; building knsvc, knsvc_db and the coordinator")
else ()
    set(KNSVC_DB_AVAILABLE OFF PARENT_SCOPE)
    set(KNSVC_DB_AVAILABLE OFF)
    message(STATUS "services: no database client library (libpq, libsqlite3, libmariadb); building knsvc only")
endif ()

add_subdirectory(common)

if (KNSVC_DB_AVAILABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/brain/CMakeLists.txt)
    add_subdirectory(brain)
endif ()

# `make services`: every backend library, tool, test and the brain.
add_custom_target(services_all)
foreach (t IN ITEMS knsvc knsvc_db knsvc_redis kn_migrate kn_dbcopy kn_brain kn_loadgen)
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
