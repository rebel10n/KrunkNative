cmake_minimum_required(VERSION 3.10)
project(KrunkNative)

set(CMAKE_C_STANDARD 11)

include_directories(shared/include)

file(GLOB_RECURSE SHARED_SOURCES shared/src/*.c)
file(GLOB_RECURSE SHARED_HEADERS shared/include/*.h)

set(CLIENT_BINARY client_bin)
file(GLOB_RECURSE CLIENT_SOURCES client/src/*.c)
file(GLOB_RECURSE CLIENT_HEADERS client/include/*.h)

add_subdirectory(client/lib/freetype)
add_subdirectory(client/lib/glad)
add_subdirectory(client/lib/glfw)

add_subdirectory(shared/lib/cJSON)
add_subdirectory(shared/lib/pcg_basic)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CLIENT_BINARY client)
    list(APPEND CLIENT_SOURCES client/win.rc)
endif ()

link_libraries(cJSON pcg_basic)

add_executable(${CLIENT_BINARY} ${CLIENT_HEADERS} ${CLIENT_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES})
target_compile_definitions(${CLIENT_BINARY} PRIVATE KRUNKNATIVE_CLIENT)
target_link_libraries(${CLIENT_BINARY} PRIVATE glfw glad freetype)
target_include_directories(${CLIENT_BINARY} PRIVATE client/include)
