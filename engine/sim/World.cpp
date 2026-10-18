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
constexpr float kPredatorSense       = 28.0f;   // хищника, идущего на охоту, существо замечает издалека; просто близкого -- с kPredatorNear
constexpr float kPredatorNear        = 12.0f;
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
constexpr float kFoodRegrowRate      = 0.055f;  // единиц/сек восстановления источника еды при полной скорости (см. ниже: растёт вместе с кустом)
constexpr float kBushCrowdPenalty    = 0.4f;    // каждый стоящий у куста вдвое-втрое снижает его привлекательность: едоки расходятся по кустам
constexpr float kFoodCap             = 1.0f;
constexpr float kObserveRadius       = 12.0f;   // на таком расстоянии замечают, что соседу стало плохо после ягод (и учатся)
constexpr float kWatchEatRadius      = 8.0f;    // ... и что сосед спокойно ест: незнакомые ягоды перестают пугать
constexpr float kServeRange          = 2.6f;    // подсунуть ягоды обидчику можно не дальше
constexpr float kFoodMinEdible       = 0.12f;   // меньше -- куст "пустой": его не видно как еду, пока не отрастёт (иначе все висят на огрызках)

using Grid = std::unordered_map<std::int64_t, std::vector<std::size_t>>;

std::int32_t cellIndex(float v) noexcept { return static_cast<std::int32_t>(std::floor(v / kBushCell)); }   // номер ячейки кустов по координате

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
        {
            const glm::vec3 p = randomPointInBounds(m_rng);
            m_food.push_back(FoodSource{p, kFoodCap, BerryKind::Raspberry, cellIndex(p.x), cellIndex(p.z), -1});
        }
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

void World::growWildlife(const Terrain& terrain) { m_fauna.populate(terrain, m_rng, kWorldHalfSize); }

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
    // Посаженный куст принадлежит ячейке по положению (спит и просыпается вместе с ней; процедурного номера у него нет).
    m_food.push_back(FoodSource{{x, 0.0f, z}, kFoodCap, kind, cellIndex(x), cellIndex(z), -1});
    return true;
}

bool World::plantNaturalBush(const Terrain& terrain, float x, float z) {
    if (terrain.isUnderwater(x, z)) return false;
    ensureBushesAt(terrain, x, z);
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

// ─── Потоковые ягоды ──────────────────────────────────────────────────────────
// Кусты -- чистая функция мира (Berries::proceduralBushes), а World держит материализованными только те ячейки, возле которых
// есть существа или камера. Ушла жизнь -- ячейка засыпает: из состояния помнится лишь отличие от нетронутой (съеденное,
// стёртое, посаженное), на побудке куст отрастает за прошедшее время. Так мир бесконечен, а цена -- только за "живую" часть.
namespace {

constexpr float kLoadRadiusCreature = 54.0f;    // вокруг существа кусты нужны с запасом на радиус зрения
constexpr float kLoadRadiusFocus    = 110.0f;   // вокруг камеры -- чтобы окрестность не была голой
constexpr float kKeepMargin         = 26.0f;    // ячейка засыпает, когда ушла от всех на радиус + запас (иначе на границе мигала бы)
constexpr float kStreamInterval     = 0.5f;
constexpr std::size_t kMaxLoadsPerPass = 80;

std::int64_t packCell(std::int32_t cx, std::int32_t cz) noexcept {
    return (static_cast<std::int64_t>(cx) << 32) | static_cast<std::uint32_t>(cz);
}
std::int32_t cellX(std::int64_t key) noexcept { return static_cast<std::int32_t>(key >> 32); }
std::int32_t cellZ(std::int64_t key) noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(key)); }

// Куст, лежавший без присмотра t секунд, отрастает: da/dt = r (0.25 + 0.75 a) -- то же уравнение, что в World::tick, в замкнутом виде.
float regrownAmount(float a0, float seconds, BerryKind kind) {
    const float k = 0.75f * kFoodRegrowRate * berryDef(kind).regrow;
    return std::min(kFoodCap, (a0 + 1.0f / 3.0f) * std::exp(k * seconds) - 1.0f / 3.0f);
}

}  // namespace

void World::loadCell(const Terrain& terrain, std::int32_t cx, std::int32_t cz) {
    const std::int64_t key = packCell(cx, cz);
    if (!m_loadedCells.insert(key).second) return;
    const auto rec = m_cellRecords.find(key);
    const double asleep = rec != m_cellRecords.end() ? m_time - rec->second.lastTime : 0.0;
    for (const BushSpec& spec : proceduralBushes(terrain, cx, cz)) {
        float amount = kFoodCap;
        if (rec != m_cellRecords.end())
            for (const auto& [local, a] : rec->second.deltas)
                if (local == spec.local) { amount = a; break; }
        if (amount < 0.0f) continue;   // стёрт игроком
        if (amount < kFoodCap) amount = regrownAmount(amount, static_cast<float>(asleep), spec.kind);
        if (m_food.size() >= kMaxFood) break;
        m_food.push_back(FoodSource{{spec.x, 0.0f, spec.z}, amount, spec.kind, cx, cz, static_cast<std::int16_t>(spec.local)});
    }
    if (rec != m_cellRecords.end()) {
        for (FoodSource f : rec->second.planted) {
            f.amount = regrownAmount(f.amount, static_cast<float>(asleep), f.kind);
            if (m_food.size() < kMaxFood) m_food.push_back(f);
        }
        m_cellRecords.erase(rec);
    }
}

void World::unloadCells(const Terrain& terrain, const std::vector<std::int64_t>& keys) {
    if (keys.empty()) return;
    const std::unordered_set<std::int64_t> going(keys.begin(), keys.end());
    std::unordered_map<std::int64_t, std::vector<std::size_t>> byCell;
    for (std::size_t i = 0; i < m_food.size(); ++i) {
        const std::int64_t k = packCell(m_food[i].cellX, m_food[i].cellZ);
        if (going.count(k)) byCell[k].push_back(i);
    }
    for (std::int64_t key : keys) {
        CellRecordData rec;
        rec.key      = key;
        rec.lastTime = m_time;
        const auto it = byCell.find(key);
        const std::int32_t cx = cellX(key), cz = cellZ(key);
        for (const BushSpec& spec : proceduralBushes(terrain, cx, cz)) {
            const FoodSource* found = nullptr;
            if (it != byCell.end())
                for (std::size_t i : it->second)
                    if (m_food[i].local == spec.local) { found = &m_food[i]; break; }
            if (!found) rec.deltas.emplace_back(static_cast<std::int16_t>(spec.local), -1.0f);           // стёрт
            else if (found->amount < kFoodCap - 1.0e-3f) rec.deltas.emplace_back(static_cast<std::int16_t>(spec.local), found->amount);
        }
        if (it != byCell.end())
            for (std::size_t i : it->second)
                if (m_food[i].local < 0) rec.planted.push_back(m_food[i]);
        if (!rec.deltas.empty() || !rec.planted.empty()) m_cellRecords[key] = std::move(rec);
        else m_cellRecords.erase(key);
        m_loadedCells.erase(key);
    }
    std::erase_if(m_food, [&](const FoodSource& f) { return going.count(packCell(f.cellX, f.cellZ)) != 0; });
}

void World::streamBushes(const Terrain& terrain, float dt) {
    m_streamTimer += dt;
    if (m_streamTimer < kStreamInterval) return;
    m_streamTimer = 0.0f;

    std::unordered_set<std::int64_t> want, keep;
    auto disc = [&](float x, float z, float radius, std::unordered_set<std::int64_t>& set) {
        const std::int32_t x0 = cellIndex(x - radius), x1 = cellIndex(x + radius), z0 = cellIndex(z - radius), z1 = cellIndex(z + radius);
        for (std::int32_t cz = z0; cz <= z1; ++cz)
            for (std::int32_t cx = x0; cx <= x1; ++cx) set.insert(packCell(cx, cz));
    };
    for (const Inhabitant& in : m_population)
        if (in.creature.alive) {
            disc(in.creature.position.x, in.creature.position.z, kLoadRadiusCreature, want);
            disc(in.creature.position.x, in.creature.position.z, kLoadRadiusCreature + kKeepMargin, keep);
        }
    for (const glm::vec2& f : m_focus) {
        disc(f.x, f.y, kLoadRadiusFocus, want);
        disc(f.x, f.y, kLoadRadiusFocus + kKeepMargin, keep);
    }
    std::size_t loads = 0;
    for (std::int64_t key : want) {
        if (m_loadedCells.count(key)) continue;
        loadCell(terrain, cellX(key), cellZ(key));
        if (++loads >= kMaxLoadsPerPass) break;
    }
    std::vector<std::int64_t> sleepers;
    for (std::int64_t key : m_loadedCells)
        if (!keep.count(key)) sleepers.push_back(key);
    std::sort(sleepers.begin(), sleepers.end());   // порядок не зависит от хэш-таблицы: результат воспроизводим
    unloadCells(terrain, sleepers);
}

void World::ensureBushesAt(const Terrain& terrain, float x, float z) {
    if (!m_streaming) return;
    for (std::int32_t dz = -1; dz <= 1; ++dz)
        for (std::int32_t dx = -1; dx <= 1; ++dx) loadCell(terrain, cellIndex(x) + dx, cellIndex(z) + dz);
}

bool World::bushNear(const Terrain& terrain, float x, float z, float radius) const {
    for (const FoodSource& f : m_food)
        if (f.local < 0 && std::hypot(f.position.x - x, f.position.z - z) < radius) return true;   // посаженные
    for (std::int32_t dz = -1; dz <= 1; ++dz)
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const std::int32_t cx = cellIndex(x) + dx, cz = cellIndex(z) + dz;
            const std::int64_t key = packCell(cx, cz);
            auto it = m_specCache.find(key);
            if (it == m_specCache.end()) {
                if (m_specCache.size() > 4096) m_specCache.clear();
                it = m_specCache.emplace(key, proceduralBushes(terrain, cx, cz)).first;
            }
            for (const BushSpec& b : it->second)
                if (std::hypot(b.x - x, b.z - z) < radius) return true;
        }
    return false;
}

void World::growBerryBushes(const Terrain& terrain) {
    m_streaming = true;
    m_food.clear();
    m_loadedCells.clear();
    m_cellRecords.clear();
    m_specCache.clear();
    m_time = 0.0;
    m_streamTimer = 100.0f;   // первая же проверка в tick() подгрузит окрестности существ
    const float reach = kWorldHalfSize + 14.0f;
    for (std::int32_t cz = cellIndex(-reach); cz <= cellIndex(reach); ++cz)
        for (std::int32_t cx = cellIndex(-reach); cx <= cellIndex(reach); ++cx) loadCell(terrain, cx, cz);
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
    st.fauna      = m_fauna.exportState();
    st.streaming  = m_streaming;
    st.time       = m_time;
    st.loadedCells.assign(m_loadedCells.begin(), m_loadedCells.end());
    std::sort(st.loadedCells.begin(), st.loadedCells.end());
    for (const auto& [key, rec] : m_cellRecords) st.cellRecords.push_back(rec);
    std::sort(st.cellRecords.begin(), st.cellRecords.end(), [](const CellRecordData& a, const CellRecordData& b) { return a.key < b.key; });
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
    if (!m_fauna.importState(st.fauna)) return false;
    m_streaming = st.streaming;
    m_time      = std::isfinite(st.time) ? st.time : 0.0;
    m_streamTimer = 100.0f;
    m_specCache.clear();
    m_loadedCells.clear();
    m_cellRecords.clear();
    if (st.streaming) {
        m_loadedCells.insert(st.loadedCells.begin(), st.loadedCells.end());
        for (const CellRecordData& rec : st.cellRecords) m_cellRecords[rec.key] = rec;
    } else if (!m_food.empty()) {
        // Старое сохранение (или мир из тестов): кусты были просто списком. Считаем их "посаженными" и объявляем стартовую
        // область загруженной -- процедурные кусты поверх них не рождаются; за её пределами мир подгружает кусты сам.
        m_streaming = true;
        for (FoodSource& f : m_food) { f.cellX = cellIndex(f.position.x); f.cellZ = cellIndex(f.position.z); f.local = -1; }
        const float reach = kWorldHalfSize + 14.0f;
        for (std::int32_t cz = cellIndex(-reach); cz <= cellIndex(reach); ++cz)
            for (std::int32_t cx = cellIndex(-reach); cx <= cellIndex(reach); ++cx) m_loadedCells.insert(packCell(cx, cz));
    }
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
    m_time += dt;
    if (terrain && m_streaming) streamBushes(*terrain, dt);

    // 1. Прошлотиковые пинги горя -- одноразовые, читаются один раз и всё.
    const std::unordered_map<std::uint64_t, float> grief = std::move(m_pendingGrief);
    m_pendingGrief.clear();

    const std::size_t n = m_population.size();

    // 2. id -> индекс (для поиска близких по родству) + пространственные сетки.
    std::unordered_map<std::uint64_t, std::size_t> idIndex;
    idIndex.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        idIndex[m_population[i].creature.id] = i;

    // Дикие животные: шаг раньше существ. Хищник мог укусить существо -- здоровье падает сразу, убитое помечаем (причина 5).
    {
        std::vector<CreatureView> views;
        views.reserve(n);
        for (const Inhabitant& in : m_population)
            if (in.creature.alive) views.push_back({in.creature.id, in.creature.position});
        std::vector<Bite> bites;
        m_fauna.tick(dt, terrain, views, m_rng, m_streaming ? 0.0f : kWorldHalfSize, bites, m_focus);
        for (const Bite& b : bites) {
            const auto it = idIndex.find(b.creatureId);
            if (it == idIndex.end()) continue;
            Creature& c = m_population[it->second].creature;
            if (!c.alive) continue;
            c.health -= b.damage;
            c.needs.fear = 1.0f;
            if (c.health <= 0.0f) { c.alive = false; c.deathCause = 5; }
        }
    }

    const Grid creatureGrid = buildGrid(n, [this](std::size_t i) { return m_population[i].creature.position; });

    std::vector<std::size_t> liveFoodIdx;
    liveFoodIdx.reserve(m_food.size());
    for (std::size_t i = 0; i < m_food.size(); ++i)
        if (m_food[i].amount >= kFoodMinEdible) liveFoodIdx.push_back(i);
    const Grid foodGrid = buildGrid(
        liveFoodIdx.size(), [this, &liveFoodIdx](std::size_t k) { return m_food[liveFoodIdx[k]].position; });

    // Сколько существ стоит у каждого съедобного куста (3.5 юнита): куда уже сбежалась толпа, туда остальные идут неохотно.
    std::vector<int> occupancy(liveFoodIdx.size(), 0);
    for (std::size_t k = 0; k < liveFoodIdx.size(); ++k)
        forEachNearby(
            creatureGrid, m_food[liveFoodIdx[k]].position, 3.5f,
            [this](std::size_t j) { return m_population[j].creature.position; },
            [&](std::size_t, float) { ++occupancy[k]; });

    // 3. Pass 1: сенсы -> мозг и поведение -> резолв eat/rest. Всё чтение чужого
    // состояния идёт из сеток/lastOutputs, построенных до этого цикла --
    // порядок обработки i не влияет на корректность (см. World.hpp).
    std::vector<BrainOutputs> freshOutputs(n);
    std::vector<std::uint8_t> ateLook(n, 255);  // что ели в этом тике (внешний вид; 255 -- ничего)
    std::vector<float>        ateValue(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        Creature&       self    = m_population[i].creature;
        if (!self.alive) continue;   // растерзанного в этом тике хищником дальше не тикаем -- в конце его уберут
        const glm::vec3 selfPos = self.position;

        SenseInputs senses{};
        senses.terrain       = terrain;
        senses.worldHalfSize = m_streaming ? 0.0f : kWorldHalfSize;   // с потоковым миром границ нет: бегать можно куда угодно

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
                // Толпа у куста отпугивает (сам существо у куста себя не считает).
                const float crowd = std::max(0.0f, static_cast<float>(occupancy[k]) - (dist < 3.5f ? 1.0f : 0.0f));
                const float score = std::max(appeal, 0.05f) / (1.0f + dist / kAppealDistScale) / (1.0f + kBushCrowdPenalty * crowd);
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

        // Хищники (дикие животные): на охоте видны с kPredatorSense, спокойные -- вблизи (kPredatorNear). Идут по тому же каналу
        // "угроза", что и валуны: страх, бегство, крик тревоги -- остальные пугаются уже по этому крику.
        for (const Animal& an : m_fauna.animals()) {
            if (!an.alive || !isPredator(an.species)) continue;
            const float dist = std::hypot(an.position.x - selfPos.x, an.position.z - selfPos.z);
            const float edge = std::max(dist - 1.0f, 0.0f);
            const float range = an.state == AnimalState::Hunt ? kPredatorSense : kPredatorNear;
            if (dist >= range || edge >= senses.threatDist) continue;
            senses.threatDir       = dirXZ(selfPos, an.position);
            senses.threatProximity = std::max(senses.threatProximity, std::clamp(1.0f - edge / range, 0.0f, 1.0f));
            senses.threatDist      = edge;
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
        float         bestDomVal = 0.0f;  // у кого самая заметная демонстрация статуса рядом -- кандидат на зуб
        std::uint64_t bestDomId  = 0;
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
                if (other.lastOutputs.dominanceDisplay > bestDomVal) { bestDomVal = other.lastOutputs.dominanceDisplay; bestDomId = other.creature.id; }
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

        // Зуб: завистливый (envy), которого копит обида (resentment) рядом с чужой демонстрацией статуса, берёт самого
        // заметного соседа на заметку. Зуб растёт, пока обида есть, и тихо проходит; на умершего -- сразу.
        if (self.envy() > 0.35f && self.needs.resentment > 0.3f && bestDomVal > 0.55f && bestDomId != 0 && (self.grudgeId == 0 || self.grudge < 0.3f))
            self.grudgeId = bestDomId;
        if (self.grudgeId != 0 && idIndex.find(self.grudgeId) == idIndex.end()) { self.grudgeId = 0; self.grudge = 0.0f; }
        if (self.grudgeId != 0) {
            if (self.grudgeId == bestDomId && self.needs.resentment > 0.3f) self.grudge = std::min(1.0f, self.grudge + self.needs.resentment * 0.05f * dt);
            else self.grudge = std::max(0.0f, self.grudge - 0.003f * dt);
            if (self.grudge <= 0.0f) self.grudgeId = 0;
        }
        std::size_t grudgeIdx = n, badBushK = liveFoodIdx.size();
        float       badBushDist = kSenseRadius;
        if (self.grudgeId != 0) {
            grudgeIdx = idIndex.find(self.grudgeId)->second;
            const float d = glm::length(m_population[grudgeIdx].creature.position - selfPos);
            if (d <= kSenseRadius) { senses.grudgeDist = d; senses.grudgePos = m_population[grudgeIdx].creature.position; }
            // Готовится подсунуть: ищет куст, чей внешний вид знает как плохой (по мнению, а не по растению: может оказаться и безвредным).
            if (self.grudge > 0.5f && self.carryAmount <= 0.0f)
                forEachNearby(
                    foodGrid, selfPos, kSenseRadius,
                    [this, &liveFoodIdx](std::size_t k) { return m_food[liveFoodIdx[k]].position; },
                    [&](std::size_t k, float dist) {
                        const BerryLook look = lookOf(m_food[liveFoodIdx[k]].kind);
                        if (self.knowsLook(look) && self.taste[static_cast<std::size_t>(look)] < -0.3f && dist < badBushDist) {
                            badBushDist = dist;
                            badBushK    = k;
                        }
                    });
            if (badBushK != liveFoodIdx.size()) { senses.badFoodDist = badBushDist; senses.badFoodPos = m_food[liveFoodIdx[badBushK]].position; }
        }

        const BrainOutputs out = self.tick(dt, senses);
        freshOutputs[i]        = out;

        // Месть: взять ягоды у плохого куста / подсунуть обидчику (решение принял слой поведения, исполняет мир).
        if (self.pickRequest && badBushK != liveFoodIdx.size() && badBushDist <= kInteractRadius) {
            FoodSource& bush = m_food[liveFoodIdx[badBushK]];
            const float take = std::min(bush.amount, 0.12f);
            if (take > 0.04f) {
                bush.amount -= take;
                self.carryKind   = static_cast<std::uint8_t>(bush.kind);
                self.carryAmount = take;
                self.carryAge    = 0.0f;
            }
        }
        if (self.serveRequest && self.carryAmount > 0.0f && grudgeIdx != n &&
            glm::length(m_population[grudgeIdx].creature.position - self.position) <= kServeRange) {
            Creature&       victim = m_population[grudgeIdx].creature;
            const BerryKind kind   = static_cast<BerryKind>(self.carryKind);
            const BerryDef& def    = berryDef(kind);
            victim.ingestBerry(def, self.carryAmount);                       // "угощение": доза как за один присест
            victim.tasteBerry(kind, def.nutrition, def.harm(), 0.3f);         // и жертва запомнила вкус
            self.carryAmount = 0.0f;
            self.carryKind   = 255;
            self.grudge      = std::max(0.0f, self.grudge - 0.6f);            // отомстил -- полегчало
            self.needs.resentment = std::max(0.0f, self.needs.resentment - 0.6f);
            ++m_stats.sabotages;
        }

        ateLook[i] = 255;
        if (out.eatDesire > kEatDesireThreshold && foundFood && bestFoodDist <= kInteractRadius) {
            FoodSource& src  = m_food[liveFoodIdx[bestFoodK]];
            const float bite = std::min(src.amount, kEatRate * dt);
            const BerryDef& def = berryDef(src.kind);
            self.feed(bite * def.nutrition);  // морошка сытнее черники при том же объёме
            self.ingestBerry(def, bite);                                  // яд/понос/тошнота/галлюцинации по профилю вида
            self.tasteBerry(src.kind, def.nutrition, def.harm(), dt);     // вкус запоминается, пока ест
            ateLook[i] = static_cast<std::uint8_t>(lookOf(src.kind));
            ateValue[i] = def.nutrition;
            src.amount -= bite;
        }
        if (out.restDesire > kRestDesireThreshold) self.rest(kRestRate * dt);
    }
    for (std::size_t i = 0; i < n; ++i)
        m_population[i].lastOutputs = freshOutputs[i];

    // 3b. Социальное обучение: соседи учатся на том, что видят. Если существу стало плохо вскоре после ягод, видевшие
    // трапезу запоминают этот ВИД ягод как плохой (свои и общительные -- убедительнее; чужим веришь меньше); если спокойно
    // ест -- незнакомые ягоды для наблюдателей чуть менее подозрительны. Читает и сбрасывает sickEvent.
    for (std::size_t i = 0; i < n; ++i) {
        Creature& e = m_population[i].creature;
        const bool sick = e.sickEvent != 0;
        e.sickEvent = 0;
        const bool badMeal = sick && e.mealLook != 255 && e.sinceBite < 25.0f;
        if (!badMeal && ateLook[i] == 255) continue;
        const auto look = static_cast<BerryLook>(badMeal ? e.mealLook : ateLook[i]);
        forEachNearby(
            creatureGrid, e.position, badMeal ? kObserveRadius : kWatchEatRadius,
            [this](std::size_t j) { return m_population[j].creature.position; },
            [&](std::size_t j, float dist) {
                if (j == i) return;
                Creature& o = m_population[j].creature;
                if (!o.alive) return;
                const float radius = badMeal ? kObserveRadius : kWatchEatRadius;
                const float w = (isFamiliar(o, e.id) ? 1.0f : 0.35f) * (0.5f + o.sociability()) / 1.5f * (1.0f - 0.5f * dist / radius);
                if (badMeal) o.observeLook(look, -1.0f, 0.9f * w);
                else if (!o.knowsLook(look) || (o.taste[static_cast<std::size_t>(look)] > 0.0f && o.tasteSamples[static_cast<std::size_t>(look)] < 150))
                    o.observeLook(look, 0.5f + 0.5f * ateValue[i], 0.06f * w);  // по тику: за пару секунд -- заметно; плохое мнение не переубеждает
            });
    }

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
            case 5: ++m_stats.deathsPredation; break;
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
    // Куст отрастает не равномерно: объеденный до нуля долго "лысый", чем больше ягод -- тем быстрее набирает новые (рост
    // 25% -> 100% от полной скорости). Раньше крошки отрастали сразу и толпа часами ждала возле пустого куста.
    for (auto& src : m_food)
        src.amount = std::min(kFoodCap, src.amount + kFoodRegrowRate * berryDef(src.kind).regrow * (0.25f + 0.75f * src.amount) * dt);
}

}  // namespace wsim
