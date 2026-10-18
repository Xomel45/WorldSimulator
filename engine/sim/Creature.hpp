#pragma once

#include "sim/Brain.hpp"
#include "sim/Genome.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace wsim {

// Дистанции мира, нужные и существу (поведение), и миру (резолв действий).
inline constexpr float kSenseRadius    = 25.0f;  // дальность "зрения": еда, угрозы, партнёры, толпа
inline constexpr float kInteractRadius = 2.0f;   // на таком расстоянии можно есть и спариваться
inline constexpr float kNoTarget       = 1.0e9f; // "цели нет" в полях расстояния

// Чем занято существо. Выбирается слоем поведения (Behavior.cpp) из потребностей, ощущений, черт
// и выходов мозга; видно в инспекторе и нужно для озвучки/отладки.
enum class Behavior : std::uint8_t { Wander, SeekFood, Eat, Rest, Flee, SeekMate, Socialize, GoHome, Count };
[[nodiscard]] const char* behaviorName(Behavior b) noexcept;  // по-русски

struct Needs {
    float hunger   = 0.2f;
    float energy   = 1.0f;
    float fear     = 0.0f;
    float libido   = 0.0f;
    float social   = 0.5f;
    float respect  = 0.3f;  // желание социального статуса, см. Creature::earnRespect()
    float resentment = 0.0f;  // обида/зависть к чужому статусу, см. Genome::kEnvy
    float grief = 0.0f;  // горе по утрате близкого, см. SenseInputs::lovedOneDied
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
// childIds) и незнакомцев. nearbyDominanceLevel — агрегированный
// BrainOutputs::dominanceDisplay соседей, тот же принцип, что и у
// nearbyFamiliarAlarmLevel/nearbyMatingCallLevel: демонстрация статуса рядом
// реально пугает (см. fearTarget в Creature::tick()), а не просто хранится в
// needs. nearbyAlarmLevel разделён на familiar/stranger (та же логика, что и
// у crowding) — иначе "сочувствие к чужакам" (Genome::kEmpathy) нечему было
// бы модулировать отдельно от паники своих. lovedOneDied — одноразовый
// сигнал: мир выставляет ненулевым в тик(и), когда хочет сообщить о смерти
// близкого (см. Needs::grief в Creature::tick()).
class Terrain;

// Рельеф даёт существу "физику": на подъёме медленнее и тяжелее, слишком
// крутое не взять, в воду с берега не зайти (плавать не умеет). nullptr --
// плоский мир без ограничений (юнит-тесты, декоративный мир в меню).
struct SenseInputs {
    const Terrain* terrain = nullptr;
    glm::vec2 foodDir{};   float foodProximity   = 0.0f;
    glm::vec2 threatDir{}; float threatProximity = 0.0f;
    glm::vec2 mateDir{};   float mateProximity   = 0.0f;
    float     familiarCrowding = 0.0f;
    float     strangerCrowding = 0.0f;
    float     nearbyFamiliarAlarmLevel = 0.0f;
    float     nearbyStrangerAlarmLevel = 0.0f;
    float     nearbyMatingCallLevel    = 0.0f;
    float     nearbyDominanceLevel     = 0.0f;
    glm::vec2 lovedOneDir{};
    float     lovedOneProximity   = 0.0f;
    float     lovedOneThreatLevel = 0.0f;
    float     lovedOneDied        = 0.0f;

    // Подробности для слоя поведения (мозг получает только сжатые foodProximity и т.п.):
    float     foodDist  = kNoTarget;  glm::vec3 foodPos{};   // ближайшая еда: расстояние и точка
    float     mateDist  = kNoTarget;                         // ближайший ГОТОВЫЙ партнёр (canMate)
    float     threatDist = kNoTarget;                        // ближайшая опасность (валун и т.п.)
    glm::vec2 alarmDir{};   float alarmNear = 0.0f;          // к ближайшему паникующему соседу; насколько он паникует
    glm::vec2 groupDir{};   int   groupCount = 0;  float groupDist = kNoTarget;  // к центру ближайшей группы, сколько в ней, как далеко центр
    glm::vec2 separation{};                                  // от тех, кто слишком близко (чтобы не сбиваться в кучу)
    float     worldHalfSize = 0.0f;                          // > 0 -- границы игровой области (мягко разворачиваемся)
};

class Creature {
public:
    Genome    genome;
    Needs     needs;
    glm::vec3 position{};
    glm::vec3 velocity{};
    float     age   = 0.0f;
    bool      alive = true;

    // "Дом" — лениво фиксируется на первом tick() (см. .cpp), место
    // рождения/первого пробуждения, а не что-то, что назначает мир (тот же
    // принцип ленивой инициализации, что и у learnedWeights). До первого
    // tick() равен {0,0,0} — см. awayFromHome().
    glm::vec3 homePosition{};

    // Идентичность/родство. 0 — единый сентинел "нет/не назначено" везде
    // ниже (без std::optional). id не назначает сам Creature — это забота
    // будущего мира (реестра популяции), тот же принцип, что и у
    // feed()/rest()/mate(): мир решает, Creature только хранит факт.
    // Здоровье и размножение: голод и истощение убивают, спаривание не чаще раза в matingCooldown секунд,
    // потомство требует зрелости (isMature) и сил (canMate) -- иначе популяция взрывается за секунды.
    float     health = 1.0f;
    float     matingCooldown = 0.0f;
    std::uint8_t deathCause = 0;       // 0 жив, 1 старость, 2 голод, 3 истощение (для статистики мира)

    // Поведение: текущая линия, сколько она держится (гистерезис против дёрганья), направление блуждания
    // и память о последнем месте с едой (место, где еду видели, -- туда идём, когда еды в поле зрения нет).
    Behavior  behavior = Behavior::Wander;
    float     behaviorTimer = 0.0f;
    float     wanderHeading = 0.0f;
    glm::vec3 foodMemory{};
    float     foodMemoryAge = -1.0f;   // < 0 -- памяти нет

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
    [[nodiscard]] float envy() const noexcept;         // [0, 1] зависть/ненависть к чужому статусу
    [[nodiscard]] float territorial() const noexcept;  // [0, 1] привязанность к месту
    [[nodiscard]] float curiosity() const noexcept;    // [0, 1] любопытство
    [[nodiscard]] float empathy() const noexcept;      // [0, 1] сочувствие к чужакам

    // Самостоятельно вычисляемые (не хранимые) сигналы — чистые функции
    // текущего состояния, вызываются и из tick() (для BrainInputs), и
    // отдельно (инспектируемы/тестируемы независимо от tick()).
    [[nodiscard]] float awayFromHome() const noexcept;  // [0,1): насыщающееся расстояние от homePosition
    [[nodiscard]] float boredomLevel() const noexcept;  // [0,1]: фоновое желание исследовать

    // Зрелость (дети не размножаются) и готовность к спариванию: сыт, не вымотан, здоров, либидо и перезарядка.
    [[nodiscard]] bool isMature() const noexcept;
    [[nodiscard]] bool canMate() const noexcept;

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
    void earnRespect(float amount) noexcept;                 // мир вызывает, когда демонстрация статуса реально выигрывает

    [[nodiscard]] static Creature reproduce(const Creature& a, const Creature& b, Rng& rng);
};

}  // namespace wsim
