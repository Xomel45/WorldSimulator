#include "sim/World.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

namespace wsim {

namespace {

// ─── Баланс (плейсхолдер-числа -- та же трактовка, что и у lifespan()'s
// секунд/kHomeRangeScale в Creature.cpp: разумные первые прикидки, не
// выверенный баланс, до появления террейна/god-tools их придётся подкрутить). ─

constexpr float kCellSize            = 15.0f;   // сторона ячейки пространственной сетки
constexpr float kHazardSense         = 18.0f;   // с какого расстояния от края опасности (валуна) существо её чувствует
constexpr float kSeparationRadius    = 3.0f;    // ближе -- существа расталкиваются, чтобы не слипаться в кучу
constexpr float kGroupRadius         = 18.0f;   // кого считаем "своей группой" для тяги к сородичам
constexpr float kFamilySenseRange    = 200.0f;  // "любовь" не ограничена kSenseRadius -- родных чувствуешь издалека (перекрывает диагональ мира ~283)
constexpr float kEatDesireThreshold  = 0.5f;
constexpr float kRestDesireThreshold = 0.5f;
constexpr float kMateDesireThreshold = 0.5f;
constexpr float kEatRate             = 0.35f;   // единиц голода/сек, пока существо ест
constexpr float kRestRate            = 0.06f;   // единиц энергии/сек, пока существо отдыхает
constexpr float kMateChancePerSec    = 0.9f;    // шанс успеха спаривания в секунду у пары рядом (x fertility)
constexpr float kMotherEnergyCost    = 0.22f;   // роды и вынашивание стоят сил
constexpr float kFatherEnergyCost    = 0.08f;
constexpr float kFoodRegrowRate      = 0.04f;   // единиц/сек восстановления источника еды
constexpr float kFoodCap             = 1.0f;
constexpr float kFoodMinEdible       = 0.12f;   // меньше -- куст "пустой": его не видно как еду, пока не отрастёт (иначе все висят на огрызках)

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
        c.age      = c.lifespan() * std::uniform_real_distribution<float>(0.12f, 0.45f)(m_rng);  // взрослые разного возраста, не новорождённые
        m_population.push_back(Inhabitant{std::move(c), BrainOutputs{}});
    }

    m_food.reserve(kFoodCount);
    for (std::size_t i = 0; i < kFoodCount; ++i)
        m_food.push_back(FoodSource{randomPointInBounds(m_rng), kFoodCap});
}

bool World::spawnCreature(float x, float z, std::span<const std::pair<Genome::TraitIndex, float>> traits) {
    if (m_population.size() >= kMaxPopulation) return false;
    Creature c;
    c.genome   = Genome::random(m_rng);
    for (const auto& [index, value] : traits) c.genome.setTrait(index, value);
    c.position = {x, 0.0f, z};
    c.id       = m_nextId++;
    c.age      = c.lifespan() * std::uniform_real_distribution<float>(0.12f, 0.35f)(m_rng);  // созданные богом -- взрослые
    m_population.push_back(Inhabitant{std::move(c), BrainOutputs{}});
    return true;
}

std::size_t World::removeCreatures(float x, float z, float radius) {
    const auto before = m_population.size();
    std::erase_if(m_population, [&](const Inhabitant& in) {
        return std::hypot(in.creature.position.x - x, in.creature.position.z - z) <= radius;
    });
    return before - m_population.size();
}

std::size_t World::countCreatures(float x, float z, float radius) const {
    std::size_t n = 0;
    for (const auto& in : m_population)
        if (std::hypot(in.creature.position.x - x, in.creature.position.z - z) <= radius) ++n;
    return n;
}

void World::settleOnLand(const Terrain& terrain) {
    auto landPoint = [&](glm::vec3 fallback) {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const glm::vec3 p = randomPointInBounds(m_rng);
            if (!terrain.isUnderwater(p.x, p.z)) return p;
        }
        return fallback;  // мир почти весь под водой -- оставляем как есть
    };
    for (auto& in : m_population) {
        Creature& c = in.creature;
        if (terrain.isUnderwater(c.position.x, c.position.z)) c.position = landPoint(c.position);
        c.position.y = terrain.heightAt(c.position.x, c.position.z);
    }
    for (auto& f : m_food)
        if (terrain.isUnderwater(f.position.x, f.position.z)) f.position = landPoint(f.position);
}

// ─── Ягодные кусты ────────────────────────────────────────────────────────────
bool World::plantBush(float x, float z, BerryKind kind) {
    if (m_food.size() >= kMaxFood) return false;
    for (const FoodSource& f : m_food)
        if (std::hypot(f.position.x - x, f.position.z - z) < kBushSpacing) return false;
    m_food.push_back(FoodSource{{x, 0.0f, z}, kFoodCap, kind});
    return true;
}

bool World::plantNaturalBush(const Terrain& terrain, float x, float z) {
    if (terrain.isUnderwater(x, z)) return false;
    const BerryKind kind = pickBerryKind(terrain.biomeAt(x, z), std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng));
    return plantBush(x, z, kind);
}

std::size_t World::removeBushes(float x, float z, float radius) {
    const auto before = m_food.size();
    std::erase_if(m_food, [&](const FoodSource& f) { return std::hypot(f.position.x - x, f.position.z - z) <= radius; });
    return before - m_food.size();
}

bool World::nearestBush(float x, float z, float radius, std::size_t& index) const {
    float best = radius;
    bool  found = false;
    for (std::size_t i = 0; i < m_food.size(); ++i) {
        const float d = std::hypot(m_food[i].position.x - x, m_food[i].position.z - z);
        if (d <= best) { best = d; index = i; found = true; }
    }
    return found;
}

void World::growBerryBushes(const Terrain& terrain) {
    constexpr float       kCell = 18.0f, kPatchRadius = 3.5f, kBaseChance = 0.24f;
    constexpr std::size_t kMinBushes = 18;  // даже в пустыне хоть что-то растёт
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    m_food.clear();

    const float half = kWorldHalfSize;
    auto land = [&](float x, float z) {
        return std::fabs(x) <= half && std::fabs(z) <= half && terrain.heightAt(x, z) > 0.4f;  // не вода и не мокрый пляж
    };
    for (float cz = -half; cz < half; cz += kCell)
        for (float cx = -half; cx < half; cx += kCell) {
            const float x0 = cx + unit(m_rng) * kCell, z0 = cz + unit(m_rng) * kCell;
            if (!land(x0, z0)) continue;
            const Biome biome = terrain.biomeAt(x0, z0);
            if (unit(m_rng) >= kBaseChance * bushDensity(biome)) continue;
            const BerryKind kind = pickBerryKind(biome, unit(m_rng));
            const int       n    = 1 + static_cast<int>(unit(m_rng) * 2.999f);  // пятно из 1-3 кустов
            for (int k = 0; k < n; ++k) {
                const float a = 6.2831853f * unit(m_rng), r = kPatchRadius * std::sqrt(unit(m_rng));
                const float x = x0 + std::cos(a) * r, z = z0 + std::sin(a) * r;
                if (land(x, z)) plantBush(x, z, kind);
            }
        }
    for (int attempt = 0; attempt < 400 && m_food.size() < kMinBushes; ++attempt) {
        const glm::vec3 p = randomPointInBounds(m_rng);
        if (land(p.x, p.z)) plantBush(p.x, p.z, pickBerryKind(terrain.biomeAt(p.x, p.z), unit(m_rng)));
    }
}

WorldState World::exportState() const {
    WorldState st;
    std::ostringstream rng;
    rng << m_rng;
    st.rng        = rng.str();
    st.nextId     = m_nextId;
    st.population = m_population;
    st.food       = m_food;
    st.buildings  = m_buildings;
    st.pendingGrief.assign(m_pendingGrief.begin(), m_pendingGrief.end());
    std::sort(st.pendingGrief.begin(), st.pendingGrief.end());
    return st;
}

bool World::importState(const WorldState& st) {
    Rng rng;
    std::istringstream in(st.rng);
    in >> rng;
    if (in.fail()) return false;
    m_rng        = rng;
    m_nextId     = std::max<std::uint64_t>(st.nextId, 1);
    m_population = st.population;
    m_food       = st.food;
    m_buildings  = st.buildings;
    m_pendingGrief.clear();
    for (const auto& [id, v] : st.pendingGrief) m_pendingGrief[id] = v;
    return true;
}

bool World::addBuilding(float x, float z, float yaw, std::uint32_t seed) {
    if (m_buildings.size() >= kMaxBuildings) return false;
    for (const auto& b : m_buildings)
        if (std::hypot(b.x - x, b.z - z) < kBuildingSpacing) return false;
    m_buildings.push_back({x, z, yaw, seed});
    return true;
}

std::size_t World::removeBuildings(float x, float z, float radius) {
    const auto before = m_buildings.size();
    std::erase_if(m_buildings, [&](const Building& b) { return std::hypot(b.x - x, b.z - z) <= radius; });
    return before - m_buildings.size();
}

void World::tick(float dt, const Terrain* terrain) {
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
        if (m_food[i].amount >= kFoodMinEdible) liveFoodIdx.push_back(i);
    const Grid foodGrid = buildGrid(
        liveFoodIdx.size(), [this, &liveFoodIdx](std::size_t k) { return m_food[liveFoodIdx[k]].position; });

    // 3. Pass 1: сенсы -> мозг и поведение -> резолв eat/rest. Всё чтение чужого
    // состояния идёт из сеток/lastOutputs, построенных до этого цикла --
    // порядок обработки i не влияет на корректность (см. World.hpp).
    std::vector<BrainOutputs> freshOutputs(n);
    for (std::size_t i = 0; i < n; ++i) {
        Creature&       self    = m_population[i].creature;
        const glm::vec3 selfPos = self.position;

        SenseInputs senses{};
        senses.terrain       = terrain;
        senses.worldHalfSize = kWorldHalfSize;

        // Еда.
        // Куст выбирается не по одной близости: учитывается, какие ягоды существо знает и любит (вкус, см.
        // Creature::tasteBerry). Приглянувшаяся издалека сытная ягода бьёт ближнюю посредственную; те, что отравили
        // (привлечение < kMinAppeal), обходятся -- пока не припёрло: сильно голодный берёт и сомнительное (но не ту, что
        // убивает: привлечение < kDesperateFloor).
        constexpr float kAppealDistScale = 12.0f, kDesperateHunger = 0.85f, kDesperateFloor = -0.4f;
        float       bestFoodDist = kSenseRadius, bestScore = 0.0f;
        std::size_t bestFoodK    = liveFoodIdx.size();
        const bool  desperate    = self.needs.hunger > kDesperateHunger;
        const bool  queasy       = self.nausea > 0.6f || self.vomitStun > 0.0f;  // тошнит -- на еду и смотреть не хочется
        forEachNearby(
            foodGrid, selfPos, kSenseRadius,
            [this, &liveFoodIdx](std::size_t k) { return m_food[liveFoodIdx[k]].position; },
            [&](std::size_t k, float dist) {
                if (queasy) return;
                const float appeal = self.berryAppeal(m_food[liveFoodIdx[k]].kind);
                if (appeal < Creature::kMinAppeal && !(desperate && appeal > kDesperateFloor)) return;
                const float score = std::max(appeal, 0.05f) / (1.0f + dist / kAppealDistScale);
                if (score > bestScore) {
                    bestScore    = score;
                    bestFoodDist = dist;
                    bestFoodK    = k;
                }
            });
        const bool foundFood = bestFoodK != liveFoodIdx.size();
        if (foundFood) {
            const glm::vec3& fp  = m_food[liveFoodIdx[bestFoodK]].position;
            senses.foodDir       = dirXZ(selfPos, fp);
            senses.foodProximity = std::clamp(1.0f - bestFoodDist / kSenseRadius, 0.0f, 1.0f);
            senses.foodDist      = bestFoodDist;
            senses.foodPos       = fp;
        }

        // Угрозы: динамические опасности снаружи (катящиеся валуны, места ударов). Хищников и боя пока нет.
        {
            float bestEdge = kHazardSense;
            for (const Hazard& h : m_hazards) {
                const float edge = std::hypot(h.pos.x - selfPos.x, h.pos.z - selfPos.z) - h.radius;
                if (edge < bestEdge) {
                    bestEdge = edge;
                    senses.threatDir       = dirXZ(selfPos, h.pos);
                    senses.threatProximity = std::clamp(1.0f - std::max(edge, 0.0f) / kHazardSense, 0.0f, 1.0f);
                    senses.threatDist      = std::max(edge, 0.0f);
                }
            }
        }

        // Соседи: толпа своих/чужих, агрегированные сигналы (прошлый тик), ближайший ГОТОВЫЙ
        // партнёр, паникующие соседи, центр группы, расталкивание.
        int         familiarCount = 0, strangerCount = 0;
        float       nearbyFamiliarAlarm = 0.0f, nearbyStrangerAlarm = 0.0f;
        float       nearbyMatingCall = 0.0f, nearbyDominance = 0.0f;
        float       bestMateDist = std::numeric_limits<float>::max();
        std::size_t bestMateJ    = n;
        float       bestAlarmDist = std::numeric_limits<float>::max();
        glm::vec2   groupSum{0.0f}, separation{0.0f};
        int         groupCount = 0;
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
                if (other.creature.canMate() && dist < bestMateDist) {
                    bestMateDist = dist;
                    bestMateJ    = j;
                }
                if (other.lastOutputs.alarmCall > 0.5f && dist < bestAlarmDist) {
                    bestAlarmDist = dist;
                    senses.alarmDir  = dirXZ(selfPos, other.creature.position);
                    senses.alarmNear = other.lastOutputs.alarmCall;
                }
                if (dist < kGroupRadius) {
                    groupSum += glm::vec2{other.creature.position.x, other.creature.position.z};
                    ++groupCount;
                }
                if (dist < kSeparationRadius && dist > 1.0e-3f) {
                    const float w = 1.0f - dist / kSeparationRadius;
                    separation += (glm::vec2{selfPos.x - other.creature.position.x, selfPos.z - other.creature.position.z} / dist) * w;
                }
            });
        senses.familiarCrowding = static_cast<float>(familiarCount) / (static_cast<float>(familiarCount) + 2.0f);
        senses.strangerCrowding = static_cast<float>(strangerCount) / (static_cast<float>(strangerCount) + 2.0f);
        senses.nearbyFamiliarAlarmLevel = nearbyFamiliarAlarm;
        senses.nearbyStrangerAlarmLevel = nearbyStrangerAlarm;
        senses.nearbyMatingCallLevel    = nearbyMatingCall;
        senses.nearbyDominanceLevel     = nearbyDominance;
        senses.separation               = separation;
        if (groupCount > 0) {
            const glm::vec2 centre = groupSum / static_cast<float>(groupCount);
            const glm::vec2 toCentre = centre - glm::vec2{selfPos.x, selfPos.z};
            senses.groupCount = groupCount;
            senses.groupDist  = glm::length(toCentre);
            senses.groupDir   = senses.groupDist > 1.0e-4f ? toCentre / senses.groupDist : glm::vec2{0.0f};
        }
        if (bestMateJ != n) {
            senses.mateDir       = dirXZ(selfPos, m_population[bestMateJ].creature.position);
            senses.mateProximity = std::clamp(1.0f - bestMateDist / kSenseRadius, 0.0f, 1.0f);
            senses.mateDist      = bestMateDist;
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
            const BerryDef& def = berryDef(src.kind);
            self.feed(bite * def.nutrition);  // морошка сытнее черники при том же объёме
            self.ingestBerry(def, bite);                                  // яд/понос/тошнота/галлюцинации по профилю вида
            self.tasteBerry(src.kind, def.nutrition, def.harm(), dt);     // вкус запоминается, пока ест
            src.amount -= bite;
        }
        if (out.restDesire > kRestDesireThreshold) self.rest(kRestRate * dt);
    }
    for (std::size_t i = 0; i < n; ++i)
        m_population[i].lastOutputs = freshOutputs[i];

    // 4. Pass 2: спаривание -- обе стороны должны решить "спариваюсь" (рядом с партнёром и в поведении
    // "ищет пару"), быть физически готовыми (canMate: зрелые, сытые, не вымотанные, перезарядка прошла)
    // и пройти бросок шанса. Потомство -- рядом с матерью, а не в точке между родителями: иначе дети
    // оказывались бы прямо в той же кучке и тоже спаривались бы мгновенно (лавина за секунды).
    std::vector<bool>    mated(n, false);
    std::vector<Creature> newborns;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
        if (mated[i] || freshOutputs[i].mateDesire <= kMateDesireThreshold || !m_population[i].creature.canMate()) continue;

        float       bestDist = std::numeric_limits<float>::max();
        std::size_t bestJ    = n;
        forEachNearby(
            creatureGrid, m_population[i].creature.position, kInteractRadius,
            [this](std::size_t j) { return m_population[j].creature.position; },
            [&](std::size_t j, float dist) {
                if (j == i || mated[j]) return;
                if (freshOutputs[j].mateDesire <= kMateDesireThreshold || !m_population[j].creature.canMate()) return;
                if (dist < bestDist) {
                    bestDist = dist;
                    bestJ    = j;
                }
            });
        if (bestJ == n) continue;

        Creature& a = m_population[i].creature;
        Creature& b = m_population[bestJ].creature;
        const float chance = kMateChancePerSec * 0.5f * (a.fertility() + b.fertility()) * dt;
        if (unit(m_rng) >= chance) continue;  // пока не сложилось -- подождут рядом

        a.mate(b.id);
        b.mate(a.id);
        a.matingCooldown = 45.0f + 40.0f * (1.0f - a.fertility());
        b.matingCooldown = 45.0f + 40.0f * (1.0f - b.fertility());
        a.needs.energy   = std::max(0.0f, a.needs.energy - kMotherEnergyCost);
        b.needs.energy   = std::max(0.0f, b.needs.energy - kFatherEnergyCost);
        mated[i]     = true;
        mated[bestJ] = true;
        ++m_stats.matings;

        if (m_population.size() + newborns.size() < kMaxPopulation) {
            Creature child = Creature::reproduce(a, b, m_rng);
            child.id       = m_nextId++;
            const float ang = 6.2831853f * unit(m_rng), r = 0.8f + 1.4f * unit(m_rng);
            child.position = a.position + glm::vec3(std::cos(ang) * r, 0.0f, std::sin(ang) * r);
            if (terrain && terrain->isUnderwater(child.position.x, child.position.z)) child.position = a.position;  // не рождаются в воде
            if (terrain) child.position.y = terrain->heightAt(child.position.x, child.position.z);
            a.recordChild(child.id);
            b.recordChild(child.id);
            newborns.push_back(std::move(child));
            ++m_stats.births;
        }
    }

    // 5. Смерть (Creature::tick() уже выставил alive=false: старость, голод, истощение) --
    // пингуем родню горем на следующий тик, затем убираем из популяции.
    for (const auto& inh : m_population) {
        if (inh.creature.alive) continue;
        const Creature& c    = inh.creature;
        switch (c.deathCause) {
            case 1: ++m_stats.deathsOld; break;
            case 2: ++m_stats.deathsStarved; break;
            case 3: ++m_stats.deathsExhausted; break;
            case 4: ++m_stats.deathsPoison; break;
            default: ++m_stats.deathsOther; break;
        }
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
        src.amount = std::min(kFoodCap, src.amount + kFoodRegrowRate * berryDef(src.kind).regrow * dt);
}

}  // namespace wsim
