# services/common: knsvc (libpq-free) + knsvc_db (libpq). See services/CMakeLists.txt.

set(KNSVC_CORE_SOURCES
    src/buf.c src/log.c src/json.c src/config.c src/loop.c
    src/http.c src/http_parse.c src/url.c
    src/ws.c src/ws_frame.c src/crypto.c src/msgpack.c)

# Only files that exist are compiled, so a module or test that is being added
# never breaks configuring the whole repository.
function(knsvc_existing out)
    set(list "")
    foreach (f IN LISTS ARGN)
        if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
            list(APPEND list ${f})
        else ()
            message(STATUS "services/common: ${f} missing, skipped")
        endif ()
    endforeach ()
    set(${out} ${list} PARENT_SCOPE)
endfunction ()

knsvc_existing(KNSVC_CORE_SOURCES ${KNSVC_CORE_SOURCES})
add_library(knsvc STATIC ${KNSVC_CORE_SOURCES})
target_include_directories(knsvc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(knsvc PUBLIC cJSON OpenSSL::Crypto m PRIVATE Threads::Threads)
target_compile_options(knsvc PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(knsvc PRIVATE _GNU_SOURCE)
# Optional crypt(3) (yescrypt/sha512crypt) verification in kn_password_verify.
find_library(KN_LIBCRYPT crypt)
find_path(KN_CRYPT_H crypt.h)
if (KN_LIBCRYPT AND KN_CRYPT_H)
    target_link_libraries(knsvc PRIVATE ${KN_LIBCRYPT})
    target_compile_definitions(knsvc PRIVATE KNSVC_HAVE_LIBCRYPT=1)
endif ()

if (KNSVC_DB_AVAILABLE)
    knsvc_existing(KNSVC_DB_SOURCES src/db.c src/db_result.c src/dbconn.c src/migrate.c)
    add_library(knsvc_db STATIC ${KNSVC_DB_SOURCES})
    target_link_libraries(knsvc_db PUBLIC knsvc ${KN_LIBPQ_TARGET})
    target_compile_options(knsvc_db PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(knsvc_db PRIVATE _GNU_SOURCE)

    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/kn_migrate.c)
        add_executable(kn_migrate tools/kn_migrate.c)
        target_link_libraries(kn_migrate PRIVATE knsvc_db)
    endif ()
endif ()

# ---- tests (ctest; DB tests skip with code 77 when PostgreSQL is unreachable)
set(KNSVC_TESTS buf config crypto msgpack loop http ws)
foreach (t IN LISTS KNSVC_TESTS)
    if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${t}.c)
        continue()
    endif ()
    add_executable(knsvc_test_${t} tests/test_${t}.c)
    target_link_libraries(knsvc_test_${t} PRIVATE knsvc Threads::Threads)
    target_compile_definitions(knsvc_test_${t} PRIVATE _GNU_SOURCE
        KN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" KN_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME knsvc_${t} COMMAND knsvc_test_${t})
    set_tests_properties(knsvc_${t} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS knsvc)
endforeach ()

if (KNSVC_DB_AVAILABLE)
    foreach (t IN ITEMS db migrate)
        if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${t}.c)
            continue()
        endif ()
        add_executable(knsvc_test_${t} tests/test_${t}.c)
        target_link_libraries(knsvc_test_${t} PRIVATE knsvc_db Threads::Threads)
        target_compile_definitions(knsvc_test_${t} PRIVATE _GNU_SOURCE
            KN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" KN_REPO_ROOT="${CMAKE_SOURCE_DIR}")
        add_test(NAME knsvc_${t} COMMAND knsvc_test_${t})
        set_tests_properties(knsvc_${t} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "knsvc;db")
    endforeach ()
endif ()

# WebSocket interop against python3 `websockets` (skips when the module is missing)
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_FOUND AND TARGET knsvc_test_ws AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ws_interop.py)
    add_test(NAME knsvc_ws_interop
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/ws_interop.py $<TARGET_FILE:knsvc_test_ws>)
    set_tests_properties(knsvc_ws_interop PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS knsvc)
endif ()
