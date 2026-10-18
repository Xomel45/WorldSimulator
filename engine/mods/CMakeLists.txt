add_library(engine_mods STATIC
    LuaSandbox.cpp
    ModManager.cpp
)

target_include_directories(engine_mods PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)
target_include_directories(engine_mods PRIVATE ${LUA_INCLUDE_DIR})

target_link_libraries(engine_mods
    PUBLIC  engine_sim
    PRIVATE ${LUA_LIBRARIES} nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
