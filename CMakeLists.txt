cmake_minimum_required(VERSION 3.28)
project(WorldSimulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

include(CTest)

# ─── Compiler flags ───────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-designated-field-initializers  # VK-структуры намеренно используют zero-init
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
endif()

# ─── Dependencies ─────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_package(glfw3 3.4 REQUIRED)
find_package(glm REQUIRED)

# ─── Shader compilation ───────────────────────────────────────────────────────
find_program(GLSLC_EXECUTABLE NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/bin" "/usr/bin" "/usr/local/bin"
    REQUIRED
)

# Usage: target_compile_shaders(target path/to/shader.vert path/to/shader.comp ...)
# Шейдеры могут делать #include из assets/shaders/include/ (GL_GOOGLE_include_directive).
function(target_compile_shaders TARGET)
    set(SPIRV_DIR "${CMAKE_BINARY_DIR}/shaders")
    set(SHADER_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/assets/shaders/include")
    file(GLOB SHADER_INCLUDES "${SHADER_INCLUDE_DIR}/*")
    set(SPIRV_OUTPUTS "")

    foreach(SHADER IN LISTS ARGN)
        cmake_path(GET SHADER FILENAME SHADER_FILENAME)
        set(SPIRV "${SPIRV_DIR}/${SHADER_FILENAME}.spv")

        add_custom_command(
            OUTPUT  "${SPIRV}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPIRV_DIR}"
            COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.3
                    -I "${SHADER_INCLUDE_DIR}" "${SHADER}" -o "${SPIRV}"
            DEPENDS "${SHADER}" ${SHADER_INCLUDES}
            COMMENT "Compiling shader: ${SHADER_FILENAME}"
            VERBATIM
        )
        list(APPEND SPIRV_OUTPUTS "${SPIRV}")
    endforeach()

    add_custom_target("${TARGET}_shaders" DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies("${TARGET}" "${TARGET}_shaders")
endfunction()

# ─── Dear ImGui ───────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.6
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(imgui)

# ─── Subdirectories ───────────────────────────────────────────────────────────
add_subdirectory(engine)
add_subdirectory(game)
