cmake_minimum_required(VERSION 3.10)
project(KrunkNative)

set(CMAKE_C_STANDARD 11)

include_directories(shared/include)

file(GLOB_RECURSE SHARED_SOURCES shared/src/*.c)
file(GLOB_RECURSE SHARED_HEADERS shared/include/*.h)

set(CLIENT_BINARY client_bin)
file(GLOB_RECURSE CLIENT_SOURCES client/src/*.c)
file(GLOB_RECURSE CLIENT_HEADERS client/include/*.h)

set(SERVER_BINARY server_bin)
file(GLOB_RECURSE SERVER_SOURCES server/src/*.c)
file(GLOB_RECURSE SERVER_HEADERS server/include/*.h)

# LIBRARIES
add_subdirectory(client/lib/freetype)
add_subdirectory(client/lib/glad)
add_subdirectory(client/lib/glfw)

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

# CLIENT
add_executable(${CLIENT_BINARY} ${CLIENT_HEADERS} ${CLIENT_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES})
target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_CLIENT)
target_link_libraries(${CLIENT_BINARY} PRIVATE glfw glad freetype)
target_include_directories(${CLIENT_BINARY} PRIVATE client/include)

# SERVER
add_executable(${SERVER_BINARY} ${SERVER_HEADERS} ${SERVER_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES})
target_compile_definitions(${SERVER_BINARY} PRIVATE KRUNKNATIVE_SERVER)
target_link_libraries(${SERVER_BINARY} PRIVATE replxx)
target_include_directories(${SERVER_BINARY} PRIVATE server/include)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(${SERVER_BINARY} PRIVATE ws2_32)
endif ()

# TESTS
add_executable(proc_input_test ${SHARED_HEADERS} ${SHARED_SOURCES} shared/tests/proc_input_test.c)
target_link_libraries(proc_input_test PRIVATE m)