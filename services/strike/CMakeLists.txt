# services/strike: Krunker Strike's own backend (doc/v10/strike-director.md).
#
#   kn_strike_director  the matchmaker "director" (FRVR's shape: GET /live, POST
#                       /v1/director/allocate[/private]); knsvc only, no database, no kn_brain.
#                       Reuses the brain finder's server table (finder_table.c) and the
#                       heartbeat / KNINFO contract; director_ticket.c is also linked by server_bin.

add_executable(kn_strike_director
    director.c
    director_ticket.c
    ${CMAKE_SOURCE_DIR}/services/brain/finder_table.c)
target_include_directories(kn_strike_director PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/services/brain ${CMAKE_SOURCE_DIR}/shared/include)
target_link_libraries(kn_strike_director PRIVATE knsvc kn_build_info)
target_compile_options(kn_strike_director PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(kn_strike_director PRIVATE _GNU_SOURCE)

# director tickets (pure, no sockets)
add_executable(kn_strike_director_ticket_test tests/test_director_ticket.c director_ticket.c)
target_include_directories(kn_strike_director_ticket_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kn_strike_director_ticket_test PRIVATE knsvc)
add_test(NAME strike_director_ticket COMMAND kn_strike_director_ticket_test)
set_tests_properties(strike_director_ticket PROPERTIES LABELS "knsvc;strike")

# 2 Strike servers + 1 base server_bin against the director (ports from kn_proto.free_ports()).
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_FOUND)
    add_test(NAME strike_director_e2e
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/director_e2e.py ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(strike_director_e2e PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "strike;e2e")
endif ()
