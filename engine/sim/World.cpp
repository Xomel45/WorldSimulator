#include "sim/World.hpp"

#include "sim/Trees.hpp"

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
constexpr float kFoodRegrowRate      = 0.075f;  // единиц/сек восстановления источника еды при полной скорости (см. ниже: растёт вместе с кустом)
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

float World::sunElevation(double time) noexcept {
    return std::sin(6.28318531f * static_cast<float>(std::fmod(time / kDayLength + 0.08, 1.0)));
}
float World::yearPhase(double time) noexcept { return static_cast<float>(std::fmod(time / kYearLength, 1.0)); }
int World::season(double time) noexcept {
    const float p = yearPhase(time);
    return p < 0.2f ? 0 : (p < 0.5f ? 1 : (p < 0.7f ? 2 : 3));
}
const char* World::seasonName(int s) noexcept {
    static constexpr const char* kNames[] = {"весна", "лето", "осень", "зима"};
    return kNames[s & 3];
}
float World::coldness(double time) noexcept {
    const float c = 0.5f + 0.5f * std::cos(6.28318531f * (yearPhase(time) - 0.8f));   // пик зимы на 0.8 года
    const float t = std::clamp((c - 0.55f) / 0.35f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float World::foodFactor(double time) noexcept {
    const float summer = std::max(0.0f, std::cos(6.28318531f * (yearPhase(time) - 0.35f)));
    return 1.0f + 0.3f * summer - 0.4f * coldness(time);
}
float World::rain(double time) noexcept {
    if (time < 200.0) return 0.0f;
    // Гладкий шум по окнам ~90 с: значения в узлах -- хеш номера окна; дождь идёт, пока шум выше порога.
    auto h = [](std::int64_t i) { std::uint64_t x = static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull; x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29; return static_cast<float>(x & 0xFFFFFF) / 16777216.0f; };
    const double u = time / 90.0;
    const auto   i = static_cast<std::int64_t>(std::floor(u));
    const float  f = static_cast<float>(u - static_cast<double>(i)), sm = f * f * (3.0f - 2.0f * f);
    const float  n = h(i) * (1.0f - sm) + h(i + 1) * sm;
    const float  t = std::clamp((n - 0.62f) / 0.2f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float World::nightness(double time) noexcept {
    const float e = sunElevation(time);
    const float t = std::clamp((-0.1f - e) / 0.3f, 0.0f, 1.0f);   // от -0.1 до -0.4 сгущается
    return t * t * (3.0f - 2.0f * t);
}

void World::setRelation(int a, int b, float v) noexcept {
    if (a <= 0 || b <= 0 || a >= kMaxTribes || b >= kMaxTribes || a == b) return;
    m_relations[a][b] = m_relations[b][a] = std::clamp(v, -1.0f, 1.0f);
}

std::size_t World::tribeCount(int tribe) const noexcept {
    std::size_t n = 0;
    for (const Inhabitant& in : m_population) n += in.creature.alive && in.creature.tribe == tribe;
    return n;
}

void World::electLeaders() {
    for (int tr = 1; tr < kMaxTribes; ++tr) {
        std::uint64_t best = 0;
        float bestScore = -1.0f;
        for (const Inhabitant& in : m_population) {
            const Creature& c = in.creature;
            if (!c.alive || c.tribe != tr || !c.isMature()) continue;
            const float f = c.age / std::max(c.lifespan(), 1.0f);
            const float maturity = std::clamp(1.0f - std::fabs(f - 0.4f) * 2.0f, 0.0f, 1.0f);
            const float kids = std::min(static_cast<float>(c.childIds.size()), 4.0f) / 4.0f;
            float score = 0.6f * maturity + 0.3f * c.boldness() + 0.3f * c.sociability() + 0.15f * kids + 0.2f * c.needs.respect + 0.2f * c.health;
            if (c.id == m_leaders[tr]) score += 0.15f;   // действующему вождю -- бонус
            if (score > bestScore) { bestScore = score; best = c.id; }
        }
        if (best != m_leaders[tr]) { m_leaders[tr] = best; ++m_stats.leaderChanges; }
    }
}

void World::formTribes(const Terrain& terrain, int count) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    count = std::clamp(count, 1, kMaxTribes - 1);
    // Центры лагерей: сушa, как можно дальше друг от друга (лучший из 60 случайных кандидатов по расстоянию до уже выбранных).
    const float reach = (m_streaming ? 90.0f : kWorldHalfSize - 20.0f);
    std::vector<glm::vec2> centres;
    for (int t = 0; t < count; ++t) {
        glm::vec2 best{0.0f};
        float bestScore = -1.0f;
        for (int k = 0; k < 60; ++k) {
            const glm::vec2 p{(unit(m_rng) * 2.0f - 1.0f) * reach, (unit(m_rng) * 2.0f - 1.0f) * reach};
            if (terrain.heightAt(p.x, p.y) < 1.0f || terrain.heightAt(p.x + 8.0f, p.y) < 0.8f || terrain.heightAt(p.x - 8.0f, p.y) < 0.8f) continue;
            float score = 1.0e6f;
            for (const glm::vec2& c : centres) score = std::min(score, glm::length(p - c));
            if (score > bestScore) { bestScore = score; best = p; }
        }
        centres.push_back(best);
    }
    m_structures.erase(std::remove_if(m_structures.begin(), m_structures.end(), [](const Structure& s) { return s.kind == StructureKind::Stockpile; }), m_structures.end());
    for (std::size_t i = 0; i < m_population.size(); ++i) {
        Creature& c = m_population[i].creature;
        const int tribe = 1 + static_cast<int>(i % static_cast<std::size_t>(count));
        const glm::vec2 centre = centres[static_cast<std::size_t>(tribe - 1)];
        glm::vec2 p = centre;
        for (int attempt = 0; attempt < 12; ++attempt) {
            const float ang = 6.2831853f * unit(m_rng), r = 2.0f + 8.0f * std::sqrt(unit(m_rng));
            const glm::vec2 q = centre + glm::vec2{std::cos(ang), std::sin(ang)} * r;
            if (!terrain.isUnderwater(q.x, q.y)) { p = q; break; }
        }
        c.tribe = static_cast<std::uint8_t>(tribe);
        c.position = {p.x, terrain.heightAt(p.x, p.y), p.y};
        c.homePosition = c.position;
        m_population[i].lastOutputs = {};
    }
    for (int t = 1; t <= count; ++t) {
        const glm::vec2 c = centres[static_cast<std::size_t>(t - 1)];
        if (placeStructure(&terrain, StructureKind::Stockpile, c.x + 1.5f, c.y + 1.5f, 0.0f)) {
            m_structures.back().owner = static_cast<std::uint64_t>(t);
            m_structures.back().stock = 12.0f;
        }
    }
    for (int a = 1; a <= count; ++a)
        for (int b = a + 1; b <= count; ++b) setRelation(a, b, -0.6f + 1.0f * unit(m_rng));   // от вражды до тёплых отношений
}

bool World::placeStructure(const Terrain* terrain, StructureKind kind, float x, float z, float yaw) {
    if (m_structures.size() >= kMaxStructures || !std::isfinite(x) || !std::isfinite(z)) return false;
    if (terrain && terrain->isUnderwater(x, z)) return false;
    int near = 0;
    for (const Structure& s : m_structures) {
        const float d = std::hypot(s.x - x, s.z - z);
        if (d < 1.6f) return false;
        if (kind == StructureKind::Stockpile && s.kind == StructureKind::Stockpile && d < 25.0f) return false;   // один склад на стойбище
        if (kind == StructureKind::Hut && s.kind == StructureKind::Hut && d < 6.0f) return false;
        if (kind == StructureKind::Campfire && s.kind == StructureKind::Campfire && d < 10.0f) return false;
        near += d < 8.0f && s.kind == kind;
    }
    if (near >= 3) return false;
    m_structures.push_back({kind, x, z, yaw, 1.0f});
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
float regrownAmount(float a0, float seconds, BerryKind kind, float rateMul = 1.0f) {
    const float k = 0.75f * kFoodRegrowRate * berryDef(kind).regrow * rateMul;
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
        if (amount < kFoodCap) amount = regrownAmount(amount, static_cast<float>(asleep), spec.kind, foodFactor(m_time));
        if (m_food.size() >= kMaxFood) break;
        m_food.push_back(FoodSource{{spec.x, 0.0f, spec.z}, amount, spec.kind, cx, cz, static_cast<std::int16_t>(spec.local)});
    }
    if (rec != m_cellRecords.end()) {
        for (FoodSource f : rec->second.planted) {
            f.amount = regrownAmount(f.amount, static_cast<float>(asleep), f.kind, foodFactor(m_time));
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
    st.structures = m_structures;
    for (int tr = 0; tr < kMaxTribes; ++tr) st.leaders[static_cast<std::size_t>(tr)] = m_leaders[tr];
    for (int a = 0; a < kMaxTribes; ++a) for (int b = 0; b < kMaxTribes; ++b) st.relations[static_cast<std::size_t>(a * kMaxTribes + b)] = m_relations[a][b];
    st.streaming  = m_streaming;
    st.time       = m_time;
    st.streamTimer = m_streamTimer;
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
    m_structures = st.structures;
    for (int tr = 0; tr < kMaxTribes; ++tr) m_leaders[tr] = st.leaders[static_cast<std::size_t>(tr)];
    m_electionSlot = static_cast<std::int64_t>(std::floor((std::isfinite(st.time) ? st.time : 0.0) / kElectionSeconds));
    for (int a = 0; a < kMaxTribes; ++a) for (int b = 0; b < kMaxTribes; ++b) { const float v = st.relations[static_cast<std::size_t>(a * kMaxTribes + b)]; m_relations[a][b] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f; }
    if (!m_fauna.importState(st.fauna)) return false;
    m_streaming = st.streaming;
    m_time      = std::isfinite(st.time) ? st.time : 0.0;
    m_streamTimer = std::isfinite(st.streamTimer) ? std::clamp(st.streamTimer, 0.0f, 100.0f) : 100.0f;
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

void World::tick(float dt, Terrain* mutableTerrain) {
    const Terrain* terrain = mutableTerrain;
    m_time += dt;
    if (terrain && m_streaming) streamBushes(*terrain, dt);
    for (int a = 1; a < kMaxTribes; ++a)
        for (int b = a + 1; b < kMaxTribes; ++b) setRelation(a, b, m_relations[a][b] * (1.0f - 0.0003f * dt));   // со временем обиды стихают
    for (int a = 1; a < kMaxTribes; ++a)
        for (int b = a + 1; b < kMaxTribes; ++b) {
            const bool was = m_allied[a][b];
            const bool now = was ? m_relations[a][b] > 0.3f : m_relations[a][b] > 0.6f;
            if (now != was) { m_allied[a][b] = m_allied[b][a] = now; ++(now ? m_stats.alliancesFormed : m_stats.alliancesBroken); }
        }
    // Выборы вождей по слотам мирового времени (производны от времени -- сохранять номер слота не нужно).
    {
        const auto slot = static_cast<std::int64_t>(std::floor(m_time / kElectionSeconds));
        if (slot != m_electionSlot) { m_electionSlot = slot; electLeaders(); }
    }
    // Костры сгорают (hp -- топливо); догоревшие убираем.
    const float coldNow = terrain ? coldness(m_time) : 0.0f, rainNow = terrain ? rain(m_time) : 0.0f;
    for (Structure& st : m_structures)
        if (st.kind == StructureKind::Campfire) st.hp = std::max(0.0f, st.hp - dt * (1.0f + 2.5f * rainNow) / kFireBurnSeconds);   // дождь гасит быстрее
    std::erase_if(m_structures, [](const Structure& st) { return st.kind == StructureKind::Campfire && st.hp <= 0.0f; });

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
        m_fauna.tick(dt, terrain, views, m_rng, m_streaming ? 0.0f : kWorldHalfSize, bites, m_focus, &m_structures, terrain ? nightness(m_time) : 0.0f);
        for (const Bite& b : bites) {
            const auto it = idIndex.find(b.creatureId);
            if (it == idIndex.end()) continue;
            Creature& c = m_population[it->second].creature;
            if (!c.alive) continue;
            float damage = b.damage;
            for (const Structure& st : m_structures)   // у хижины стены: укус вчетверо слабее
                if (st.kind == StructureKind::Hut && std::hypot(st.x - c.position.x, st.z - c.position.z) < 3.2f) { damage *= 0.25f; ++m_stats.bitesBlockedByWalls; break; }
            if (c.hasShield()) { damage *= 0.35f; c.shieldDur = std::max(0.0f, c.shieldDur - 0.34f); ++m_stats.shieldBlocks; }
            c.health -= damage;
            pushEvent(EventType::Bitten, c.position);
            c.needs.fear = 1.0f;
            c.dangerMemory = 120.0f;
            if (c.hasSpear() && b.attackerId != 0) {   // копьё: укушенный бьёт в ответ, хищник ранен и отступает
                m_fauna.hurt(b.attackerId, 0.5f);
                pushEvent(EventType::Stab, c.position);
                c.spearDur = std::max(0.0f, c.spearDur - 0.25f);
                ++m_stats.spearHits;
            }
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
            senses.predatorSeen    = true;
        }

        // Склад: ближайший в 60 единицах.
        std::size_t stockIdx = m_structures.size();
        {
            float best = 60.0f;
            for (std::size_t k = 0; k < m_structures.size(); ++k) {
                const Structure& st = m_structures[k];
                if (st.kind != StructureKind::Stockpile) continue;
                if (!pileOpenTo(st, self.tribe)) continue;   // чужой (недружественный) склад своим не считается
                const float d = std::hypot(st.x - selfPos.x, st.z - selfPos.z);
                if (d < best) { best = d; stockIdx = k; }
            }
            if (stockIdx != m_structures.size()) {
                senses.stockDist = best;
                senses.stockPos = {m_structures[stockIdx].x, selfPos.y, m_structures[stockIdx].z};
                senses.stockAmount = m_structures[stockIdx].stock;
            }
        }
        // Племена: склад враждебного племени (куда идти грабить) и чужак у своего лагеря (кого гнать).
        std::size_t rivalIdx = m_structures.size();
        if (self.tribe != 0) {
            float best = 110.0f;
            for (std::size_t k = 0; k < m_structures.size(); ++k) {
                const Structure& st = m_structures[k];
                if (st.kind != StructureKind::Stockpile || st.owner == 0 || st.owner == self.tribe || st.owner >= kMaxTribes || st.stock < 1.0f) continue;
                if (m_relations[self.tribe][st.owner] > -0.15f) continue;
                const float d = std::hypot(st.x - selfPos.x, st.z - selfPos.z);
                if (d < best) { best = d; rivalIdx = k; }
            }
            if (rivalIdx != m_structures.size()) { senses.rivalStockDist = best; senses.rivalStockPos = {m_structures[rivalIdx].x, selfPos.y, m_structures[rivalIdx].z}; }
            if (stockIdx != m_structures.size() && senses.stockDist < 25.0f) {   // у своего лагеря: чужак из нетёплого племени гонится прочь
                float bestI = 12.0f;
                forEachNearby(
                    creatureGrid, selfPos, 12.0f, [this](std::size_t j) { return m_population[j].creature.position; },
                    [&](std::size_t j, float dist) {
                        const Creature& o = m_population[j].creature;
                        if (j == i || !o.alive || o.tribe == 0 || o.tribe == self.tribe || m_relations[self.tribe][o.tribe] > -0.1f || dist >= bestI) return;
                        bestI = dist;
                        senses.intruderDist = dist;
                        senses.intruderPos = o.position;
                        senses.intruderId = o.id;
                    });
            }
        }
        // Вождь своего племени.
        if (self.tribe != 0 && m_leaders[self.tribe] != 0 && m_leaders[self.tribe] != self.id) {
            const auto lit = idIndex.find(m_leaders[self.tribe]);
            if (lit != idIndex.end() && m_population[lit->second].creature.alive) {
                const Creature& lead = m_population[lit->second].creature;
                senses.leaderDist = std::hypot(lead.position.x - selfPos.x, lead.position.z - selfPos.z);
                senses.leaderPos  = lead.position;
            }
        }
        // Ночь и хижина: ближайшая в 40 единицах.
        if (terrain) senses.night = nightness(m_time);
        std::size_t hutIdx = m_structures.size(), fireIdx = m_structures.size();
        {
            // Хижина: своя/семьи предпочтительнее (чужая считается на 30 единиц дальше); костёр -- ближайший горящий.
            float bestScore = 70.0f, bestFire = 40.0f;
            for (std::size_t k = 0; k < m_structures.size(); ++k) {
                const Structure& st = m_structures[k];
                const float d = std::hypot(st.x - selfPos.x, st.z - selfPos.z);
                if (st.kind == StructureKind::Hut && d < 40.0f) {
                    const bool mine = st.owner == 0 || st.owner == self.id || st.owner == self.bondedPartnerId || st.owner == self.parentIds[0] || st.owner == self.parentIds[1] ||
                                      std::find(self.childIds.begin(), self.childIds.end(), st.owner) != self.childIds.end();
                    const float score = d + (mine ? 0.0f : 30.0f);
                    if (score < bestScore) { bestScore = score; hutIdx = k; }
                } else if (st.kind == StructureKind::Campfire && st.hp > 0.05f && d < bestFire) { bestFire = d; fireIdx = k; }
            }
            if (hutIdx != m_structures.size()) {
                const Structure& h = m_structures[hutIdx];
                senses.hutDist = std::hypot(h.x - selfPos.x, h.z - selfPos.z);
                senses.hutPos = {h.x, selfPos.y, h.z};
            }
            if (fireIdx != m_structures.size()) { senses.fireDist = bestFire; senses.firePos = {m_structures[fireIdx].x, selfPos.y, m_structures[fireIdx].z}; }
        }
        // Зимний холод и дождь; у костра и в хижине холод ослаблен.
        if (terrain) {
            senses.rain = rainNow;
            const bool warm = (hutIdx != m_structures.size() && senses.hutDist < 3.2f) || (fireIdx != m_structures.size() && senses.fireDist < 6.0f);
            senses.cold = coldNow * (warm ? 0.2f : 1.0f);
            if (senses.cold > 0.5f) ++m_stats.coldTicks;
        }
        // Вода: берег ищем лучами (редко, кэш в существе), когда хочется пить или пополнить ведро.
        if (terrain && (self.thirst > 0.4f || self.behavior == Behavior::Drink || (self.hasBucket && self.water < 1.5f && self.thirst > 0.25f))) {
            self.waterSearch -= dt;
            if (!self.waterKnown && self.waterSearch <= 0.0f) {   // цель держим, пока не дойдёт: иначе метается между берегами
                self.waterSearch = 2.0f;
                self.waterKnown = false;
                float best = 45.0f;
                for (int a = 0; a < 16; ++a) {
                    const float ang = static_cast<float>(a) * 0.3927f, cx = std::cos(ang), cz = std::sin(ang);
                    for (float r = 2.0f; r <= 44.0f && r < best; r += 2.0f)
                        if (terrain->isUnderwater(selfPos.x + cx * r, selfPos.z + cz * r)) {
                            best = r;
                            self.waterX = selfPos.x + cx * (r - 1.5f);
                            self.waterZ = selfPos.z + cz * (r - 1.5f);
                            self.waterKnown = true;
                            break;
                        }
                }
            }
            if (self.waterKnown) {
                const float d = std::hypot(self.waterX - selfPos.x, self.waterZ - selfPos.z);
                if (d > 60.0f) self.waterKnown = false;
                else { senses.waterDist = d; senses.waterPos = {self.waterX, selfPos.y, self.waterZ}; }
            }
        }
        else self.waterKnown = false;
        // Добыча для копья: ближайшее травоядное в 22 единицах.
        if (self.hasSpear() && self.needs.hunger > 0.4f) {
            float best = 22.0f;
            for (const Animal& an : m_fauna.animals()) {
                if (!an.alive || speciesDef(an.species).diet != Diet::Herbivore) continue;
                const float d = std::hypot(an.position.x - selfPos.x, an.position.z - selfPos.z);
                if (d < best) { best = d; senses.preyDist = d; senses.preyPos = an.position; senses.preyId = an.id; }
            }
        }

        // Дерево для рубки: ищем (редко, результат кэшируется в существе), только пока оно нужно.
        if (terrain && (self.wantsWood() || (self.knows(kSkillHut) && hutIdx == m_structures.size() && self.wood < kHutWood) || (self.knows(kSkillFire) && fireIdx == m_structures.size() && senses.night > 0.1f && self.wood < kFireWood) || (self.knows(kSkillStockpile) && stockIdx != m_structures.size() && m_structures[stockIdx].stock < 24.0f && self.wood < kMaxWood))) {
            if (self.treeKnown && terrain->treeFelled(self.treeCx, self.treeCz)) self.treeKnown = false;   // срубил кто-то другой
            self.treeSearch -= dt;
            if (!self.treeKnown && self.treeSearch <= 0.0f) {
                self.treeSearch = 1.5f;
                TreeInstance tree;
                if (Trees::nearest(*terrain, selfPos.x, selfPos.z, 34.0f, tree)) {
                    self.treeKnown = true;
                    self.treeCx = tree.cx;
                    self.treeCz = tree.cz;
                    self.treeX = tree.x;
                    self.treeZ = tree.z;
                }
            }
            if (self.treeKnown) {
                const float d = std::hypot(self.treeX - selfPos.x, self.treeZ - selfPos.z);
                if (d > 45.0f) self.treeKnown = false;
                else { senses.treeDist = std::max(d - 0.3f, 0.0f); senses.treePos = {self.treeX, selfPos.y, self.treeZ}; }
            }
        } else {
            self.treeKnown = false;
        }

        // Соседи: толпа своих/чужих, агрегированные сигналы (прошлый тик), ближайший ГОТОВЫЙ
        // партнёр, паникующие соседи, центр группы, расталкивание.
        int         contact[kMaxTribes]{};
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
                if (self.tribe != 0 && other.creature.tribe != 0 && other.creature.tribe != self.tribe && dist < 6.0f) ++contact[other.creature.tribe];
                if (isFamiliar(self, other.creature.id) || (self.tribe != 0 && other.creature.tribe == self.tribe)) {
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
        // Контакт племён: мирная встреча рядом сближает (если нет открытой вражды), встреча врагов лишь накаляет.
        if (self.tribe != 0)
            for (int tb = 1; tb < kMaxTribes; ++tb)
                if (contact[tb] > 0 && tb != self.tribe) {
                    const float rel = m_relations[self.tribe][tb];
                    setRelation(self.tribe, tb, rel + (rel > -0.3f ? 0.0004f : -0.0002f) * static_cast<float>(std::min(contact[tb], 3)) * dt);
                }
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

        const bool hadSpear = self.hasSpear(), hadShield = self.hasShield(), hadBucket = self.hasBucket;
        const BrainOutputs out = self.tick(dt, senses);
        freshOutputs[i]        = out;
        if (!hadSpear && self.hasSpear()) ++m_stats.spearsMade;
        if (!hadShield && self.hasShield()) ++m_stats.shieldsMade;
        if (!hadBucket && self.hasBucket) ++m_stats.bucketsMade;

        // Охота: удар копьём по добыче рядом; убитое кормит.
        if (self.huntRequest && senses.preyId != 0 && senses.preyDist <= 2.4f && self.huntTimer <= 0.0f) {
            self.huntTimer = 1.2f;
            self.spearDur  = std::max(0.0f, self.spearDur - 0.06f);
            pushEvent(EventType::Stab, senses.preyPos);
            if (m_fauna.hurt(senses.preyId, 0.55f)) { self.feed(0.7f); ++m_stats.meatKills; }
        }
        // Костёр: тёплый сон рядом (энергия восстанавливается быстрее); знающий с брёвнами подкладывает, когда огонь слабеет.
        if (fireIdx != m_structures.size()) {
            Structure& fire = m_structures[fireIdx];
            if (senses.fireDist < 5.0f && self.behavior == Behavior::Sleep) self.rest(0.03f * dt);
            if (senses.fireDist < 3.0f && self.knows(kSkillFire) && self.wood >= 1.0f && fire.hp < 0.35f && senses.night > 0.1f) {
                self.wood -= 1.0f;
                fire.hp = std::min(1.0f, fire.hp + 0.5f);
                ++m_stats.firesStoked;
            }
        }
        // Грабёж: у склада враждебного племени берёт брёвна (до двух), отношения портятся.
        if (self.raidRequest && rivalIdx != m_structures.size() && senses.rivalStockDist <= 2.8f) {
            Structure& st = m_structures[rivalIdx];
            const float take = std::min({2.0f, std::floor(st.stock), kMaxWood - self.wood});
            if (take >= 1.0f) {
                st.stock -= take;
                self.wood += take;
                ++m_stats.raids;
                m_stats.logsStolen += static_cast<std::uint64_t>(take);
                setRelation(self.tribe, static_cast<int>(st.owner), m_relations[self.tribe][st.owner] - 0.15f);
                pushEvent(EventType::Raid, senses.rivalStockPos);
            }
        }
        // Удар по чужаку (защита лагеря): урон 0.12 (копьё вдвое), щит гасит, раненый пугается; смерть -- deathCause 7.
        if (self.strikeRequest && senses.intruderId != 0 && senses.intruderDist <= 2.4f && self.huntTimer <= 0.0f) {
            const auto it = idIndex.find(senses.intruderId);
            if (it != idIndex.end()) {
                Creature& victim = m_population[it->second].creature;
                float dmg = 0.12f * (self.hasSpear() ? 2.0f : 1.0f);
                if (victim.hasShield()) { dmg *= 0.4f; victim.shieldDur = std::max(0.0f, victim.shieldDur - 0.2f); }
                victim.health -= dmg;
                victim.needs.fear = std::max(victim.needs.fear, 0.75f);
                self.huntTimer = 1.0f;
                ++m_stats.strikes;
                setRelation(self.tribe, victim.tribe, m_relations[self.tribe][victim.tribe] - 0.04f);
                pushEvent(EventType::Strike, victim.position);
                if (victim.health <= 0.0f && victim.alive) { victim.alive = false; victim.deathCause = 7; }
            }
        }
        // Склад: сдать излишки / взять нужное.
        if (self.stockRequest != 0 && stockIdx != m_structures.size() && senses.stockDist <= 2.8f) {
            Structure& st = m_structures[stockIdx];
            if (self.stockRequest == 1) {
                const float give = std::min(std::floor(self.wood - self.woodGoal()), kStockpileCapacity - st.stock);
                if (give >= 1.0f) {
                    self.wood -= give; st.stock += give; m_stats.logsStored += static_cast<std::uint64_t>(give);
                    if (st.owner != 0 && st.owner != self.tribe && self.tribe != 0) {   // дар чужому (дружественному) складу
                        setRelation(self.tribe, static_cast<int>(st.owner), m_relations[self.tribe][st.owner] + 0.06f * give);
                        m_stats.giftLogs += static_cast<std::uint64_t>(give);
                        pushEvent(EventType::Gift, senses.stockPos);
                    }
                }
            } else {
                const float take = std::min({std::floor(st.stock), std::ceil(self.woodGoal() - self.wood), kMaxWood - self.wood});
                if (take >= 1.0f) {
                    self.wood += take; st.stock -= take; m_stats.logsTaken += static_cast<std::uint64_t>(take);
                    if (st.owner != 0 && st.owner != self.tribe && self.tribe != 0) {   // обмен: берёт у друзей, им это не бесплатно
                        setRelation(self.tribe, static_cast<int>(st.owner), m_relations[self.tribe][st.owner] - 0.03f * take);
                        m_stats.tradeLogs += static_cast<std::uint64_t>(take);
                    }
                }
            }
        }

        // Рубка: пока стоит у дерева, набегает прогресс; срубленное исчезает (Terrain::fellTree), брёвна -- в руки.
        if (self.chopRequest && self.treeKnown && mutableTerrain && senses.treeDist <= 2.2f) {
            self.chopProgress += dt / 3.0f;
            if (self.chopProgress >= 1.0f) {
                TreeInstance tree;
                float yield = 1.0f;
                if (Trees::nearest(*terrain, self.treeX, self.treeZ, 0.6f, tree)) yield = std::clamp(std::round(tree.scale * 1.6f), 1.0f, 3.0f);
                self.wood = std::min(kMaxWood, self.wood + yield);
                self.chopProgress = 0.0f;
                self.treeKnown = false;
                m_dirty.push_back(mutableTerrain->fellTree(self.treeCx, self.treeCz));
                ++m_stats.treesFelled;
                pushEvent(EventType::TreeFelled, {self.treeX, selfPos.y, self.treeZ});
            }
        } else {
            self.chopProgress = 0.0f;
        }

        // Стройка: работа закончена в этом тике -- мир ставит постройку и забирает доски.
        if (self.buildRequest != 0) {
            pushEvent(self.buildRequest == 1 ? EventType::Plank : EventType::Built, {self.buildAt.x, selfPos.y, self.buildAt.y});
            if (self.buildRequest == 1 && self.wood >= kBridgeWood && mutableTerrain && mutableTerrain->addBridge(self.buildAt.x, self.buildAt.y)) {
                self.wood -= kBridgeWood;
                ++m_stats.planksLaid;
            } else if (self.buildRequest == 2 && self.wood >= kBarricadeWood && placeStructure(terrain, StructureKind::Barricade, self.buildAt.x, self.buildAt.y, self.buildYaw)) {
                self.wood -= kBarricadeWood;
                ++m_stats.barricadesBuilt;
            } else if (self.buildRequest == 4 && self.wood >= kStockpileWood && placeStructure(terrain, StructureKind::Stockpile, self.buildAt.x, self.buildAt.y, 0.0f)) {
                self.wood -= kStockpileWood;
                m_structures.back().owner = self.tribe;
                ++m_stats.stockpilesBuilt;
            } else if (self.buildRequest == 5 && self.wood >= kHutWood && placeStructure(terrain, StructureKind::Hut, self.buildAt.x, self.buildAt.y, 0.0f)) {
                self.wood -= kHutWood;
                m_structures.back().owner = self.id;          // хозяин: он и его семья спят здесь охотнее
                self.homePosition = {self.buildAt.x, selfPos.y, self.buildAt.y};   // дом там, где хижина
                ++m_stats.hutsBuilt;
            } else if (self.buildRequest == 6 && self.wood >= kFireWood && placeStructure(terrain, StructureKind::Campfire, self.buildAt.x, self.buildAt.y, 0.0f)) {
                self.wood -= kFireWood;
                ++m_stats.firesBuilt;
            } else if (self.buildRequest == 3 && self.wood >= kTrapWood && placeStructure(terrain, StructureKind::Trap, self.buildAt.x, self.buildAt.y, 0.0f)) {
                self.wood -= kTrapWood;
                ++m_stats.trapsSet;
            }
            self.buildRequest = 0;
        }

        // Знания: открытие в опасности (или очень редко от скуки) и передача соседям и потомству по наблюдению.
        {
            const auto tickNo = static_cast<std::uint64_t>(m_time * 60.0 + 0.5);
            auto roll = [&](std::uint64_t salt, float perSecond) {
                std::uint64_t h = (self.id * 0x9E3779B97F4A7C15ull) ^ (tickNo * 0xBF58476D1CE4E5B9ull) ^ (salt * 0x94D049BB133111EBull);
                h ^= h >> 31; h *= 0x94D049BB133111EBull; h ^= h >> 29;
                return static_cast<float>(h & 0xFFFFFF) / 16777216.0f < perSecond * dt;
            };
            const float curious = 0.5f + self.curiosity();
            const float inspiration = (0.0004f + 0.012f * std::clamp(self.dangerMemory / 120.0f, 0.0f, 1.0f)) * curious;
            // Мост придумывает тот, кто упирается в воду, когда голоден (вода впереди по курсу).
            bool waterAhead = false;
            if (terrain && !self.knows(kSkillBridge) && self.needs.hunger > 0.4f)
                waterAhead = terrain->isUnderwater(selfPos.x + std::cos(self.wanderHeading) * 3.0f, selfPos.z + std::sin(self.wanderHeading) * 3.0f);
            struct Learn { Skill skill; Skill needs; float rate; };
            const Learn learns[] = {{kSkillSpear, static_cast<Skill>(0), inspiration},
                                    {kSkillShield, kSkillSpear, inspiration * 0.5f},
                                    {kSkillBarricade, kSkillSpear, inspiration * 0.4f},
                                    {kSkillTrap, kSkillSpear, inspiration * 0.4f},
                                    {kSkillBridge, static_cast<Skill>(0), (0.0002f + (waterAhead ? 0.006f : 0.0f)) * curious},
                                    {kSkillBucket, static_cast<Skill>(0), (0.0002f + (self.thirst > 0.5f ? 0.006f : 0.0f)) * curious},
                                    {kSkillStockpile, kSkillSpear, (0.0004f + (self.wood >= 3.0f ? 0.004f : 0.0f)) * curious},
                                    {kSkillHut, kSkillSpear, (0.0008f + (senses.night > 0.5f ? 0.008f : 0.0f)) * curious},
                                    {kSkillFire, static_cast<Skill>(0), (0.0004f + (senses.night > 0.3f ? 0.006f : 0.0f)) * curious}};
            const float builderBoost = self.role() == Role::Builder ? 2.0f : 1.0f;   // строитель открывает постройки вдвое быстрее
            const Skill kBuildSkills = static_cast<Skill>(kSkillBridge | kSkillBarricade | kSkillTrap | kSkillStockpile | kSkillHut | kSkillFire);
            for (const Learn& l : learns)
                if (!self.knows(l.skill) && (l.needs == 0 || self.knows(l.needs)) && roll(l.skill, l.rate * ((l.skill & kBuildSkills) ? builderBoost : 1.0f))) { self.skills |= l.skill; ++m_stats.skillsLearned; }
            if (self.skills != kAllSkills) {
                forEachNearby(
                    creatureGrid, selfPos, 8.0f, [this](std::size_t j) { return m_population[j].creature.position; },
                    [&](std::size_t j, float) {
                        if (j == i) return;
                        const Creature& o = m_population[j].creature;
                        const bool family = o.id == self.bondedPartnerId || self.id == o.parentIds[0] || self.id == o.parentIds[1] ||
                                            o.id == self.parentIds[0] || o.id == self.parentIds[1];
                        for (const Learn& l : learns)
                            if (o.knows(l.skill) && !self.knows(l.skill) && (l.needs == 0 || self.knows(l.needs)) &&
                                roll(10 + l.skill, (family ? 0.06f : 0.02f) * curious)) {
                                self.skills |= l.skill;
                                ++m_stats.skillsLearned;
                            }
                    });
            }
        }

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
            if (self.hasBucket && self.water >= 1.0f && src.amount < 0.45f) {   // ведро: поливает поникший куст
                src.amount = std::min(1.0f, src.amount + 0.3f);
                self.water -= 1.0f;
                ++m_stats.bushesWatered;
            }
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
                const Creature& ci = m_population[i].creature; const Creature& cj = m_population[j].creature;
                if (ci.tribe != 0 && cj.tribe != 0 && ci.tribe != cj.tribe && m_relations[ci.tribe][cj.tribe] < 0.0f) return;   // враги не женятся
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
            child.tribe    = a.tribe;
            if (a.tribe != b.tribe && b.tribe != 0) {   // смешанный брак: ребёнок берёт племя одного из родителей, племена сближаются
                if (a.tribe == 0 || unit(m_rng) < 0.5f) child.tribe = b.tribe;
                if (a.tribe != 0) setRelation(a.tribe, b.tribe, m_relations[a.tribe][b.tribe] + 0.05f);
                ++m_stats.mixedBirths;
            }
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
            case 6: ++m_stats.deathsThirst; break;
            case 7: ++m_stats.deathsConflict; break;
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
    const float growMul = terrain ? foodFactor(m_time) * (1.0f + 0.3f * rainNow) : 1.0f;   // сезон и дождь (только в мире с рельефом)
    for (auto& src : m_food)
        src.amount = std::min(kFoodCap, src.amount + kFoodRegrowRate * berryDef(src.kind).regrow * growMul * (0.25f + 0.75f * src.amount) * dt);
}

}  // namespace wsim
