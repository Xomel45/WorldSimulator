add_executable(worldsim
    main.cpp
)

target_include_directories(worldsim PRIVATE
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_SOURCE_DIR}/game
)

target_link_libraries(worldsim PRIVATE
    engine_core
)

# Передаём пути к ресурсам в рантайм
target_compile_definitions(worldsim PRIVATE
    WSIM_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/"
    WSIM_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/"
)

# Шейдеры
target_compile_shaders(worldsim
    "${CMAKE_SOURCE_DIR}/assets/shaders/triangle.vert"
    "${CMAKE_SOURCE_DIR}/assets/shaders/triangle.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/scene.vert"
    "${CMAKE_SOURCE_DIR}/assets/shaders/scene.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/scene_rt.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/pathtrace.rgen"
    "${CMAKE_SOURCE_DIR}/assets/shaders/pathtrace.rmiss"
    "${CMAKE_SOURCE_DIR}/assets/shaders/pathtrace.rchit"
    "${CMAKE_SOURCE_DIR}/assets/shaders/geometry_deform.comp"
    "${CMAKE_SOURCE_DIR}/assets/shaders/fullscreen.vert"
    "${CMAKE_SOURCE_DIR}/assets/shaders/blur.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/composite.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/vision.frag"
    "${CMAKE_SOURCE_DIR}/assets/shaders/fsr_easu.comp"
    "${CMAKE_SOURCE_DIR}/assets/shaders/fsr_rcas.comp"
    "${CMAKE_SOURCE_DIR}/assets/shaders/nis_upscale.comp"
)
