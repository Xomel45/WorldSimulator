function(wsim_sim_test NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE engine_sim)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

wsim_sim_test(genome_tests)
wsim_sim_test(brain_tests)
wsim_sim_test(creature_tests)
