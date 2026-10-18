#include "sim/Trees.hpp"

#include <algorithm>
#include <cmath>

namespace wsim {

namespace {

std::uint64_t mix(std::uint64_t h) {
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}

std::uint64_t cellHash(std::uint32_t seed, int cx, int cz, std::uint64_t salt) {
    return mix(mix(static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull + salt) ^
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32 | static_cast<std::uint32_t>(cz)));
}

float unit(std::uint64_t h) { return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0x1000000); }

// Гладкий value-noise 0..1 по целочисленной решётке с шагом `scale` юнитов.
float smoothNoise(std::uint32_t seed, float x, float z, float scale, std::uint64_t salt) {
    const float gx = x / scale, gz = z / scale;
    const int   ix = static_cast<int>(std::floor(gx)), iz = static_cast<int>(std::floor(gz));
    float fx = gx - static_cast<float>(ix), fz = gz - static_cast<float>(iz);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    auto n = [&](int a, int b) { return unit(cellHash(seed, a, b, salt)); };
    const float a = n(ix, iz), b = n(ix + 1, iz), c = n(ix, iz + 1), d = n(ix + 1, iz + 1);
    return (a * (1.0f - fx) + b * fx) * (1.0f - fz) + (c * (1.0f - fx) + d * fx) * fz;
}

// Кандидат в ячейке без проверки рельефа: дёшево (несколько хэшей), нужен и для отрисовки, и для столкновений.
struct Candidate {
    bool         exists = false;
    TreeInstance tree;
};

Candidate candidate(const Terrain& terrain, int cx, int cz) {
    const std::uint32_t seed = terrain.seed();
    Candidate c;
    const float jx = 0.12f + 0.76f * unit(cellHash(seed, cx, cz, 1)), jz = 0.12f + 0.76f * unit(cellHash(seed, cx, cz, 2));
    const float x = (static_cast<float>(cx) + jx) * Trees::kCell, z = (static_cast<float>(cz) + jz) * Trees::kCell;
    const float d = Trees::density(seed, x, z);

    // Биом ячейки (без высоты -- её учтёт suitable): границы слегка "дрожат" на ячейку.
    Climate cl = terrain.climateAt(x, z);
    cl.temperature += (unit(cellHash(seed, cx, cz, 7)) - 0.5f) * 0.08f;
    cl.moisture    += (unit(cellHash(seed, cx, cz, 8)) - 0.5f) * 0.08f;
    const Biome biome = classify(cl);

    // Шанс дерева в ячейке: нет в чистых полях, до ~0.85 в густом лесу; биом масштабирует, а редким биомам
    // добавляет немного одиночек и там, где шум "лесистости" ничего не даёт.
    float mult = 1.0f, floor = 0.0f;
    switch (biome) {
        case Biome::Forest:  mult = 1.0f;  break;
        case Biome::Taiga:   mult = 1.3f;  break;  // хвойный лес гуще, чем уцелеет после остывания на высоте
        case Biome::Jungle:  mult = 1.15f; break;
        case Biome::Plains:  mult = 0.35f; floor = 0.02f; break;
        case Biome::Tundra:  mult = 0.35f; floor = 0.03f; break;
        case Biome::Savanna: mult = 0.30f; floor = 0.04f; break;
        case Biome::Desert:  mult = 0.14f; floor = 0.025f; break;
    }
    const float chance = std::min(0.9f, std::clamp((d - 0.30f) * 2.0f, 0.0f, 0.85f) * mult + floor);
    if (unit(cellHash(seed, cx, cz, 3)) >= chance) return c;

    c.exists = true;
    TreeInstance& t = c.tree;
    t.x       = x;
    t.z       = z;
    t.scale   = 0.8f + 0.5f * unit(cellHash(seed, cx, cz, 4));
    t.yaw     = 360.0f * unit(cellHash(seed, cx, cz, 5));
    t.variant = static_cast<std::uint8_t>(unit(cellHash(seed, cx, cz, 6)) * static_cast<float>(kTreeVariants) * 0.9999f);
    // Вид по биому. Берёзовые рощи -- отдельный шум; в умеренном поясе остальное дубы.
    const float pick      = unit(cellHash(seed, cx, cz, 9));
    const float birchness = smoothNoise(seed, x, z, 45.0f, 11);
    switch (biome) {
        case Biome::Forest:
        case Biome::Plains:  // изредка сосновые боры среди дубрав и берёзовых рощ (на равнинах чаще)
            t.species = pick < (biome == Biome::Plains ? 0.12f : 0.06f) ? TreeSpecies::Pine
                        : birchness > 0.55f                              ? TreeSpecies::Birch
                                                                         : TreeSpecies::Oak;
            break;
        case Biome::Taiga: {
            // Ель -- основа тайги; сосны (боры) там, где суше: чем меньше влажность, тем их больше.
            const float pineShare = std::clamp(0.7f - cl.moisture, 0.05f, 0.4f);
            t.species = pick < 0.10f ? TreeSpecies::Birch : (pick < 0.10f + pineShare ? TreeSpecies::Pine : TreeSpecies::Spruce);
            break;
        }
        case Biome::Tundra:  t.species = pick < 0.2f ? TreeSpecies::Pine : TreeSpecies::Spruce; t.scale *= 0.75f; break;  // карликовые
        case Biome::Savanna: t.species = TreeSpecies::Acacia; break;
        case Biome::Desert:  t.species = TreeSpecies::Cactus; break;
        case Biome::Jungle:
            if (pick < 0.30f) t.species = TreeSpecies::Palm;
            else { t.species = TreeSpecies::Oak; t.scale *= 1.25f; }  // крупные
            break;
    }
    return c;
}

bool isConifer(TreeSpecies s) { return s == TreeSpecies::Spruce || s == TreeSpecies::Pine; }

// Годится ли место по рельефу (текущая высота -- с правками игрока).
bool suitable(const Terrain& terrain, TreeInstance& t) {
    const float h = terrain.heightAt(t.x, t.z);
    if (h < Trees::kMinHeight || h > (isConifer(t.species) ? Trees::kMaxHeightConifer : Trees::kMaxHeight)) return false;
    const float e = 1.5f;
    const float dx = (terrain.heightAt(t.x + e, t.z) - terrain.heightAt(t.x - e, t.z)) / (2.0f * e);
    const float dz = (terrain.heightAt(t.x, t.z + e) - terrain.heightAt(t.x, t.z - e)) / (2.0f * e);
    if (std::hypot(dx, dz) > Trees::kMaxSlope) return false;
    // Берёзы любят пониже: на высоте дубы.
    if (t.species == TreeSpecies::Birch && h > 7.0f) t.species = TreeSpecies::Oak;
    // Высота остужает климат: лёд -- без деревьев, прохладные склоны -- хвойные, жаркие виды выше своей зоны гибнут.
    const float temp = withAltitude(terrain.climateAt(t.x, t.z), h).temperature;
    if (temp < Trees::kMinTemperature) return false;
    if ((t.species == TreeSpecies::Oak || t.species == TreeSpecies::Birch) && temp < 0.24f) t.species = TreeSpecies::Spruce;
    if ((t.species == TreeSpecies::Cactus || t.species == TreeSpecies::Acacia || t.species == TreeSpecies::Palm) && temp < 0.5f) return false;
    return true;
}

}  // namespace

float Trees::density(std::uint32_t seed, float x, float z) {
    // Две октавы: крупные рощи (~80 юнитов) и мелкая "пестрота" внутри.
    return 0.7f * smoothNoise(seed, x, z, 80.0f, 21) + 0.3f * smoothNoise(seed, x, z, 23.0f, 22);
}

std::vector<TreeInstance> Trees::inRect(const Terrain& terrain, float x0, float z0, float x1, float z1, const Blocker* blocked) {
    std::vector<TreeInstance> out;
    const int cx0 = static_cast<int>(std::floor(x0 / kCell)), cx1 = static_cast<int>(std::floor((x1 - 1.0e-4f) / kCell));
    const int cz0 = static_cast<int>(std::floor(z0 / kCell)), cz1 = static_cast<int>(std::floor((z1 - 1.0e-4f) / kCell));
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx) {
            Candidate c = candidate(terrain, cx, cz);
            if (!c.exists) continue;
            TreeInstance& t = c.tree;
            if (t.x < x0 || t.x >= x1 || t.z < z0 || t.z >= z1) continue;  // корень в соседнем прямоугольнике -- его там и нарисуют
            if (!suitable(terrain, t)) continue;
            if (blocked && (*blocked)(t.x, t.z, 2.2f * t.scale)) continue;
            out.push_back(t);
        }
    return out;
}

bool Trees::blocksPoint(const Terrain& terrain, float x, float z, float radius) {
    const int cx = static_cast<int>(std::floor(x / kCell)), cz = static_cast<int>(std::floor(z / kCell));
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            Candidate c = candidate(terrain, cx + dx, cz + dz);
            if (!c.exists) continue;
            TreeInstance& t = c.tree;
            const float r = radius + kTrunkRadius * t.scale * 0.5f;
            if (std::hypot(t.x - x, t.z - z) > r) continue;   // дёшево: сначала расстояние
            if (suitable(terrain, t)) return true;             // и только рядом проверяем рельеф (дорого)
        }
    return false;
}

}  // namespace wsim
