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

Candidate candidate(std::uint32_t seed, int cx, int cz) {
    Candidate c;
    const float jx = 0.12f + 0.76f * unit(cellHash(seed, cx, cz, 1)), jz = 0.12f + 0.76f * unit(cellHash(seed, cx, cz, 2));
    const float x = (static_cast<float>(cx) + jx) * Trees::kCell, z = (static_cast<float>(cz) + jz) * Trees::kCell;
    const float d = Trees::density(seed, x, z);
    // Шанс дерева в ячейке: нет в чистых полях, до ~0.8 в густом лесу.
    const float chance = std::clamp((d - 0.30f) * 2.0f, 0.0f, 0.85f);
    if (unit(cellHash(seed, cx, cz, 3)) >= chance) return c;

    c.exists = true;
    TreeInstance& t = c.tree;
    t.x       = x;
    t.z       = z;
    t.scale   = 0.8f + 0.5f * unit(cellHash(seed, cx, cz, 4));
    t.yaw     = 360.0f * unit(cellHash(seed, cx, cz, 5));
    t.variant = static_cast<std::uint8_t>(unit(cellHash(seed, cx, cz, 6)) * static_cast<float>(kTreeVariants) * 0.9999f);
    // Берёзовые рощи: отдельный шум; дубы -- по умолчанию.
    const float birchness = smoothNoise(seed, x, z, 45.0f, 11);
    t.species = birchness > 0.55f ? TreeSpecies::Birch : TreeSpecies::Oak;
    return c;
}

// Годится ли место по рельефу (текущая высота -- с правками игрока).
bool suitable(const Terrain& terrain, TreeInstance& t) {
    const float h = terrain.heightAt(t.x, t.z);
    if (h < Trees::kMinHeight || h > Trees::kMaxHeight) return false;
    const float e = 1.5f;
    const float dx = (terrain.heightAt(t.x + e, t.z) - terrain.heightAt(t.x - e, t.z)) / (2.0f * e);
    const float dz = (terrain.heightAt(t.x, t.z + e) - terrain.heightAt(t.x, t.z - e)) / (2.0f * e);
    if (std::hypot(dx, dz) > Trees::kMaxSlope) return false;
    // Берёзы любят пониже: на высоте дубы.
    if (t.species == TreeSpecies::Birch && h > 7.0f) t.species = TreeSpecies::Oak;
    return true;
}

}  // namespace

float Trees::density(std::uint32_t seed, float x, float z) {
    // Две октавы: крупные рощи (~80 юнитов) и мелкая "пестрота" внутри.
    return 0.7f * smoothNoise(seed, x, z, 80.0f, 21) + 0.3f * smoothNoise(seed, x, z, 23.0f, 22);
}

std::vector<TreeInstance> Trees::inRect(const Terrain& terrain, float x0, float z0, float x1, float z1, const Blocker* blocked) {
    std::vector<TreeInstance> out;
    const std::uint32_t seed = terrain.seed();
    const int cx0 = static_cast<int>(std::floor(x0 / kCell)), cx1 = static_cast<int>(std::floor((x1 - 1.0e-4f) / kCell));
    const int cz0 = static_cast<int>(std::floor(z0 / kCell)), cz1 = static_cast<int>(std::floor((z1 - 1.0e-4f) / kCell));
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx) {
            Candidate c = candidate(seed, cx, cz);
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
    const std::uint32_t seed = terrain.seed();
    const int cx = static_cast<int>(std::floor(x / kCell)), cz = static_cast<int>(std::floor(z / kCell));
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            Candidate c = candidate(seed, cx + dx, cz + dz);
            if (!c.exists) continue;
            TreeInstance& t = c.tree;
            const float r = radius + kTrunkRadius * t.scale * 0.5f;
            if (std::hypot(t.x - x, t.z - z) > r) continue;   // дёшево: сначала расстояние
            if (suitable(terrain, t)) return true;             // и только рядом проверяем рельеф (дорого)
        }
    return false;
}

}  // namespace wsim
