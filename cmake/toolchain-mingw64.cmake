# Кросс-компиляция под Windows x64 (PE) через llvm-mingw.
# Использование: cmake --preset release-windows

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-clang)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-clang++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Где искать Windows-зависимости: mingw-сисрут (Vulkan/GLFW) + рантайм llvm-mingw
set(CMAKE_FIND_ROOT_PATH
    /usr/${TOOLCHAIN_PREFIX}
    /opt/llvm-mingw/${TOOLCHAIN_PREFIX})

# Программы берём с хоста (glslc, objcopy), либы/хедеры/пакеты — только из сисрута
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
