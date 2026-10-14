# kn_editor_mcp's tool core compiled into another target: client_bin's map editor AI panel runs
# the same tools in-process against the open map (client/src/editor/editor_ai_tools.c; the live
# mode's bus requests short-circuit to the editor through Live.loopback). Everything of src/ but the
# stdio / HTTP front ends (main.c, http.c), plus the embedded schema. Defines KN_AI_EDITOR_TOOLS=1.
#   include(tools/editor-mcp/core.cmake)
#   kn_editor_mcp_embed(<target>)
function(kn_editor_mcp_embed target)
    set(dir ${CMAKE_SOURCE_DIR}/tools/editor-mcp)
    file(GLOB srcs CONFIGURE_DEPENDS ${dir}/src/*.c)
    list(FILTER srcs EXCLUDE REGEX "/src/(main|http)\\.c$")
    set(embed ${CMAKE_CURRENT_BINARY_DIR}/${target}_editor_mcp_schema.c)
    add_custom_command(
        OUTPUT ${embed}
        COMMAND ${CMAKE_COMMAND} -DIN=${dir}/data/schema.json -DOUT=${embed} -P ${dir}/embed.cmake
        DEPENDS ${dir}/data/schema.json ${dir}/embed.cmake
        COMMENT "Embedding the editor tools' schema (${target})"
        VERBATIM)
    target_sources(${target} PRIVATE ${srcs} ${embed})
    # the tool sources find their own headers (util.h, schema.h, ...) without putting them on the
    # target's include path, where they could shadow the client's
    set_source_files_properties(${srcs} PROPERTIES
        INCLUDE_DIRECTORIES "${dir}/src;${CMAKE_SOURCE_DIR}/shared/include;${CMAKE_SOURCE_DIR}/client/src/editor")
    if (NOT MSVC)
        set_source_files_properties(${srcs} PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;$<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>")
    endif ()
    target_compile_definitions(${target} PRIVATE KN_AI_EDITOR_TOOLS=1)
endfunction()
