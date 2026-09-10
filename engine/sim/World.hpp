#pragma once

#include "sim/Creature.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
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
};

// Population + еда + пространственный индекс для соседских запросов.
// Единственный слой, которому разрешено вызывать world-driven мутаторы
// Creature (feed/rest/mate/recordChild) и Creature::reproduce() -- сам
// Creature этого не делает (см. Creature.hpp). Ноль зависимостей от
// renderer/Vulkan -- Application только читает population() для отрисовки.
class World {
public:
    static constexpr float       kWorldHalfSize     = 100.0f;
    static constexpr std::size_t kInitialPopulation  = 24;
    static constexpr std::size_t kMaxPopulation       = 200;  // защитный потолок: голод пока не убивает (см. CLAUDE.md), иначе популяция не ограничена ничем
    static constexpr std::size_t kFoodCount           = 20;

    explicit World(std::uint32_t seed);

    // Один шаг: сенсы -> мозг -> резолв действий -> размножение -> смерть -> еда.
    void tick(float dt);

    [[nodiscard]] std::span<const Inhabitant>  population() const noexcept { return m_population; }
    [[nodiscard]] std::span<const FoodSource>  food() const noexcept { return m_food; }
    [[nodiscard]] std::size_t populationCount() const noexcept { return m_population.size(); }
    [[nodiscard]] std::size_t foodCount() const noexcept { return m_food.size(); }

private:
    Rng           m_rng;
    std::uint64_t m_nextId = 1;

    std::vector<Inhabitant>  m_population;
    std::vector<FoodSource>  m_food;

    // id выжившего родственника только что умершего существа -> ping на
    // ровно следующий тик (см. SenseInputs::lovedOneDied -- тот же
    // одноразовый контракт). Заполняется в конце tick(), где умер кто-то;
    // читается и полностью очищается в начале следующего tick().
    std::unordered_map<std::uint64_t, float> m_pendingGrief;
};

}  // namespace wsim
