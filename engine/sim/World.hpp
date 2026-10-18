#pragma once

#include "sim/Berries.hpp"
#include "sim/Creature.hpp"
#include "sim/Terrain.hpp"

#include <cstddef>
#include <string>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wsim {

// Существо в мире + его сигналы наружу с ПРОШЛОГО тика (alarmCall/matingCall/
// dominanceDisplay). Прошлый, а не текущий тик -- иначе соседи в одном
// vector<Inhabitant> сенсили бы друг друга в порядке, зависящем от индекса
// (кто раньше в векторе -- у того уже "будущее" состояние). Отдельно от
// Creature, а не поле в нём: Creature остаётся тем же "мир не лезет внутрь,
// сам ничего не ищет" объектом, что и раньше -- lastOutputs это забота мира.
struct Inhabitant {
    Creature     creature;
    BrainOutputs lastOutputs{};
};

// Точка еды. Не "исчезает" при истощении -- просто amount падает к 0 и
// медленно восстанавливается на месте (см. World::tick()).
struct FoodSource {
    glm::vec3 position{};
    float     amount = 0.0f;
    BerryKind kind   = BerryKind::Raspberry;  // вид ягод: сытность, скорость отрастания, цвет (sim/Berries)
};

// Population + еда + пространственный индекс для соседских запросов.
// Единственный слой, которому разрешено вызывать world-driven мутаторы
// Creature (feed/rest/mate/recordChild) и Creature::reproduce() -- сам
// Creature этого не делает (см. Creature.hpp). Ноль зависимостей от
// renderer/Vulkan -- Application только читает population() для отрисовки.
// Постройка: пока только точка с поворотом и зерном вида (сам меш рисует Application по зерну).
// Существа её не замечают -- это декорация и мишень для валунов/ластика; обитаемые дома --
// будущая система цивилизаций.
struct Building {
    float         x = 0, z = 0, yaw = 0;  // yaw в градусах, фасад смотрит в +Z до поворота
    std::uint32_t seed = 0;               // вид дома (houseVariant)
};

// Динамическая опасность, которую существа чувствуют и от которой бегут: катящийся валун, место удара.
// Снаружи (Application) выставляется каждый кадр; в сохранение не входит -- это не состояние мира.
struct Hazard {
    glm::vec3 pos{};
    float     radius = 3.0f;  // само тело/зона поражения; страх начинается раньше (kHazardSense)
};

// Счётчики жизни мира -- для отладочного окна и тестов баланса.
struct WorldStats {
    std::uint64_t births = 0, matings = 0;
    std::uint64_t deathsOld = 0, deathsStarved = 0, deathsExhausted = 0, deathsOther = 0;
};

// Полное состояние мира для сохранения/загрузки (всё, что нужно, чтобы продолжить симуляцию
// бит в бит: в том числе состояние ГСЧ и ожидающие пинги горя).
struct WorldState {
    std::string                                          rng;      // текст состояния std::mt19937
    std::uint64_t                                        nextId = 1;
    std::vector<Inhabitant>                              population;
    std::vector<FoodSource>                              food;
    std::vector<Building>                                buildings;
    std::vector<std::pair<std::uint64_t, float>>         pendingGrief;  // по возрастанию id
};

class World {
public:
    static constexpr float       kWorldHalfSize     = 100.0f;
    static constexpr std::size_t kInitialPopulation  = 24;
    static constexpr std::size_t kMaxPopulation       = 200;  // защитный потолок: голод пока не убивает (см. CLAUDE.md), иначе популяция не ограничена ничем
    static constexpr std::size_t kFoodCount           = 30;

    explicit World(std::uint32_t seed);

    [[nodiscard]] WorldState exportState() const;
    // false -- состояние некорректно (ГСЧ не разобрался); мир при этом не тронут.
    bool importState(const WorldState& state);

    // Один шаг: сенсы -> мозг -> резолв действий -> размножение -> смерть -> еда.
    // terrain != nullptr -- существа подчиняются рельефу (уклон, вода); nullptr --
    // плоский мир без ограничений (тесты, декоративная колония в меню).
    void tick(float dt, const Terrain* terrain = nullptr);

    // Опасности на текущий кадр (заменяют предыдущие). Существа в радиусе kHazardSense от края чувствуют угрозу.
    void setHazards(std::span<const Hazard> hazards) { m_hazards.assign(hazards.begin(), hazards.end()); }
    [[nodiscard]] const WorldStats& stats() const noexcept { return m_stats; }

    // Переселяет существ и еду, оказавшихся в воде, на случайную сушу в границах мира.
    void settleOnLand(const Terrain& terrain);
    // Заново сажает ягодные кусты по рельефу: пятна по 1-3 куста на суше, густота и виды -- по биому
    // (sim/Berries). Заменяет прежнюю еду (при создании мира и в "Новом мире"); ГСЧ -- мировой, так что результат
    // воспроизводим и после загрузки сохранения продолжается тем же.
    void growBerryBushes(const Terrain& terrain);

    [[nodiscard]] std::span<const Inhabitant>  population() const noexcept { return m_population; }
    [[nodiscard]] std::span<const FoodSource>  food() const noexcept { return m_food; }
    [[nodiscard]] std::size_t populationCount() const noexcept { return m_population.size(); }
    [[nodiscard]] std::size_t foodCount() const noexcept { return m_food.size(); }

    // ── Руки бога: вмешательство игрока в мир (см. sim/Powers) ──────────────
    // Новое существо в точке (x, z). Черты из traits (индекс -> [-1,1]) заменяют
    // случайные; мозг всегда случайный. false -- популяция уже на потолке.
    bool spawnCreature(float x, float z, std::span<const std::pair<Genome::TraitIndex, float>> traits = {});
    // Убирает всех существ в круге; возвращает сколько убрано.
    std::size_t removeCreatures(float x, float z, float radius);
    [[nodiscard]] std::size_t countCreatures(float x, float z, float radius) const;

    static constexpr std::size_t kMaxBuildings   = 300;
    static constexpr float       kBuildingSpacing = 6.0f;  // ближе центры не ставим
    // false -- слишком близко к другому дому или их уже kMaxBuildings.
    bool addBuilding(float x, float z, float yaw, std::uint32_t seed);
    std::size_t removeBuildings(float x, float z, float radius);
    [[nodiscard]] std::span<const Building> buildings() const noexcept { return m_buildings; }

    static constexpr std::size_t kMaxFood      = 150;
    static constexpr float       kBushSpacing  = 1.6f;   // ближе центры не сажаем
    // Куст в (x, z) данного вида, полный. false -- слишком близко к другому кусту или их уже kMaxFood.
    bool plantBush(float x, float z, BerryKind kind);
    // То же, но вид -- по биому точки (случайный из подходящих); false как у plantBush.
    bool plantNaturalBush(const Terrain& terrain, float x, float z);
    std::size_t removeBushes(float x, float z, float radius);
    // Ближайший куст не дальше radius (индекс в food()); false -- нет.
    [[nodiscard]] bool nearestBush(float x, float z, float radius, std::size_t& index) const;

private:
    Rng           m_rng;
    std::uint64_t m_nextId = 1;

    std::vector<Inhabitant>  m_population;
    std::vector<FoodSource>  m_food;
    std::vector<Building>    m_buildings;
    std::vector<Hazard>      m_hazards;
    WorldStats               m_stats;

    // id выжившего родственника только что умершего существа -> ping на
    // ровно следующий тик (см. SenseInputs::lovedOneDied -- тот же
    // одноразовый контракт). Заполняется в конце tick(), где умер кто-то;
    // читается и полностью очищается в начале следующего tick().
    std::unordered_map<std::uint64_t, float> m_pendingGrief;
};

}  // namespace wsim
