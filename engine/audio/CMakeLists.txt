add_library(engine_audio STATIC
    Synth.cpp
    AudioEngine.cpp
    miniaudio_impl.cpp
)

target_include_directories(engine_audio PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)
# miniaudio.h -- как системный заголовок: чужой код без наших предупреждений.
target_include_directories(engine_audio SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})

# Без декодеров/энкодеров/генераторов/менеджера ресурсов: нужны только движок и свои PCM-источники.
# Эти макросы меняют структуры -- поэтому на всю библиотеку, а не на один файл.
target_compile_definitions(engine_audio PRIVATE
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER)
set_source_files_properties(miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(engine_audio
    PUBLIC  glm::glm
    PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
