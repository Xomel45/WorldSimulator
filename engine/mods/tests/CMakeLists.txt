add_executable(mods_tests mods_tests.cpp)
target_link_libraries(mods_tests PRIVATE engine_mods)
target_include_directories(mods_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/sim/tests)
add_test(NAME mods_tests COMMAND mods_tests)
