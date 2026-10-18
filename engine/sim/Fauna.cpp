#include "sim/Fauna.hpp"

#include "sim/Trees.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wsim {

namespace {

constexpr float kPi = 3.14159265f;

constexpr std::uint32_t bit(Species s) { return 1u << static_cast<unsigned>(s); }

// Порядок биомов: Tundra, Taiga, Plains, Forest, Desert, Savanna, Jungle.
constexpr std::array<SpeciesDef, kSpeciesCount> kDefs{{
    //  id          название        рацион                 размер скор. зрение паника группа          тундра тайга равн. лес  пуст. сав.  джун.   потолок взрослость размнож. жизнь  добыча                          на существ  плавает
    {"hare",     "Заяц",        Diet::Herbivore, 0.45f, 7.0f, 22.0f, 16.0f, 1, 3, {{0.5f, 0.6f, 1.0f, 0.8f, 0.0f, 0.2f, 0.0f}},  20,  90.0f,  70.0f,  900.0f, 0,                               false, false},
    {"deer",     "Олень",       Diet::Herbivore, 1.10f, 6.0f, 28.0f, 20.0f, 2, 5, {{0.3f, 0.7f, 0.8f, 1.0f, 0.0f, 0.0f, 0.0f}},  14, 140.0f, 130.0f, 1500.0f, 0,                               false, false},
    {"antelope", "Антилопа",    Diet::Herbivore, 0.95f, 7.5f, 30.0f, 22.0f, 3, 7, {{0.0f, 0.0f, 0.3f, 0.0f, 0.3f, 1.0f, 0.0f}},  16, 130.0f, 110.0f, 1400.0f, 0,                               false, false},
    {"tapir",    "Тапир",       Diet::Herbivore, 1.00f, 4.8f, 18.0f, 12.0f, 1, 2, {{0.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.0f, 1.0f}},  7, 150.0f, 160.0f, 1600.0f, 0,                               false, true },
    {"wolf",     "Волк",        Diet::Predator,  0.95f, 7.0f, 32.0f,  0.0f, 2, 3, {{0.7f, 0.9f, 0.3f, 0.5f, 0.0f, 0.0f, 0.0f}},  5, 200.0f, 320.0f, 2000.0f, bit(Species::Hare) | bit(Species::Deer), true,  false},
    {"fox",      "Лиса",        Diet::Predator,  0.55f, 6.2f, 24.0f,  0.0f, 1, 1, {{0.4f, 0.5f, 0.7f, 0.7f, 0.2f, 0.0f, 0.0f}},  4, 160.0f, 240.0f, 1500.0f, bit(Species::Hare),                false, false},
    {"lion",     "Лев",         Diet::Predator,  1.15f, 7.5f, 34.0f,  0.0f, 1, 2, {{0.0f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f, 0.0f}},  4, 220.0f, 360.0f, 2200.0f, bit(Species::Antelope) | bit(Species::Deer), true,  false},
    {"jaguar",   "Ягуар",       Diet::Predator,  0.90f, 7.0f, 28.0f,  0.0f, 1, 1, {{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}},  3, 200.0f, 360.0f, 1900.0f, bit(Species::Tapir) | bit(Species::Deer),   true,  true },
}};

constexpr float kPredatorBiomeShare = 0.12f;  // хищники в выборе вида по биому в разы реже травоядных

float unitRand(Rng& rng) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); }

glm::vec2 headingVec(float h) { return {std::cos(h), std::sin(h)}; }
float headingOf(glm::vec2 v) { return std::atan2(v.y, v.x); }

float wrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

}  // namespace

const SpeciesDef& speciesDef(Species s) noexcept { return kDefs[static_cast<std::size_t>(s)]; }

bool speciesFromId(std::string_view id, Species& out) noexcept {
    for (int i = 0; i < kSpeciesCount; ++i)
        if (id == kDefs[static_cast<std::size_t>(i)].id) { out = static_cast<Species>(i); return true; }
    return false;
}

bool pickSpeciesFor(Biome biome, float u, Species& out) noexcept {
    float total = 0.0f;
    auto weight = [&](int i) {
        const SpeciesDef& d = kDefs[static_cast<std::size_t>(i)];
        return d.biome[static_cast<std::size_t>(biome)] * (d.diet == Diet::Predator ? kPredatorBiomeShare : 1.0f);
    };
    for (int i = 0; i < kSpeciesCount; ++i) total += weight(i);
    if (total <= 0.0f) return false;
    float t = u * total;
    int   last = -1;
    for (int i = 0; i < kSpeciesCount; ++i) {
        const float w = weight(i);
        if (w <= 0.0f) continue;
        last = i;
        if (t < w) { out = static_cast<Species>(i); return true; }
        t -= w;
    }
    out = static_cast<Species>(last);
    return last >= 0;
}

const char* animalStateName(AnimalState s) noexcept {
    switch (s) {
        case AnimalState::Wander: return "Бродит";
        case AnimalState::Rest:   return "Отдыхает";
        case AnimalState::Graze:  return "Пасётся";
        case AnimalState::Flee:   return "Убегает";
        case AnimalState::Hunt:   return "Охотится";
        case AnimalState::Feed:   return "Ест добычу";
        case AnimalState::Count:  break;
    }
    return "?";
}

std::size_t Fauna::count(Species s) const noexcept {
    std::size_t n = 0;
    for (const Animal& a : m_animals) n += a.alive && a.species == s;
    return n;
}

bool Fauna::spawn(Species s, float x, float z, Rng& rng, bool adult) {
    const SpeciesDef& d = speciesDef(s);
    if (m_animals.size() >= kMaxAnimals || static_cast<int>(count(s)) >= d.cap) return false;
    for (const Animal& a : m_animals)
        if (a.alive && std::hypot(a.position.x - x, a.position.z - z) < kMinSpacing) return false;
    Animal a;
    a.id       = m_nextId++;
    a.species  = s;
    a.position = {x, 0.0f, z};
    a.heading  = 2.0f * kPi * unitRand(rng);
    a.hunger   = 0.15f + 0.25f * unitRand(rng);
    a.age      = adult ? d.maturity * (1.0f + 0.5f * unitRand(rng)) : 0.0f;
    a.breedCooldown = d.breedCooldown * 0.5f * unitRand(rng);
    a.wanderTimer   = 4.0f + 8.0f * unitRand(rng);
    m_animals.push_back(a);
    return true;
}

bool Fauna::spawnNatural(const Terrain& terrain, float x, float z, Rng& rng) {
    Species s{};
    if (!pickSpeciesFor(terrain.biomeAt(x, z), unitRand(rng), s)) return false;
    if (!speciesDef(s).swims && terrain.isUnderwater(x, z)) return false;
    if (!spawn(s, x, z, rng)) return false;
    m_animals.back().position.y = terrain.heightAt(x, z);
    return true;
}

std::size_t Fauna::removeInCircle(float x, float z, float radius) {
    std::size_t removed = 0;
    for (Animal& a : m_animals)
        if (a.alive && std::hypot(a.position.x - x, a.position.z - z) <= radius) { a.alive = false; a.deathCause = 4; ++removed; }
    std::erase_if(m_animals, [](const Animal& a) { return !a.alive; });
    return removed;
}

bool Fauna::nearest(float x, float z, float radius, std::size_t& index) const {
    float best = radius;
    bool  found = false;
    for (std::size_t i = 0; i < m_animals.size(); ++i) {
        const Animal& a = m_animals[i];
        if (!a.alive) continue;
        const float d = std::hypot(a.position.x - x, a.position.z - z);
        if (d <= best) { best = d; index = i; found = true; }
    }
    return found;
}

bool Fauna::importState(const State& s) {
    for (const Animal& a : s.animals)
        if (!std::isfinite(a.position.x) || !std::isfinite(a.position.z) || static_cast<int>(a.species) >= kSpeciesCount) return false;
    m_animals          = s.animals;
    m_nextId           = std::max<std::uint64_t>(s.nextId, 1);
    m_immigrationTimer = std::isfinite(s.immigrationTimer) ? s.immigrationTimer : 0.0f;
    m_explored         = s.explored;
    std::sort(m_explored.begin(), m_explored.end());
    m_explored.erase(std::unique(m_explored.begin(), m_explored.end()), m_explored.end());
    m_streamTimer      = 100.0f;
    return true;
}

namespace {
std::int32_t wildCellIndex(float v, float cell) { return static_cast<std::int32_t>(std::floor(v / cell)); }
std::int64_t packWild(std::int32_t cx, std::int32_t cz) { return (static_cast<std::int64_t>(cx) << 32) | static_cast<std::uint32_t>(cz); }
}  // namespace

void Fauna::hurt(std::uint64_t id, float damage) {
    for (Animal& a : m_animals) {
        if (a.id != id || !a.alive) continue;
        a.health -= damage;
        if (a.health <= 0.0f) { a.alive = false; a.deathCause = 5; }
        else { a.state = AnimalState::Flee; a.stateTimer = 6.0f; a.searchCooldown = 25.0f; a.targetIsCreature = false; }
        return;
    }
}

Fauna::State Fauna::exportState() const { return {m_nextId, m_animals, m_immigrationTimer, m_explored}; }

bool Fauna::markExplored(std::int32_t cx, std::int32_t cz) {
    const std::int64_t key = packWild(cx, cz);
    const auto it = std::lower_bound(m_explored.begin(), m_explored.end(), key);
    if (it != m_explored.end() && *it == key) return false;
    m_explored.insert(it, key);
    return true;
}

void Fauna::spawnGroupAt(Species s, glm::vec2 centre, const Terrain& terrain, Rng& rng, float halfSize) {
    const SpeciesDef& d = speciesDef(s);
    const int n = d.groupMin + static_cast<int>(unitRand(rng) * static_cast<float>(d.groupMax - d.groupMin + 1) * 0.9999f);
    for (int k = 0; k < n; ++k) {
        const float a = 2.0f * kPi * unitRand(rng), r = 4.0f * std::sqrt(unitRand(rng));
        const float x = centre.x + std::cos(a) * r, z = centre.y + std::sin(a) * r;
        if (halfSize > 0.0f && (std::fabs(x) > halfSize - 3.0f || std::fabs(z) > halfSize - 3.0f)) continue;
        if (!d.swims && terrain.isUnderwater(x, z)) continue;
        if (spawn(s, x, z, rng)) m_animals.back().position.y = terrain.heightAt(x, z);
    }
}

void Fauna::populate(const Terrain& terrain, Rng& rng, float halfSize) {
    m_animals.clear();
    m_explored.clear();
    m_streamTimer = 100.0f;
    // Стартовая область считается исследованной: дальше заселение идёт ячейками (streamWildlife).
    for (std::int32_t cz = wildCellIndex(-halfSize, kWildCell); cz <= wildCellIndex(halfSize, kWildCell); ++cz)
        for (std::int32_t cx = wildCellIndex(-halfSize, kWildCell); cx <= wildCellIndex(halfSize, kWildCell); ++cx) markExplored(cx, cz);
    auto landPoint = [&](glm::vec2& p, Species* forSpecies) {
        for (int attempt = 0; attempt < 80; ++attempt) {
            p = {(unitRand(rng) * 2.0f - 1.0f) * (halfSize - 6.0f), (unitRand(rng) * 2.0f - 1.0f) * (halfSize - 6.0f)};
            if (terrain.heightAt(p.x, p.y) <= 0.6f) continue;
            if (forSpecies && !speciesLivesIn(*forSpecies, terrain.biomeAt(p.x, p.y))) continue;
            return true;
        }
        return false;
    };
    // Травоядные: группы там, где им место (вид выбирается по биому точки, хищники в этом проходе не селятся).
    for (int g = 0; g < 14; ++g) {
        glm::vec2 p;
        if (!landPoint(p, nullptr)) break;
        Species s{};
        if (!pickSpeciesFor(terrain.biomeAt(p.x, p.y), unitRand(rng), s) || isPredator(s)) continue;
        spawnGroupAt(s, p, terrain, rng, halfSize);
    }
    // Хищники: по одной стае там, где им место и есть кого есть (хотя бы три головы добычи).
    for (int i = 0; i < kSpeciesCount; ++i) {
        const auto s = static_cast<Species>(i);
        const SpeciesDef& d = speciesDef(s);
        if (d.diet != Diet::Predator) continue;
        std::size_t prey = 0;
        for (const Animal& a : m_animals) prey += (d.prey & bit(a.species)) != 0;
        if (prey < 3 || unitRand(rng) > 0.85f) continue;
        glm::vec2 p;
        Species sp = s;
        if (landPoint(p, &sp)) spawnGroupAt(s, p, terrain, rng, halfSize);
    }
}

// Мир без границ: ячейки вокруг существ и точек интереса заселяются при первом попадании в поле зрения (раз и навсегда), а вымирающие
// виды возвращаются кольцом вокруг этих точек. Дальних зверей мир не гасит -- он просто не рождает новых сверх потолков видов.
void Fauna::streamWildlife(float dt, const Terrain& terrain, std::span<const CreatureView> creatures, std::span<const glm::vec2> focus, Rng& rng) {
    m_streamTimer += dt;
    if (m_streamTimer < 1.0f) return;
    m_streamTimer = 0.0f;
    constexpr float kReach = 96.0f;   // заселяем на такое расстояние от существ и камеры
    std::vector<glm::vec2> anchors;
    anchors.reserve(creatures.size() + focus.size());
    for (const CreatureView& c : creatures) anchors.push_back({c.position.x, c.position.z});
    for (const glm::vec2& f : focus) anchors.push_back(f);
    if (anchors.empty()) return;

    std::vector<std::int64_t> fresh;
    for (const glm::vec2& a : anchors) {
        const std::int32_t x0 = wildCellIndex(a.x - kReach, kWildCell), x1 = wildCellIndex(a.x + kReach, kWildCell);
        const std::int32_t z0 = wildCellIndex(a.y - kReach, kWildCell), z1 = wildCellIndex(a.y + kReach, kWildCell);
        for (std::int32_t cz = z0; cz <= z1; ++cz)
            for (std::int32_t cx = x0; cx <= x1; ++cx)
                if (markExplored(cx, cz)) fresh.push_back(packWild(cx, cz));
    }
    std::sort(fresh.begin(), fresh.end());   // порядок не зависит от порядка существ
    for (std::int64_t key : fresh) {
        const auto cx = static_cast<std::int32_t>(key >> 32), cz = static_cast<std::int32_t>(static_cast<std::uint32_t>(key));
        const glm::vec2 p{(static_cast<float>(cx) + unitRand(rng)) * kWildCell, (static_cast<float>(cz) + unitRand(rng)) * kWildCell};
        if (terrain.heightAt(p.x, p.y) <= 0.6f) continue;
        const Biome biome = terrain.biomeAt(p.x, p.y);
        if (unitRand(rng) < 0.8f) {   // группа травоядных (плотность как в стартовой области: ~14 групп на 200x200)
            Species s{};
            if (pickSpeciesFor(biome, unitRand(rng), s) && !isPredator(s)) spawnGroupAt(s, p, terrain, rng, 0.0f);
        }
        if (unitRand(rng) < 0.06f) {  // редкая стая хищников, если рядом есть кого есть
            Species s{};
            for (int i = 0; i < 8; ++i) {
                if (!pickSpeciesFor(biome, unitRand(rng), s)) break;
                if (isPredator(s)) break;
            }
            if (isPredator(s)) {
                std::size_t prey = 0;
                for (const Animal& a : m_animals)
                    prey += a.alive && (speciesDef(s).prey & bit(a.species)) != 0 && std::hypot(a.position.x - p.x, a.position.z - p.y) < 90.0f;
                if (prey >= 3) spawnGroupAt(s, p, terrain, rng, 0.0f);
            }
        }
    }

    // Пополнение: вымирающий вид приходит кольцом вокруг случайной точки интереса (травоядные -- охотнее, хищники -- редко и вслед за добычей).
    m_immigrationTimer += 1.0f;
    if (m_immigrationTimer >= 25.0f) {
        m_immigrationTimer = 0.0f;
        for (int si = 0; si < kSpeciesCount; ++si) {
            const auto sp = static_cast<Species>(si);
            const SpeciesDef& d = speciesDef(sp);
            const std::size_t have = count(sp);
            bool want = false;
            if (d.diet == Diet::Herbivore) want = have < static_cast<std::size_t>(std::max(2, d.cap / 6)) && unitRand(rng) < 0.7f;
            else if (have == 0) {
                std::size_t prey = 0;
                for (const Animal& o : m_animals) prey += o.alive && (d.prey & bit(o.species)) != 0;
                want = prey >= 6 && unitRand(rng) < 0.1f;
            }
            if (!want) continue;
            for (int attempt = 0; attempt < 14; ++attempt) {
                const glm::vec2 a = anchors[static_cast<std::size_t>(unitRand(rng) * static_cast<float>(anchors.size()) * 0.9999f)];
                const float ang = 2.0f * kPi * unitRand(rng), r = 80.0f + 30.0f * unitRand(rng);
                const glm::vec2 p = a + glm::vec2{std::cos(ang), std::sin(ang)} * r;
                if (terrain.heightAt(p.x, p.y) <= 0.6f || !speciesLivesIn(sp, terrain.biomeAt(p.x, p.y))) continue;
                const std::size_t before = m_animals.size();
                spawnGroupAt(sp, p, terrain, rng, 0.0f);
                m_stats.immigrants += m_animals.size() - before;
                break;
            }
        }
    }
}

void Fauna::tick(float dt, const Terrain* terrain, std::span<const CreatureView> creatures, Rng& rng, float halfSize, std::vector<Bite>& bites,
                 std::span<const glm::vec2> focus) {
    const std::size_t n = m_animals.size();

    auto idToAnimal = [&](std::uint64_t id) -> Animal* {
        for (Animal& a : m_animals) if (a.id == id && a.alive) return &a;
        return nullptr;
    };
    auto idToCreature = [&](std::uint64_t id) -> const CreatureView* {
        for (const CreatureView& c : creatures) if (c.id == id) return &c;
        return nullptr;
    };

    for (std::size_t i = 0; i < n; ++i) {
        Animal& a = m_animals[i];
        if (!a.alive) continue;
        const SpeciesDef& d = speciesDef(a.species);
        const glm::vec2 pos{a.position.x, a.position.z};

        // ── Жизнь: возраст, голод, перезарядки.
        a.age += dt;
        a.breedCooldown  = std::max(0.0f, a.breedCooldown - dt);
        a.attackCooldown = std::max(0.0f, a.attackCooldown - dt);
        a.searchCooldown = std::max(0.0f, a.searchCooldown - dt);
        a.sinceBite     += dt;
        a.stateTimer     = std::max(0.0f, a.stateTimer - dt);
        a.hunger         = std::clamp(a.hunger + (d.diet == Diet::Predator ? 0.0025f : 0.004f) * dt, 0.0f, 1.0f);
        if (a.hunger >= 0.999f) a.health -= 0.012f * dt;
        else if (a.hunger < 0.6f) a.health = std::min(1.0f, a.health + 0.004f * dt);
        if (a.age >= d.lifespan) { a.alive = false; a.deathCause = 1; continue; }
        if (a.health <= 0.0f)    { a.alive = false; a.deathCause = 2; continue; }

        glm::vec2 desired{0.0f};   // куда хочет идти
        float     speedFrac = 0.0f;
        bool      sprinting = false;

        if (d.diet == Diet::Herbivore) {
            // ── Травоядное: страх -> пасётся -> бродит стадом.
            float threatDist = std::numeric_limits<float>::max();
            glm::vec2 threatFrom{0.0f};
            for (const Animal& o : m_animals) {
                if (!o.alive || speciesDef(o.species).diet != Diet::Predator) continue;
                const float dist = std::hypot(o.position.x - pos.x, o.position.z - pos.y);
                if (dist < d.panic && dist < threatDist) { threatDist = dist; threatFrom = {o.position.x, o.position.z}; }
            }
            for (const CreatureView& c : creatures) {
                const float dist = std::hypot(c.position.x - pos.x, c.position.z - pos.y);
                if (dist < d.panic * 0.3f && dist < threatDist) { threatDist = dist; threatFrom = {c.position.x, c.position.z}; }
            }
            if (threatDist < std::numeric_limits<float>::max()) {
                a.state = AnimalState::Flee;
                a.stateTimer = 2.5f;
                const glm::vec2 away = pos - threatFrom;
                a.heading = headingOf(glm::length(away) > 1.0e-4f ? away : headingVec(a.heading)) + (unitRand(rng) - 0.5f) * 0.4f;
            }
            if (a.state == AnimalState::Flee && a.stateTimer > 0.0f) {
                desired   = headingVec(a.heading);
                speedFrac = a.stamina > 0.1f ? 1.0f : 0.45f;
                sprinting = true;
            } else {
                if (a.state == AnimalState::Flee) { a.state = AnimalState::Wander; a.wanderTimer = 3.0f; }
                if (a.hunger > 0.55f || (a.state == AnimalState::Graze && a.hunger > 0.2f)) a.state = AnimalState::Graze;
                else if (a.state == AnimalState::Graze) { a.state = AnimalState::Wander; a.wanderTimer = 6.0f + 8.0f * unitRand(rng); }
                if (a.state == AnimalState::Graze) {
                    a.hunger = std::max(0.0f, a.hunger - 0.06f * dt);   // щиплет траву: еды хватает везде, куда ни встань
                } else {
                    a.wanderTimer -= dt;
                    if (a.wanderTimer <= 0.0f) {
                        a.state = a.state == AnimalState::Rest ? AnimalState::Wander : AnimalState::Rest;
                        a.wanderTimer = a.state == AnimalState::Rest ? 5.0f + 8.0f * unitRand(rng) : 8.0f + 12.0f * unitRand(rng);
                    }
                    if (a.state == AnimalState::Wander) {
                        a.heading += (unitRand(rng) - 0.5f) * 1.6f * dt * 4.0f * 0.25f;
                        desired   = headingVec(a.heading);
                        speedFrac = 0.28f;
                        // Стадо: тянется к центру своих, если отстал.
                        glm::vec2 sum{0.0f};
                        int       mates = 0;
                        for (const Animal& o : m_animals)
                            if (&o != &a && o.alive && o.species == a.species && std::hypot(o.position.x - pos.x, o.position.z - pos.y) < 18.0f) {
                                sum += glm::vec2{o.position.x, o.position.z};
                                ++mates;
                            }
                        if (mates > 0) {
                            const glm::vec2 toCentre = sum / static_cast<float>(mates) - pos;
                            if (glm::length(toCentre) > 7.0f) desired = glm::normalize(desired + glm::normalize(toCentre) * 1.2f);
                        }
                    }
                }
            }
            a.stamina = sprinting ? std::max(0.0f, a.stamina - 0.07f * dt) : std::min(1.0f, a.stamina + 0.08f * dt);
        } else {
            // ── Хищник: голоден -> выслеживает добычу и преследует; сыт -> бродит и отдыхает.
            a.stamina = std::min(1.0f, a.stamina + (a.state == AnimalState::Hunt ? 0.0f : 0.05f) * dt);
            if (a.state == AnimalState::Hunt) {
                const CreatureView* cv = a.targetIsCreature ? idToCreature(a.targetId) : nullptr;
                Animal*             av = a.targetIsCreature ? nullptr : idToAnimal(a.targetId);
                if (!cv && !av) {
                    // Цель пропала: если она была существом, которое мы только что кусали -- считаем добычей.
                    if (a.targetIsCreature && a.sinceBite < 4.0f) { a.hunger = std::max(0.0f, a.hunger - 0.5f); a.state = AnimalState::Feed; a.stateTimer = 6.0f; }
                    else { a.state = AnimalState::Wander; a.searchCooldown = 10.0f; }
                } else {
                    const glm::vec2 tp = cv ? glm::vec2{cv->position.x, cv->position.z} : glm::vec2{av->position.x, av->position.z};
                    const float dist = glm::length(tp - pos);
                    if (dist > d.sense * 1.4f || a.stateTimer <= 0.0f) {
                        a.state = AnimalState::Wander;
                        a.searchCooldown = 15.0f;   // сбежала -- отдыхаем и пробуем позже
                    } else {
                        desired = dist > 1.0e-4f ? (tp - pos) / dist : headingVec(a.heading);
                        const bool exhausted = a.stamina <= 0.0f;
                        speedFrac = exhausted ? 0.3f : 1.0f;
                        sprinting = !exhausted;
                        if (dist <= 1.3f * d.size + 0.6f && a.attackCooldown <= 0.0f) {
                            if (cv) {
                                a.attackCooldown = 1.1f;
                                a.sinceBite = 0.0f;
                                bites.push_back({cv->id, 0.28f, a.id});
                                ++m_stats.creatureBites;
                            } else {
                                a.attackCooldown = 0.7f;
                                if (unitRand(rng) < 0.55f) {   // добыча не всегда дается: промах -- и она убегает
                                    av->alive = false;
                                    av->deathCause = 3;
                                    ++m_stats.kills;
                                    a.hunger = std::max(0.0f, a.hunger - 0.55f);
                                    a.state = AnimalState::Feed;
                                    a.stateTimer = 6.0f;
                                }
                            }
                        }
                        a.stamina = sprinting ? std::max(0.0f, a.stamina - 0.12f * dt) : std::min(1.0f, a.stamina + 0.08f * dt);
                    }
                }
            }
            if (a.state == AnimalState::Feed) {
                if (a.stateTimer <= 0.0f) { a.state = AnimalState::Wander; a.wanderTimer = 6.0f; }
                // стоит над добычей
            } else if (a.state != AnimalState::Hunt) {
                if (a.hunger > 0.5f && a.searchCooldown <= 0.0f) {
                    // Выбор добычи: ближайшее животное из списка; голодный (и умеющий) -- существо, если животных рядом нет.
                    float best = d.sense;
                    const Animal* target = nullptr;
                    for (const Animal& o : m_animals)
                        if (o.alive && (d.prey & bit(o.species)) != 0) {
                            const float dist = std::hypot(o.position.x - pos.x, o.position.z - pos.y);
                            if (dist < best) { best = dist; target = &o; }
                        }
                    if (target) {
                        a.state = AnimalState::Hunt; a.targetId = target->id; a.targetIsCreature = false; a.stateTimer = 25.0f;
                    } else if (d.huntsCreatures && a.hunger > 0.7f) {
                        float bestC = d.sense * 0.8f;
                        const CreatureView* ct = nullptr;
                        for (const CreatureView& c : creatures) {
                            const float dist = std::hypot(c.position.x - pos.x, c.position.z - pos.y);
                            if (dist < bestC) { bestC = dist; ct = &c; }
                        }
                        if (ct) { a.state = AnimalState::Hunt; a.targetId = ct->id; a.targetIsCreature = true; a.stateTimer = 25.0f; }
                        else a.searchCooldown = 4.0f;
                    } else {
                        a.searchCooldown = 4.0f;
                    }
                }
                if (a.state != AnimalState::Hunt) {
                    a.wanderTimer -= dt;
                    if (a.wanderTimer <= 0.0f) {
                        a.state = a.state == AnimalState::Rest ? AnimalState::Wander : AnimalState::Rest;
                        a.wanderTimer = a.state == AnimalState::Rest ? 8.0f + 12.0f * unitRand(rng) : 10.0f + 15.0f * unitRand(rng);
                    }
                    if (a.state == AnimalState::Wander) {
                        a.heading += (unitRand(rng) - 0.5f) * 1.6f * dt;
                        desired   = headingVec(a.heading);
                        speedFrac = a.hunger > 0.5f ? 0.45f : 0.28f;   // голодный ищет добычу, ходит бодрее
                    }
                }
            }
        }

        // ── Расталкивание со своими, границы мира, обход воды и круч.
        for (const Animal& o : m_animals)
            if (&o != &a && o.alive) {
                const float dist = std::hypot(o.position.x - pos.x, o.position.z - pos.y);
                if (dist < 1.4f * d.size && dist > 1.0e-3f && o.species == a.species) desired += (pos - glm::vec2{o.position.x, o.position.z}) / dist * 0.8f * (speedFrac > 0.0f ? 1.0f : 0.0f);
            }
        const float lim = halfSize - 5.0f;
        if (speedFrac > 0.0f && halfSize > 0.0f) {
            if (std::fabs(pos.x) > lim) desired.x -= std::copysign(1.2f, pos.x);
            if (std::fabs(pos.y) > lim) desired.y -= std::copysign(1.2f, pos.y);
        }
        float moveSpeed = speedFrac * d.speed;
        glm::vec2 dir = glm::length(desired) > 1.0e-4f ? glm::normalize(desired) : glm::vec2{0.0f};
        if (terrain && moveSpeed > 0.0f) {
            auto blockedAt = [&](glm::vec2 v) {
                const glm::vec2 q = pos + v * (1.4f + 0.12f * moveSpeed);
                if (!d.swims && terrain->isUnderwater(q.x, q.y)) return true;
                if ((terrain->heightAt(q.x, q.y) - a.position.y) / 1.5f > 1.0f) return true;
                return Trees::blocksPoint(*terrain, q.x, q.y, 0.6f);
            };
            if (blockedAt(dir)) {
                bool found = false;
                for (int k = 1; k <= 4 && !found; ++k)
                    for (float sign : {1.0f, -1.0f}) {
                        const float ang = sign * static_cast<float>(k) * 40.0f * kPi / 180.0f, c = std::cos(ang), sn = std::sin(ang);
                        const glm::vec2 v{dir.x * c - dir.y * sn, dir.x * sn + dir.y * c};
                        if (!blockedAt(v)) { dir = v; found = true; break; }
                    }
                if (!found) { moveSpeed = 0.0f; a.heading += kPi * 0.6f; }
            }
        }
        // Курс плавно доворачивается (животное не вертится на месте).
        if (moveSpeed > 0.0f) {
            const float target = headingOf(dir);
            a.heading += std::clamp(wrapPi(target - a.heading), -6.0f * dt, 6.0f * dt);
        }
        a.speedNow = moveSpeed;
        if (moveSpeed > 0.0f) {
            const glm::vec2 step = headingVec(a.heading) * moveSpeed * dt;
            const float nx = halfSize > 0.0f ? std::clamp(a.position.x + step.x, -halfSize + 1.0f, halfSize - 1.0f) : a.position.x + step.x;
            const float nz = halfSize > 0.0f ? std::clamp(a.position.z + step.y, -halfSize + 1.0f, halfSize - 1.0f) : a.position.z + step.y;
            // Жёсткий запрет: в воду (кроме умеющих плавать) и в ствол не заходим, даже если луч вперёд ничего не заметил.
            const bool hardBlocked = terrain && ((!d.swims && terrain->isUnderwater(nx, nz)) || Trees::blocksPoint(*terrain, nx, nz, 0.4f));
            if (hardBlocked) {
                a.heading += (unitRand(rng) < 0.5f ? 1.0f : -1.0f) * 0.9f;
                a.speedNow = 0.0f;
            } else {
                a.position.x = nx;
                a.position.z = nz;
            }
        }
        a.position.y = terrain ? terrain->heightAt(a.position.x, a.position.z) : 0.0f;
    }

    // ── Размножение: взрослые сытые и здоровые, рядом есть сородич, ниже потолка вида.
    for (std::size_t i = 0; i < n; ++i) {
        // Ссылку на животное держать нельзя: spawn() ниже может перевыделить вектор -- работаем по индексу.
        const Animal    snapshot = m_animals[i];
        const Species   species = snapshot.species;
        const SpeciesDef& d = speciesDef(species);
        const Animal& a = snapshot;
        if (!a.alive || a.age < d.maturity || a.breedCooldown > 0.0f || a.hunger > (d.diet == Diet::Predator ? 0.45f : 0.4f) || a.health < 0.7f) continue;
        if (static_cast<int>(count(species)) >= d.cap || m_animals.size() >= kMaxAnimals) continue;
        bool mate = false;
        for (std::size_t j = 0; j < n && !mate; ++j) {
            const Animal& o = m_animals[j];
            mate = j != i && o.alive && o.species == species && o.age >= d.maturity && std::hypot(o.position.x - a.position.x, o.position.z - a.position.z) < 9.0f;
        }
        if (!mate || unitRand(rng) > dt / 6.0f) continue;
        const int litter = species == Species::Hare ? 1 + (unitRand(rng) < 0.5f ? 1 : 0) : 1;
        for (int k = 0; k < litter; ++k) {
            const float ang = 2.0f * kPi * unitRand(rng);
            const float x = a.position.x + std::cos(ang) * 1.6f, z = a.position.z + std::sin(ang) * 1.6f;
            if (halfSize > 0.0f && (std::fabs(x) > halfSize - 2.0f || std::fabs(z) > halfSize - 2.0f)) continue;
            if (terrain && !d.swims && terrain->isUnderwater(x, z)) continue;
            if (spawn(species, x, z, rng, /*adult=*/false)) {
                m_animals.back().position.y = terrain ? terrain->heightAt(x, z) : 0.0f;
                ++m_stats.births;
            }
        }
        m_animals[i].breedCooldown = d.breedCooldown * (0.8f + 0.4f * unitRand(rng));
        m_animals[i].hunger        = std::min(1.0f, m_animals[i].hunger + 0.1f);
    }

    // ── Миграция: вид почти вымер -- с края приходит новая группа (травоядные), хищники -- редко и только если есть добыча.
    if (terrain && halfSize <= 0.0f) streamWildlife(dt, *terrain, creatures, focus, rng);   // мир без границ: ячейки и пополнение
    if (halfSize > 0.0f) m_immigrationTimer += dt;
    if (terrain && halfSize > 0.0f && m_immigrationTimer >= 25.0f) {
        m_immigrationTimer = 0.0f;
        for (int si = 0; si < kSpeciesCount; ++si) {
            const auto s = static_cast<Species>(si);
            const SpeciesDef& d = speciesDef(s);
            const std::size_t have = count(s);
            const std::size_t minCount = static_cast<std::size_t>(std::max(2, d.cap / 6));
            bool want = false;
            if (d.diet == Diet::Herbivore) want = have < minCount && unitRand(rng) < 0.7f;
            else if (have == 0) {
                std::size_t prey = 0;
                for (const Animal& o : m_animals) prey += o.alive && (d.prey & bit(o.species)) != 0;
                want = prey >= 6 && unitRand(rng) < 0.1f;
            }
            if (!want) continue;
            for (int attempt = 0; attempt < 14; ++attempt) {
                const bool alongX = unitRand(rng) < 0.5f;
                const float edge = (unitRand(rng) < 0.5f ? -1.0f : 1.0f) * (halfSize - 5.0f), along = (unitRand(rng) * 2.0f - 1.0f) * (halfSize - 8.0f);
                const float x = alongX ? along : edge, z = alongX ? edge : along;
                if (terrain->heightAt(x, z) <= 0.6f || !speciesLivesIn(s, terrain->biomeAt(x, z))) continue;
                const int cnt = d.groupMin + static_cast<int>(unitRand(rng) * static_cast<float>(d.groupMax - d.groupMin + 1) * 0.9999f);
                for (int k = 0; k < cnt; ++k) {
                    const float a2 = 2.0f * kPi * unitRand(rng), r = 3.0f * std::sqrt(unitRand(rng));
                    const float px = x + std::cos(a2) * r, pz = z + std::sin(a2) * r;
                    if (std::fabs(px) > halfSize - 2.0f || std::fabs(pz) > halfSize - 2.0f) continue;
                    if (!d.swims && terrain->isUnderwater(px, pz)) continue;
                    if (spawn(s, px, pz, rng)) { m_animals.back().position.y = terrain->heightAt(px, pz); ++m_stats.immigrants; }
                }
                break;
            }
        }
    }

    // ── Уборка: умершие уходят (статистика).
    for (const Animal& a : m_animals) {
        if (a.alive) continue;
        if (a.deathCause == 1) ++m_stats.deathsOld;
        else if (a.deathCause == 2) ++m_stats.deathsStarved;
    }
    std::erase_if(m_animals, [](const Animal& a) { return !a.alive; });
}

}  // namespace wsim
