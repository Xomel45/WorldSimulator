#pragma once

#include "sim/Brain.hpp"
#include "sim/Genome.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

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
// lovedOne* — ближайший/самый актуальный из семьи (партнёр/родители/дети,
// см. Creature::bondedPartnerId/parentIds/childIds), а не общий сородич:
// отдельный от threatProximity сигнал, т.к. существо может убегать от своей
// опасности и одновременно знать, что близкий в беде где-то ещё.
// familiarCrowding/strangerCrowding — тот же принцип "свои vs чужие": мир
// делит соседей на "известных" (id входит в bondedPartnerId/parentIds/
// childIds) и незнакомцев.
struct SenseInputs {
    glm::vec2 foodDir{};   float foodProximity   = 0.0f;
    glm::vec2 threatDir{}; float threatProximity = 0.0f;
    glm::vec2 mateDir{};   float mateProximity   = 0.0f;
    float     familiarCrowding = 0.0f;
    float     strangerCrowding = 0.0f;
    float     nearbyAlarmLevel      = 0.0f;
    float     nearbyMatingCallLevel = 0.0f;
    glm::vec2 lovedOneDir{};
    float     lovedOneProximity   = 0.0f;
    float     lovedOneThreatLevel = 0.0f;
};

class Creature {
public:
    Genome    genome;
    Needs     needs;
    glm::vec3 position{};
    glm::vec3 velocity{};
    float     age   = 0.0f;
    bool      alive = true;

    // Идентичность/родство. 0 — единый сентинел "нет/не назначено" везде
    // ниже (без std::optional). id не назначает сам Creature — это забота
    // будущего мира (реестра популяции), тот же принцип, что и у
    // feed()/rest()/mate(): мир решает, Creature только хранит факт.
    std::uint64_t               id = 0;
    std::uint64_t               bondedPartnerId = 0;  // последний mate() перезаписывает — не история, моногамия "по последней связи"
    std::array<std::uint64_t,2> parentIds{0, 0};       // {0,0} = не рождён через reproduce()
    std::vector<std::uint64_t>  childIds;              // растёт через recordChild()

    // Пластичная часть мозга (hidden->output+bias) в течение жизни — своя
    // изменяемая копия, стартующая от genome.initialPlasticWeights(). Гены
    // (genome) НИКОГДА этим не трогаются: обучение не наследуется (Дарвин, не
    // Ламарк) — reproduce() кроссовит только genome. Лениво инициализируется
    // первым tick(), если пуст; resetLearning() — явный сброс "с нуля".
    std::vector<float> learnedWeights;

    // Черты тела считаются из генома по требованию (единственный источник
    // истины — геном; кэшировать эти значения имеет смысл только если
    // профилирование когда-нибудь покажет, что это узкое место).
    [[nodiscard]] float maxSpeed() const noexcept;    // [1, 6] м/с
    [[nodiscard]] float size() const noexcept;        // [0.7, 1.4] масштаб
    [[nodiscard]] float metabolism() const noexcept;  // [0.7, 1.4] множитель расхода
    [[nodiscard]] float lifespan() const noexcept;    // [300, 1200] с (плейсхолдер-юнит до системы времени)
    [[nodiscard]] float fertility() const noexcept;   // [0, 1]
    [[nodiscard]] float plasticity() const noexcept;  // [0, 0.05] скорость обучения в течение жизни

    // Явный темперамент — именованные, инспектируемые дилы "насколько", в
    // отличие от неявной индивидуальности, зашитой в веса мозга. Напрямую
    // (детерминированно) модулируют tick(), см. .cpp.
    [[nodiscard]] float boldness() const noexcept;     // [0, 1] храбрость
    [[nodiscard]] float sociability() const noexcept;  // [0, 1] интроверт..экстраверт
    [[nodiscard]] float bondStrength() const noexcept; // [0, 1] привязанность к близким

    // Копирует genome.initialPlasticWeights() поверх learnedWeights — "с
    // чистого листа". tick() вызывает это лениво сам, если learnedWeights пуст.
    void resetLearning() noexcept;

    // Один тик: считает мозг (с обучением — см. resetLearning()/plasticity()),
    // обновляет needs/position/age. Возвращает выход мозга как есть — удался
    // ли eat/flee/mate, решает мир (позже), вызывая feed()/rest() по факту.
    BrainOutputs tick(float dt, const SenseInputs& senses) noexcept;

    void feed(float amount) noexcept;                    // мир вызывает при успешном поедании
    void rest(float amount) noexcept;                     // мир вызывает при успешном отдыхе
    void mate(std::uint64_t partnerId) noexcept;           // мир вызывает при успешном спаривании: сбрасывает либидо, запоминает партнёра
    void recordChild(std::uint64_t childId);                // мир вызывает после назначения ребёнку id

    [[nodiscard]] static Creature reproduce(const Creature& a, const Creature& b, Rng& rng);
};

}  // namespace wsim
