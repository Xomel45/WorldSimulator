add_library(engine_renderer STATIC
    Window.cpp
    VulkanContext.cpp
    RtSystem.cpp
    PathTracer.cpp
    GpuBuffer.cpp
    Mesh.cpp
    ObjLoader.cpp
    FsrCon.cpp
)

target_include_directories(engine_renderer PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)

# Официальные хедеры FSR (ffx_a.h/ffx_fsr1.h) — общие для GLSL и C++ (A_CPU)
target_include_directories(engine_renderer PRIVATE
    ${CMAKE_SOURCE_DIR}/assets/shaders/include
)

target_link_libraries(engine_renderer PUBLIC
    Vulkan::Vulkan
    glfw
    glm::glm
)

target_compile_definitions(engine_renderer PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
