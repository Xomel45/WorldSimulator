cmake_minimum_required(VERSION 3.28)
project(WorldSimulator VERSION 0.1.0 LANGUAGES C CXX)  # C -- только для встроенной Lua

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
    add_compile_options(-g -Og)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
endif()

# ─── Dependencies ─────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_package(glfw3 3.4 REQUIRED)
if(WIN32)
    # mingw-пакет glm имеет баг в cmake-конфиге (_IMPORT_PREFIX="/usr" -> хостовые include),
    # а glm header-only и есть в mingw-сисруте: делаем чистый INTERFACE-таргет.
    add_library(glm::glm INTERFACE IMPORTED)
else()
    find_package(glm REQUIRED)
endif()
find_package(nlohmann_json 3 REQUIRED)     # манифесты модов (mod.json)

if(WIN32)
    # Arch-mingw-сисрут /usr/x86_64-w64-mingw32/include -- GCC-окружение (libstdc++ + C-хедеры).
    # Как -isystem (так делают импортированные таргеты) он перебивает <ctype.h>/<cmath> libc++
    # у clang и ломает сборку. Подключаем его с низшим приоритетом (-idirafter): хедеры
    # зависимостей находятся, стандартная библиотека берётся родная (llvm-mingw).
    add_compile_options(-idirafter /usr/x86_64-w64-mingw32/include)
    foreach(tgt Vulkan::Vulkan Vulkan::Headers glm::glm nlohmann_json::nlohmann_json)
        if(TARGET ${tgt})
            set_target_properties(${tgt} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
        endif()
    endforeach()
    if(TARGET glfw)
        set_target_properties(glfw PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
    endif()
    # Всё статически: у игрока не нужны рантаймы llvm-mingw (остаётся только системный vulkan-1.dll).
    add_link_options(-static)
endif()

# Lua (скрипты модов): системная 5.4+, а если нет (или WSIM_BUNDLED_LUA=ON, как в Windows-сборке) --
# исходники Lua собираются вместе с игрой (FetchContent), без внешних пакетов.
option(WSIM_BUNDLED_LUA "Собирать Lua из исходников вместо системной" OFF)
if(NOT WSIM_BUNDLED_LUA)
    find_package(Lua 5.4 QUIET)
endif()
if(WSIM_BUNDLED_LUA OR NOT LUA_FOUND)
    include(FetchContent)
    FetchContent_Declare(lua_src
        GIT_REPOSITORY https://github.com/lua/lua.git
        GIT_TAG        v5.4.7
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(lua_src)  # у репозитория Lua нет CMakeLists -- только скачивание
    file(GLOB WSIM_LUA_SOURCES "${lua_src_SOURCE_DIR}/*.c")
    list(FILTER WSIM_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
    add_library(wsim_lua STATIC ${WSIM_LUA_SOURCES})
    target_include_directories(wsim_lua PUBLIC "${lua_src_SOURCE_DIR}")
    target_compile_options(wsim_lua PRIVATE -w)  # чужой код, без наших предупреждений
    set(LUA_INCLUDE_DIR "${lua_src_SOURCE_DIR}")
    set(LUA_LIBRARIES wsim_lua)
    message(STATUS "Lua: встроенная из исходников (${lua_src_SOURCE_DIR})")
endif()

# miniaudio: звук (один заголовок, без зависимостей). Репозиторий нужен только ради miniaudio.h:
# SOURCE_SUBDIR=none отключает его собственный CMake.
include(FetchContent)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.22
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  none
)
FetchContent_MakeAvailable(miniaudio)
find_package(Threads REQUIRED)

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
