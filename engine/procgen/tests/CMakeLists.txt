add_executable(procgen_tests procgen_tests.cpp)
target_link_libraries(procgen_tests PRIVATE engine_procgen)
target_include_directories(procgen_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/sim/tests)
add_test(NAME procgen_tests COMMAND procgen_tests)
