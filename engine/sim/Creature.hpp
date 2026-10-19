#pragma once

#include "sim/Berries.hpp"
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
enum class Behavior : std::uint8_t { Wander, SeekFood, Eat, Rest, Flee, SeekMate, Socialize, GoHome, Sick, Sabotage, Chop, Craft, Build, Drink, Haul, Hunt, Sleep, Raid, Defend, Follow, March, Count };

// Умения (маска Creature::skills). Рубить деревья умеют все (врождённо); изготовление орудий -- знание: открывается в опасности или
// от случая и передаётся соседям и потомству по наблюдению (World::tick), как вкус ягод -- но у знания нет "забывания".
enum Skill : std::uint16_t { kSkillSpear = 1, kSkillShield = 2, kSkillBridge = 4, kSkillBarricade = 8, kSkillTrap = 16, kSkillBucket = 32, kSkillStockpile = 64, kSkillHut = 128, kSkillFire = 256 };
inline constexpr std::uint16_t kAllSkills = 511;
inline constexpr float kFireWood = 2.0f;   // брёвен на костёр

// Роль в общине -- склонность из генов (наследуется): охотники смелее и крупнее, строители любопытны и общительны, остальные рубят.
// Роль меняет приоритеты (охотник охотится раньше, дровосек рубит охотнее, строитель строит и учится строить быстрее), но не запрещает ничего.
enum class Role : std::uint8_t { Woodcutter, Hunter, Builder };
[[nodiscard]] const char* roleName(Role r) noexcept;
inline constexpr float kHutWood = 4.0f;   // брёвен на хижину
inline constexpr float kBucketWood = 2.0f, kStockpileWood = 3.0f, kBucketCapacity = 3.0f;   // ведро: цена, глотков воды
inline constexpr float kBridgeWood = 1.0f, kBarricadeWood = 3.0f, kTrapWood = 2.0f;   // досок на настил / баррикаду / ловушку
inline constexpr float kSpearWood = 2.0f, kShieldWood = 3.0f, kMaxWood = 4.0f;   // цена орудий и сколько можно нести
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
    // Для мести (Behavior::Sabotage): ближайший куст, чей внешний вид существо знает как плохой; и тот, на кого зуб.
    float     badFoodDist = kNoTarget;  glm::vec3 badFoodPos{};
    float     grudgeDist  = kNoTarget;  glm::vec3 grudgePos{};
    // Ресурсы: ближайшее дерево для рубки (ищет мир, только если существу нужно дерево) и "видит ли хищника" (запоминается как опасность).
    float     treeDist = kNoTarget;  glm::vec3 treePos{};
    bool      predatorSeen = false;
    // Жажда и вода: ближайший берег (точка на суше у воды), добыча для копья (травоядное), склад.
    float     waterDist = kNoTarget;  glm::vec3 waterPos{};
    float     preyDist = kNoTarget;   glm::vec3 preyPos{};  std::uint64_t preyId = 0;
    float     stockDist = kNoTarget;  glm::vec3 stockPos{};  float stockAmount = 0.0f;
    float     cold = 0.0f;                                   // 0..1 зимний холод здесь (у костра/хижины ослаблен); только в мире с рельефом
    float     rain = 0.0f;                                   // 0..1 сила дождя (пьёт на ходу)
    float     night = 0.0f;                                  // 0 день .. 1 ночь (только в мире с рельефом: тесты без рельефа не спят)
    // Племена: чужой склад во враждебном племени (куда идти грабить) и чужак рядом со своим лагерем (кого гнать).
    float     rivalStockDist = kNoTarget;  glm::vec3 rivalStockPos{};
    float     squadDist = kNoTarget;      glm::vec3 squadPos{};  int squadPhase = 0, squadKind = 0;   // цель отряда (сбор у вождя / поход на склад / возвращение)
    float     leaderDist = kNoTarget;     glm::vec3 leaderPos{};   // вождь своего племени (если это не сам существо)
    float     intruderDist = kNoTarget;    glm::vec3 intruderPos{};  std::uint64_t intruderId = 0;
    float     hutDist = kNoTarget;    glm::vec3 hutPos{};   // подходящая хижина (своя/семьи ближе чужих)
    float     fireDist = kNoTarget;   glm::vec3 firePos{};  // ближайший горящий костёр
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
    std::uint8_t deathCause = 0;       // 0 жив, 1 старость, 2 голод, 3 истощение, 4 яд (для статистики мира)

    // Отравление ягодами (см. BerryDef: у каждого вида своё действие). Все уровни 0 -- здоров; сами выводятся.
    //  toxin -- смертельный яд: бьёт по здоровью и силам (смерть: deathCause 4);
    //  diarrhea -- понос: еда проскакивает (голод растёт), силы тают, идёт медленнее;
    //  nausea -- тошнота: по достижении 1 приступ рвоты (vomitStun секунд стоит на месте, желудок пуст, часть яда выведена);
    //  hallucination -- галлюцинации: мерещатся угрозы (visionTimer/visionHeading: бежит от несуществующего, зовёт на
    //  помощь), курс неверный.
    float toxin = 0.0f, diarrhea = 0.0f, nausea = 0.0f, hallucination = 0.0f;
    float vomitStun = 0.0f, visionTimer = 0.0f, visionHeading = 0.0f;
    // Память о вкусе ягод -- по ВНЕШНЕМУ ВИДУ (BerryLook), а не по растению: подделки под съедобные неотличимы, и мнение
    // складывается о всей группе. taste -- чем кончались прошлые трапезы (сытность минус вред), tasteSamples -- сколько
    // "тиков еды" это длилось (0 -- группа незнакома). Живёт всю жизнь, потомству не передаётся (как learnedWeights).
    std::array<float, kBerryLookCount>        taste{};
    std::array<std::uint8_t, kBerryLookCount> tasteSamples{};

    // Социальное. mealLook/sinceBite -- что и когда ело в последний раз (внешний вид; 255 -- ничего): если вскоре станет плохо,
    // соседи, видевшие трапезу, запомнят эту ягоду как плохую (sickEvent: мир читает и сбрасывает; см. World::tick).
    // grudgeId/grudge -- на кого зуб (0 -- ни на кого) и насколько сильный: копится у завистливых рядом с чужой
    // демонстрацией статуса (needs.resentment). carryKind/carryAmount/carryAge -- ягоды, которые несёт, чтобы
    // подсунуть обидчику (Behavior::Sabotage). pickRequest/serveRequest -- что существо просит у мира в этом тике.
    std::uint8_t  mealLook  = 255;
    float         sinceBite = 1000.0f;
    std::uint8_t  sickEvent = 0;
    std::uint64_t grudgeId  = 0;
    float         grudge    = 0.0f;
    std::uint8_t  carryKind = 255;
    float         carryAmount = 0.0f, carryAge = 0.0f;
    std::uint8_t  pickRequest = 0, serveRequest = 0;

    // Ресурсы и орудия (этап "цивилизация"). wood -- брёвна в руках; spearDur/shieldDur -- прочность копья/щита (0 -- нет);
    // skills -- маска умений; dangerMemory -- сколько ещё секунд помнит хищника рядом (подталкивает мастерить и рубить).
    // chopRequest -- просит мир рубить дерево treeCx/treeCz (кэш поиска ближайшего: treeKnown/treeX/treeZ/treeSearch, ведёт мир).
    std::uint16_t skills = 0;
    float        wood = 0.0f, spearDur = 0.0f, shieldDur = 0.0f, dangerMemory = 0.0f, chopProgress = 0.0f, craftProgress = 0.0f;
    std::uint8_t chopRequest = 0;
    // Жажда (только в мире с рельефом), ведро с водой, охота и склад. thirst 0..1 (1 -- здоровье тает); water -- глотков в ведре;
    // huntRequest/stockRequest (1 сдать, 2 взять) исполняет мир; waterKnown/waterX/Z/waterSearch -- кэш поиска берега.
    float        thirst = 0.2f, water = 0.0f, huntTimer = 0.0f, waterX = 0.0f, waterZ = 0.0f, waterSearch = 0.0f;
    bool         hasBucket = false, waterKnown = false;
    bool         hutNear = false;
    std::uint8_t squad = 0;         // отряд (индекс племени, 0 -- вне отряда)
    std::uint8_t tribe = 0;         // племя 1..3 (0 -- ничьё): наследуется от матери; свои-по-племени считаются знакомыми
    std::uint8_t raidRequest = 0, strikeRequest = 0;   // 1 -- у чужого склада: взять брёвна / рядом с чужаком: ударить (исполняет мир)   // видит хижину (решает слой поведения каждый тик; нужно woodGoal)
    std::uint8_t huntRequest = 0, stockRequest = 0;
    // Стройка (мост/баррикада/ловушка): buildRequest -- 1 мост, 2 баррикада, 3 ловушка, ставится на ТОТ тик, когда работа закончена
    // (мир исполняет); buildProgress -- работа идёт; buildTimer -- пауза до следующей.
    std::uint8_t buildRequest = 0;
    float        buildProgress = 0.0f, buildTimer = 0.0f, buildYaw = 0.0f;
    glm::vec2    buildAt{};
    bool         treeKnown = false;
    std::int32_t treeCx = 0, treeCz = 0;
    float        treeX = 0.0f, treeZ = 0.0f, treeSearch = 0.0f;
    [[nodiscard]] bool  knows(Skill s) const noexcept { return (skills & s) != 0; }
    [[nodiscard]] Role  role() const noexcept;
    [[nodiscard]] bool  hasSpear() const noexcept { return spearDur > 0.0f; }
    [[nodiscard]] bool  hasShield() const noexcept { return shieldDur > 0.0f; }
    // Сколько брёвен нужно на следующее орудие, которое существо умеет, но не имеет (0 -- нечего мастерить); какое именно (1 копьё, 2 щит).
    [[nodiscard]] float woodNeeded() const noexcept { const int t = nextTool(); return t == 1 ? kSpearWood : (t == 2 ? kShieldWood : (t == 3 ? kBucketWood : 0.0f)); }
    [[nodiscard]] int   nextTool() const noexcept {
        if (knows(kSkillSpear) && !hasSpear()) return 1;
        if (knows(kSkillShield) && !hasShield()) return 2;
        return (knows(kSkillBucket) && !hasBucket) ? 3 : 0;
    }
    // Сколько брёвен хочется иметь: на следующее орудие, на пару настилов моста, на баррикаду/ловушку, пока помнит хищника.
    [[nodiscard]] float woodGoal() const noexcept {
        float goal = woodNeeded();
        if (knows(kSkillBridge)) goal = std::max(goal, 2.0f);
        if (knows(kSkillHut) && !hutNear) goal = std::max(goal, kHutWood);
        if (dangerMemory > 0.0f && knows(kSkillBarricade)) goal = std::max(goal, kBarricadeWood);
        if (dangerMemory > 0.0f && knows(kSkillTrap)) goal = std::max(goal, kTrapWood);
        return goal;
    }
    [[nodiscard]] bool  wantsWood() const noexcept { return woodGoal() > wood && wood < kMaxWood; }

    // Поведение: текущая линия, сколько она держится (гистерезис против дёрганья), направление блуждания
    // и память о последнем месте с едой (место, где еду видели, -- туда идём, когда еды в поле зрения нет).
    Behavior  behavior = Behavior::Wander;
    float     behaviorTimer = 0.0f;
    float     wanderHeading = 0.0f;
    glm::vec3 foodMemory{};
    float     foodMemoryAge = -1.0f;   // < 0 -- памяти нет
    float     exploreTimer = 0.0f;     // > 0 -- идёт искать новые места (после трапезы или пустого куста), а не толчётся у еды

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

    // Ягоды (мир вызывает на каждом тике поедания куста). tasteBerry -- запомнить вкус: сытность `nutrition` против
    // вреда `toxicity` (скорость обучения -- от пластичности). addToxin -- принять яд (еда или чья-то "помощь").
    void tasteBerry(BerryKind kind, float nutrition, float harm, float dt) noexcept;
    // Принять ягоду: bite -- съеденное за тик. Копит яд/понос/тошноту/галлюцинации по профилю вида.
    void ingestBerry(const BerryDef& def, float bite) noexcept;
    void addToxin(float amount) noexcept;
    // Мнение по рассказу/наблюдению (не по собственной трапезе): сдвигает вкус группы к experience с силой weight.
    void observeLook(BerryLook look, float experience, float weight) noexcept;
    // Насколько привлекает вид: незнакомый -- с авансом по любопытству (0.6..1.1), знакомый -- его вкус
    // (сытная ягода ~1+, ядовитая < 0). Мир берёт куст с лучшим привлечением на расстояние и обходит те, что < kMinAppeal.
    [[nodiscard]] float berryAppeal(BerryKind kind) const noexcept;
    [[nodiscard]] bool  knowsBerry(BerryKind kind) const noexcept { return knowsLook(lookOf(kind)); }
    [[nodiscard]] bool  knowsLook(BerryLook look) const noexcept { return tasteSamples[static_cast<std::size_t>(look)] > 0; }
    [[nodiscard]] bool  sick() const noexcept { return toxin > 0.05f || diarrhea > 0.05f || nausea > 0.05f || hallucination > 0.05f || vomitStun > 0.0f; }
    static constexpr float kMinAppeal = 0.15f;  // ниже -- обходит куст, пока не припёрло (см. World)
    void rest(float amount) noexcept;                     // мир вызывает при успешном отдыхе
    void mate(std::uint64_t partnerId) noexcept;           // мир вызывает при успешном спаривании: сбрасывает либидо, запоминает партнёра
    void recordChild(std::uint64_t childId);                // мир вызывает после назначения ребёнку id
    void earnRespect(float amount) noexcept;                 // мир вызывает, когда демонстрация статуса реально выигрывает

    [[nodiscard]] static Creature reproduce(const Creature& a, const Creature& b, Rng& rng);
};

}  // namespace wsim
