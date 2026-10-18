add_library(engine_procgen STATIC
    HouseGen.cpp
    TreeGen.cpp
    AnimalGen.cpp
)

target_include_directories(engine_procgen PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)

# Vertex.hpp включает vulkan.h -- линкуем только заголовки Vulkan, без движка рендера.
target_link_libraries(engine_procgen PUBLIC glm::glm Vulkan::Headers)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
