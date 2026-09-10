#include "sim/World.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <utility>

namespace wsim {

namespace {

// ─── Баланс (плейсхолдер-числа -- та же трактовка, что и у lifespan()'s
// секунд/kHomeRangeScale в Creature.cpp: разумные первые прикидки, не
// выверенный баланс, до появления террейна/god-tools их придётся подкрутить). ─

constexpr float kCellSize            = 15.0f;   // сторона ячейки пространственной сетки
constexpr float kSenseRadius         = 25.0f;   // радиус, в котором существо видит еду/угрозы/партнёров/толпу
constexpr float kInteractRadius      = 2.0f;    // дистанция, на которой можно реально съесть/спариться
constexpr float kFamilySenseRange    = 200.0f;  // "любовь" не ограничена kSenseRadius -- родных чувствуешь издалека (перекрывает диагональ мира ~283)
constexpr float kEatDesireThreshold  = 0.5f;
constexpr float kRestDesireThreshold = 0.5f;
constexpr float kMateDesireThreshold = 0.5f;
constexpr float kMinMateLibido       = 0.4f;    // кого вообще считать "готовым" партнёром при сенсинге
constexpr float kEatRate             = 0.5f;    // единиц голода/сек, пока существо ест
constexpr float kRestRate            = 0.5f;    // единиц энергии/сек, пока существо отдыхает
constexpr float kFoodRegrowRate      = 0.05f;   // единиц/сек восстановления источника еды
constexpr float kFoodCap             = 1.0f;

using Grid = std::unordered_map<std::int64_t, std::vector<std::size_t>>;

std::int64_t packCellKey(std::int64_t cx, std::int64_t cz) noexcept {
    return (static_cast<std::int64_t>(static_cast<std::uint32_t>(cx)) << 32) |
           static_cast<std::uint32_t>(static_cast<std::int32_t>(cz));
}

template <typename PosFn>
Grid buildGrid(std::size_t count, PosFn&& posOf) {
    Grid grid;
    grid.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 p  = posOf(i);
        const auto      cx = static_cast<std::int64_t>(std::floor(p.x / kCellSize));
        const auto      cz = static_cast<std::int64_t>(std::floor(p.z / kCellSize));
        grid[packCellKey(cx, cz)].push_back(i);
    }
    return grid;
}

// Обходит все элементы сетки в пределах radius от center, вызывая
// visit(index, dist) для каждого -- это и есть тот O(n) (вместо O(n^2))
// соседский запрос, который TODO называл "spatial-grid system".
template <typename PosFn, typename VisitFn>
void forEachNearby(const Grid& grid, const glm::vec3& center, float radius, PosFn&& posOf, VisitFn&& visit) {
    const auto cellRadius = static_cast<std::int64_t>(std::ceil(radius / kCellSize)) + 1;
    const auto ccx        = static_cast<std::int64_t>(std::floor(center.x / kCellSize));
    const auto ccz        = static_cast<std::int64_t>(std::floor(center.z / kCellSize));

    for (std::int64_t dx = -cellRadius; dx <= cellRadius; ++dx) {
        for (std::int64_t dz = -cellRadius; dz <= cellRadius; ++dz) {
            const auto it = grid.find(packCellKey(ccx + dx, ccz + dz));
            if (it == grid.end()) continue;
            for (std::size_t idx : it->second) {
                const glm::vec3 p    = posOf(idx);
                const float     dist = glm::length(glm::vec2{p.x, p.z} - glm::vec2{center.x, center.z});
                if (dist <= radius) visit(idx, dist);
            }
        }
    }
}

glm::vec2 dirXZ(const glm::vec3& from, const glm::vec3& to) noexcept {
    const glm::vec2 d{to.x - from.x, to.z - from.z};
    const float      len = glm::length(d);
    return len > 1e-4f ? d / len : glm::vec2{0.0f, 0.0f};
}

bool isFamiliar(const Creature& self, std::uint64_t otherId) noexcept {
    if (otherId == self.bondedPartnerId) return true;
    if (otherId == self.parentIds[0] || otherId == self.parentIds[1]) return true;
    for (auto id : self.childIds)
        if (id == otherId) return true;
    return false;
}

glm::vec3 randomPointInBounds(Rng& rng) {
    std::uniform_real_distribution<float> dist(-World::kWorldHalfSize, World::kWorldHalfSize);
    return {dist(rng), 0.0f, dist(rng)};
}

}  // namespace

World::World(std::uint32_t seed) : m_rng(seed) {
    m_population.reserve(kMaxPopulation);
    for (std::size_t i = 0; i < kInitialPopulation; ++i) {
        Creature c;
        c.genome   = Genome::random(m_rng);
        c.position = randomPointInBounds(m_rng);
        c.id       = m_nextId++;
        m_population.push_back(Inhabitant{std::move(c), BrainOutputs{}});
    }

    m_food.reserve(kFoodCount);
    for (std::size_t i = 0; i < kFoodCount; ++i)
        m_food.push_back(FoodSource{randomPointInBounds(m_rng), kFoodCap});
}

void World::tick(float dt) {
    // 1. Прошлотиковые пинги горя -- одноразовые, читаются один раз и всё.
    const std::unordered_map<std::uint64_t, float> grief = std::move(m_pendingGrief);
    m_pendingGrief.clear();

    const std::size_t n = m_population.size();

    // 2. id -> индекс (для поиска близких по родству) + пространственные сетки.
    std::unordered_map<std::uint64_t, std::size_t> idIndex;
    idIndex.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        idIndex[m_population[i].creature.id] = i;

    const Grid creatureGrid = buildGrid(n, [this](std::size_t i) { return m_population[i].creature.position; });

    std::vector<std::size_t> liveFoodIdx;
    liveFoodIdx.reserve(m_food.size());
    for (std::size_t i = 0; i < m_food.size(); ++i)
        if (m_food[i].amount > 0.0f) liveFoodIdx.push_back(i);
    const Grid foodGrid = buildGrid(
        liveFoodIdx.size(), [this, &liveFoodIdx](std::size_t k) { return m_food[liveFoodIdx[k]].position; });

    // 3. Pass 1: сенсы -> мозг -> резолв eat/rest. Всё чтение чужого
    // состояния идёт из сеток/lastOutputs, построенных до этого цикла --
    // порядок обработки i не влияет на корректность (см. World.hpp).
    std::vector<BrainOutputs> freshOutputs(n);
    for (std::size_t i = 0; i < n; ++i) {
        Creature&       self    = m_population[i].creature;
        const glm::vec3 selfPos = self.position;

        SenseInputs senses{};

        // Еда.
        float       bestFoodDist = kSenseRadius;
        std::size_t bestFoodK    = liveFoodIdx.size();
        forEachNearby(
            foodGrid, selfPos, kSenseRadius,
            [this, &liveFoodIdx](std::size_t k) { return m_food[liveFoodIdx[k]].position; },
            [&](std::size_t k, float dist) {
                if (dist < bestFoodDist) {
                    bestFoodDist = dist;
                    bestFoodK    = k;
                }
            });
        const bool foundFood = bestFoodK != liveFoodIdx.size();
        if (foundFood) {
            senses.foodDir       = dirXZ(selfPos, m_food[liveFoodIdx[bestFoodK]].position);
            senses.foodProximity = std::clamp(1.0f - bestFoodDist / kSenseRadius, 0.0f, 1.0f);
        }

        // Угрозы: предаторов/боя ещё нет -- см. CLAUDE.md "Known gaps".
        senses.threatDir      = {0.0f, 0.0f};
        senses.threatProximity = 0.0f;

        // Соседи: толпа своих/чужих, агрегированные сигналы (прошлый тик),
        // ближайший потенциальный партнёр (по либидо -- сенсинг, не резолв).
        int         familiarCount = 0, strangerCount = 0;
        float       nearbyFamiliarAlarm = 0.0f, nearbyStrangerAlarm = 0.0f;
        float       nearbyMatingCall = 0.0f, nearbyDominance = 0.0f;
        float       bestMateDist = std::numeric_limits<float>::max();
        std::size_t bestMateJ    = n;
        forEachNearby(
            creatureGrid, selfPos, kSenseRadius,
            [this](std::size_t j) { return m_population[j].creature.position; },
            [&](std::size_t j, float dist) {
                if (j == i) return;
                const Inhabitant& other = m_population[j];
                if (isFamiliar(self, other.creature.id)) {
                    ++familiarCount;
                    nearbyFamiliarAlarm = std::max(nearbyFamiliarAlarm, other.lastOutputs.alarmCall);
                } else {
                    ++strangerCount;
                    nearbyStrangerAlarm = std::max(nearbyStrangerAlarm, other.lastOutputs.alarmCall);
                }
                nearbyMatingCall = std::max(nearbyMatingCall, other.lastOutputs.matingCall);
                nearbyDominance  = std::max(nearbyDominance, other.lastOutputs.dominanceDisplay);
                if (other.creature.needs.libido > kMinMateLibido && dist < bestMateDist) {
                    bestMateDist = dist;
                    bestMateJ    = j;
                }
            });
        senses.familiarCrowding = static_cast<float>(familiarCount) / (static_cast<float>(familiarCount) + 2.0f);
        senses.strangerCrowding = static_cast<float>(strangerCount) / (static_cast<float>(strangerCount) + 2.0f);
        senses.nearbyFamiliarAlarmLevel = nearbyFamiliarAlarm;
        senses.nearbyStrangerAlarmLevel = nearbyStrangerAlarm;
        senses.nearbyMatingCallLevel    = nearbyMatingCall;
        senses.nearbyDominanceLevel     = nearbyDominance;
        if (bestMateJ != n) {
            senses.mateDir       = dirXZ(selfPos, m_population[bestMateJ].creature.position);
            senses.mateProximity = std::clamp(1.0f - bestMateDist / kSenseRadius, 0.0f, 1.0f);
        }

        // Родные: не ограничены kSenseRadius -- "любовь" не имеет дальности
        // видимости, см. kFamilySenseRange выше.
        float       bestLovedDist = std::numeric_limits<float>::max();
        std::size_t bestLovedIdx  = n;
        auto        considerLoved = [&](std::uint64_t id) {
            if (id == 0) return;
            const auto it = idIndex.find(id);
            if (it == idIndex.end()) return;
            const float dist = glm::length(m_population[it->second].creature.position - selfPos);
            if (dist < bestLovedDist) {
                bestLovedDist = dist;
                bestLovedIdx  = it->second;
            }
        };
        considerLoved(self.bondedPartnerId);
        considerLoved(self.parentIds[0]);
        considerLoved(self.parentIds[1]);
        for (auto childId : self.childIds) considerLoved(childId);
        if (bestLovedIdx != n) {
            const Creature& loved  = m_population[bestLovedIdx].creature;
            senses.lovedOneDir        = dirXZ(selfPos, loved.position);
            senses.lovedOneProximity  = std::clamp(1.0f - bestLovedDist / kFamilySenseRange, 0.0f, 1.0f);
            senses.lovedOneThreatLevel = loved.needs.fear;
        }
        senses.lovedOneDied = grief.count(self.id) > 0 ? 1.0f : 0.0f;

        const BrainOutputs out = self.tick(dt, senses);
        freshOutputs[i]        = out;

        if (out.eatDesire > kEatDesireThreshold && foundFood && bestFoodDist <= kInteractRadius) {
            FoodSource& src  = m_food[liveFoodIdx[bestFoodK]];
            const float bite = std::min(src.amount, kEatRate * dt);
            self.feed(bite);
            src.amount -= bite;
        }
        if (out.restDesire > kRestDesireThreshold) self.rest(kRestRate * dt);
        // fleeDesire/defendDesire: выражены, не резолвятся -- нет ни
        // предаторов, ни боя (см. CLAUDE.md "Known gaps").
    }
    for (std::size_t i = 0; i < n; ++i)
        m_population[i].lastOutputs = freshOutputs[i];

    // 4. Pass 2: спаривание -- нужен mateDesire ОБЕИХ сторон, доступный
    // только после того, как все в Pass 1 уже подумали.
    std::vector<bool>    mated(n, false);
    std::vector<Creature> newborns;
    for (std::size_t i = 0; i < n; ++i) {
        if (mated[i] || freshOutputs[i].mateDesire <= kMateDesireThreshold) continue;

        float       bestDist = std::numeric_limits<float>::max();
        std::size_t bestJ    = n;
        forEachNearby(
            creatureGrid, m_population[i].creature.position, kInteractRadius,
            [this](std::size_t j) { return m_population[j].creature.position; },
            [&](std::size_t j, float dist) {
                if (j == i || mated[j]) return;
                if (freshOutputs[j].mateDesire <= kMateDesireThreshold) return;
                if (dist < bestDist) {
                    bestDist = dist;
                    bestJ    = j;
                }
            });
        if (bestJ == n) continue;

        Creature& a = m_population[i].creature;
        Creature& b = m_population[bestJ].creature;
        a.mate(b.id);
        b.mate(a.id);
        mated[i]     = true;
        mated[bestJ] = true;

        if (m_population.size() + newborns.size() < kMaxPopulation) {
            Creature child = Creature::reproduce(a, b, m_rng);
            child.id       = m_nextId++;
            a.recordChild(child.id);
            b.recordChild(child.id);
            newborns.push_back(std::move(child));
        }
    }

    // 5. Смерть (Creature::tick() уже выставил alive=false по старости) --
    // пингуем родню горем на следующий тик, затем убираем из популяции.
    for (const auto& inh : m_population) {
        if (inh.creature.alive) continue;
        const Creature& c    = inh.creature;
        auto             ping = [this](std::uint64_t id) {
            if (id != 0) m_pendingGrief[id] = 1.0f;
        };
        ping(c.bondedPartnerId);
        ping(c.parentIds[0]);
        ping(c.parentIds[1]);
        for (auto childId : c.childIds) ping(childId);
    }
    std::erase_if(m_population, [](const Inhabitant& inh) { return !inh.creature.alive; });

    // 6. Новорождённые -- добавляются постфактум, не участвуют в этом тике.
    for (auto& child : newborns)
        m_population.push_back(Inhabitant{std::move(child), BrainOutputs{}});

    // 7. Еда медленно восстанавливается на месте.
    for (auto& src : m_food)
        src.amount = std::min(kFoodCap, src.amount + kFoodRegrowRate * dt);
}

}  // namespace wsim
