add_executable(onion_unit_tests
    test_crypto.cpp
    test_layer.cpp
    test_framing.cpp
    test_socket.cpp
)

target_compile_options(onion_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(onion_unit_tests PRIVATE onion_core GTest::gtest_main)

add_test(NAME unit_tests COMMAND onion_unit_tests)

# The bash-driven multi-process tests are available on POSIX hosts. On Windows
# the C++ socket smoke test above covers the same networking code path.
if(NOT WIN32)
    add_test(NAME local_topology
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/local_topology_test.sh
                $<TARGET_FILE:onion>
                ${CMAKE_CURRENT_BINARY_DIR}/local_topology
    )
    set_tests_properties(local_topology PROPERTIES TIMEOUT 60)

    add_test(NAME tamper_rejection
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tamper_test.sh
                $<TARGET_FILE:onion>
                ${CMAKE_CURRENT_BINARY_DIR}/tamper_test
    )
    set_tests_properties(tamper_rejection PROPERTIES TIMEOUT 60)
endif()
