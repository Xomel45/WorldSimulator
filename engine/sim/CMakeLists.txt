add_library(engine_sim STATIC
    Genome.cpp
    Brain.cpp
    Creature.cpp
    World.cpp
    Terrain.cpp
    Powers.cpp
    Physics.cpp
    Behavior.cpp
    Trees.cpp
    SaveGame.cpp
)

target_include_directories(engine_sim PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_sim PUBLIC
    glm::glm
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
