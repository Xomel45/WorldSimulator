add_library(engine_core STATIC
    Application.cpp
    InputSystem.cpp
)

target_include_directories(engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)

# engine_core depends on renderer and ui; Logger.hpp header-only — no circular dep
target_link_libraries(engine_core PUBLIC
    engine_renderer
    engine_ui
)
