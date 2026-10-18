add_executable(synth_tests synth_tests.cpp)
target_link_libraries(synth_tests PRIVATE engine_audio)
target_include_directories(synth_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/sim/tests)
add_test(NAME synth_tests COMMAND synth_tests)
