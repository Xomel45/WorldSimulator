#pragma once

#include <glm/glm.hpp>

#include <string_view>

namespace wsim {

// Куда сила бога (в том числе скрипт мода) может отправить звук. Интерфейс живёт в sim,
// чтобы sim ничего не знал про аудио: реализует его слой звука игры (SoundDirector).
class SoundSink {
public:
    virtual ~SoundSink() = default;
    // name -- имя из ограниченного набора ("thud", "pop", "whoosh", "zap", "chime", "knock").
    // false -- неизвестное имя.
    virtual bool playNamed(std::string_view name, const glm::vec3& pos, float volume) = 0;
};

}  // namespace wsim
