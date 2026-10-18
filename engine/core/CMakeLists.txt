add_library(engine_core STATIC
    Application.cpp
    InputSystem.cpp
    TerrainChunks.cpp
    SoundDirector.cpp
)

target_include_directories(engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)

# engine_core depends on renderer, ui и sim (World теперь живёт в Application);
# Logger.hpp header-only — no circular dep
target_link_libraries(engine_core PUBLIC
    engine_renderer
    engine_ui
    engine_sim
    engine_mods
    engine_procgen
    engine_audio
)
