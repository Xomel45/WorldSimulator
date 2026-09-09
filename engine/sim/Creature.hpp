#pragma once

#include "sim/Brain.hpp"
#include "sim/Genome.hpp"

#include <glm/glm.hpp>

namespace wsim {

struct Needs {
    float hunger   = 0.2f;
    float energy   = 1.0f;
    float fear     = 0.0f;
    float libido   = 0.0f;
    float social   = 0.5f;
};

// Локальные ощущения — считает будущая мировая/пространственная система,
// Creature сам ничего не ищет (нет зависимости от мира на этом этапе).
// nearbyAlarmLevel/nearbyMatingCallLevel — агрегированные (например, max по
// соседям) сигналы BrainOutputs::alarmCall/matingCall других существ рядом:
// это и есть коммуникация, а не просто приватная метрика "social".
struct SenseInputs {
    glm::vec2 foodDir{};   float foodProximity   = 0.0f;
    glm::vec2 threatDir{}; float threatProximity = 0.0f;
    glm::vec2 mateDir{};   float mateProximity   = 0.0f;
    float     crowding    = 0.0f;
    float     nearbyAlarmLevel      = 0.0f;
    float     nearbyMatingCallLevel = 0.0f;
};

class Creature {
public:
    Genome    genome;
    Needs     needs;
    glm::vec3 position{};
    glm::vec3 velocity{};
    float     age   = 0.0f;
    bool      alive = true;

    // Черты тела считаются из генома по требованию (единственный источник
    // истины — геном; кэшировать эти значения имеет смысл только если
    // профилирование когда-нибудь покажет, что это узкое место).
    [[nodiscard]] float maxSpeed() const noexcept;    // [1, 6] м/с
    [[nodiscard]] float size() const noexcept;        // [0.7, 1.4] масштаб
    [[nodiscard]] float metabolism() const noexcept;  // [0.7, 1.4] множитель расхода
    [[nodiscard]] float lifespan() const noexcept;    // [300, 1200] с (плейсхолдер-юнит до системы времени)
    [[nodiscard]] float fertility() const noexcept;   // [0, 1]

    // Один тик: считает мозг, обновляет needs/position/age. Возвращает выход
    // мозга как есть — удался ли eat/flee/mate, решает мир (позже), вызывая
    // feed()/rest() по факту.
    BrainOutputs tick(float dt, const SenseInputs& senses) noexcept;

    void feed(float amount) noexcept;  // мир вызывает при успешном поедании
    void rest(float amount) noexcept;  // мир вызывает при успешном отдыхе
    void mate() noexcept;              // мир вызывает при успешном спаривании (сбрасывает либидо)

    [[nodiscard]] static Creature reproduce(const Creature& a, const Creature& b, Rng& rng);
};

}  // namespace wsim
