add_library(engine_ui STATIC
    ImGuiLayer.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(engine_ui PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(engine_ui PUBLIC engine_renderer)

target_compile_definitions(engine_ui PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
