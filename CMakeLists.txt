cmake_minimum_required(VERSION 4.1)
project(KrunkNative)

set(CMAKE_CXX_STANDARD 20)

include_directories(shared/include)

file(GLOB_RECURSE SHARED_SOURCES shared/src/*.c)
file(GLOB_RECURSE SHARED_HEADERS shared/include/*.h)

file(GLOB_RECURSE CLIENT_SOURCES client/src/*.c)
file(GLOB_RECURSE CLIENT_HEADERS client/include/*.h)

add_subdirectory(client/lib/freetype)
add_subdirectory(client/lib/glad)
add_subdirectory(client/lib/glfw)

add_subdirectory(shared/lib/cJSON)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND CLIENT_SOURCES client/win.rc)
endif ()

link_libraries(cJSON)

add_executable(client ${CLIENT_HEADERS} ${CLIENT_SOURCES} ${SHARED_HEADERS} ${SHARED_SOURCES})
target_compile_definitions(client PRIVATE KRUNKNATIVE_CLIENT)
target_link_libraries(client PRIVATE glfw glad freetype)
target_include_directories(client PRIVATE client/include)
