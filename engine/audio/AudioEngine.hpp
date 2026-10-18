#pragma once

#include "audio/Synth.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace wsim::audio {

// Группы громкости (ползунки в настройках; общая громкость -- у всего движка).
enum class Group { Ambient, Creatures, Effects };

// Звук на miniaudio: движок со слушателем, пул одиночных голосов, несколько петель.
// Все PCM рождаются в коде (Synth) на частоте самого устройства и синтезируются в фоновом
// потоке при старте -- игра не ждёт; пока набор не готов, play() молчит.
// Нет звукового устройства / WSIM_NOAUDIO=1 -- все методы тихо ничего не делают.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] bool ready() const noexcept;        // звуки синтезированы
    [[nodiscard]] const std::string& status() const noexcept;  // имя устройства или причина, почему звука нет

    // Слушатель: точка и горизонтальное направление взгляда (для панорамы лево/право).
    void setListener(const glm::vec3& pos, const glm::vec3& forward);
    void setVolumes(float master, float ambient, float creatures, float effects);  // 0..1

    // Одиночный звук. pos == nullptr -- 2D (интерфейс). Громкость с расстоянием падает линейно
    // от minDist до нуля на maxDist. pitch: 1 -- как есть, <1 ниже.
    void play(SoundId id, Group group, float volume = 1.0f, float pitch = 1.0f, const glm::vec3* pos = nullptr,
              float minDist = 8.0f, float maxDist = 160.0f);

    // Целевая громкость петли (0 -- тишина); к цели идёт плавно, чтобы не щёлкало.
    void setLoop(SoundId loop, float volume);

    void update(float dt);  // раз в кадр: убирает доигравшие голоса, двигает громкость петель

    // Отладка: записать весь набор звуков в WAV (WSIM_DUMP_SOUNDS=каталог), чтобы послушать.
    void dumpBank(const std::string& dir) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace wsim::audio
