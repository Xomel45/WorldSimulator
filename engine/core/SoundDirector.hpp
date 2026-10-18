#pragma once

#include "audio/AudioEngine.hpp"
#include "sim/Physics.hpp"
#include "sim/SoundSink.hpp"
#include "sim/Terrain.hpp"
#include "sim/World.hpp"
#include "ui/UiSound.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <unordered_map>

namespace wsim {

// Режиссёр звука: решает, что и когда играть по состоянию игры. AudioEngine умеет только
// "сыграть такой-то звук тут", а Director знает про мир: ветер крепчает с высотой камеры,
// вода шумит, когда её много в кадре, существа подают голос, валуны бухают.
class SoundDirector final : public SoundSink {
public:
    explicit SoundDirector(audio::AudioEngine& audio);

    // Раз в кадр. focus -- точка на земле под камерой (слушатель стоит на ней, а не в небе:
    // иначе с высоты всё было бы далеко и тихо), camDistance -- высота/дальность камеры, yaw -- куда смотрит камера.
    void update(float dt, bool playing, const glm::vec3& focus, const glm::vec3& forward, float camDistance, const World& world,
                const Terrain& terrain);

    void ui(UiSound sound);                                   // клик/наведение интерфейса (2D)
    void impact(const ImpactEvent& hit);                      // валун ударил о землю
    void effect(audio::SoundId id, const glm::vec3& pos, float volume = 1.0f, float pitch = 1.0f);  // сила бога в точке
    void terrainRumble(float intensity) { m_rumbleTarget = intensity; }  // гул, пока лепят рельеф (0..1)

    // SoundSink: звуки по имени из скриптов модов.
    bool playNamed(std::string_view name, const glm::vec3& pos, float volume) override;

private:
    void creatureVoices(float dt, const World& world);
    void ambient(float dt, bool playing, const glm::vec3& focus, float camDistance, const Terrain& terrain);

    struct VoiceState {
        float cooldown   = 0.0f;
        bool  alarmHigh  = false, matingHigh = false, domHigh = false;  // выход мозга выше порога (для фронта "перешёл порог")
    };

    audio::AudioEngine&                        m_audio;
    std::mt19937                               m_rng{std::random_device{}()};
    std::unordered_map<std::uint64_t, VoiceState> m_voices;
    glm::vec3                                  m_focus{0.0f};
    float                                      m_camDistance = 25.0f;
    float                                      m_voiceBudget = 2.0f;   // сколько голосов можно начать сейчас (копится со временем)
    float                                      m_pruneTimer  = 0.0f;
    float                                      m_birdTimer   = 4.0f;
    float                                      m_time        = 0.0f;
    float                                      m_rumbleTarget = 0.0f;
    bool                                       m_playing     = false;
};

}  // namespace wsim
